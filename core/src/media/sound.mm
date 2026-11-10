#include "valtz/media/sound.h"

#include "valtz/base/log.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <optional>
#include <set>

namespace valtz::media {

namespace fs = std::filesystem;

// ---- the plan ---------------------------------------------------------

Json
to_json(const SoundPlan& p)
{
  Json ls = Json::array();
  for (const auto& l : p.layers) {
    Json lj{{"file", l.file.string()},
            {"timing", to_json(l.timing)},
            {"sound", to_json(l.sound)},
            {"follow_speed", l.follow_speed}};
    if (l.room != ReverbRoom{}) {
      lj["room"] = {{"pre_delay", l.room.pre_delay},
                    {"size", l.room.size}};
    }
    if (!l.field.at_rest()) {
      lj["field"] = {{"bass_mono", l.field.bass_mono},
                     {"crossfeed", l.field.crossfeed}};
    }
    if (l.tail > 0) {
      lj["tail"] = l.tail;
    }
    ls.push_back(std::move(lj));
  }
  Json ts = Json::array();
  for (const auto& t : p.transitions) {
    ts.push_back({{"from", t.from}, {"to", t.to},
                  {"kind", std::string(to_string(t.kind))}});
  }
  return {{"layers", ls}, {"transitions", ts}, {"seconds", p.seconds}};
}

SoundPlan
sound_plan_from_json(const Json& j)
{
  SoundPlan p;
  for (const auto& l : jget(j, "layers", Json::array())) {
    SoundLayer sl;
    sl.file = jget<std::string>(l, "file", "");
    sl.timing = layer_timing_from_json(jget(l, "timing", Json()));
    sl.sound = keyed_sound_from_json(jget(l, "sound", Json::object()));
    sl.follow_speed = jget(l, "follow_speed", false);
    const Json room = jget(l, "room", Json::object());
    sl.room.pre_delay = std::clamp(jget(room, "pre_delay", sl.room.pre_delay),
                                   0.0, kMaxPreDelay);
    sl.room.size = std::clamp(jget(room, "size", sl.room.size), 0.0, 1.0);
    const Json field = jget(l, "field", Json::object());
    sl.field.bass_mono = std::clamp(jget(field, "bass_mono", 0.0), 0.0,
                                    kMaxBassMono);
    sl.field.crossfeed = std::clamp(jget(field, "crossfeed", 0.0), 0.0, 1.0);
    sl.tail = std::max(0.0, jget(l, "tail", 0.0));
    p.layers.push_back(std::move(sl));
  }
  for (const auto& t : jget(j, "transitions", Json::array())) {
    // Its kind; written before kinds, a dissolve or a cut.
    const auto kind = transition_kind(jget<std::string>(
        t, "kind", jget(t, "dissolve", false) ? "dissolve" : "cut"));
    p.transitions.push_back({jget<std::size_t>(t, "from", 0),
                              jget<std::size_t>(t, "to", 0),
                              kind.value_or(TransitionSpec::Kind::Cut)});
  }
  p.seconds = std::max(0.0, jget(j, "seconds", 0.0));
  return p;
}

namespace {

// A file's sound track, loaded; nil without one.
AVAssetTrack*
audio_track(AVAsset* asset)
{
  __block AVAssetTrack* track = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [asset loadTracksWithMediaType:AVMediaTypeAudio
               completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*) {
                 track = t.firstObject;
                 dispatch_semaphore_signal(sem);
               }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  return track;
}

// How long a track runs, loaded.
double
track_seconds(AVAssetTrack* track)
{
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [track loadValuesAsynchronouslyForKeys:@[ @"timeRange" ]
                       completionHandler:^{
                         dispatch_semaphore_signal(sem);
                       }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  const CMTimeRange r = track.timeRange;
  return CMTIME_IS_NUMERIC(r.duration) ? CMTimeGetSeconds(r.duration) : 0;
}

double
fps_of(Rational r)
{
  const double f = r.to_double();
  return f > 0 ? f : 24.0;
}

std::vector<LayerTiming>
timings_of(const SoundPlan& p)
{
  std::vector<LayerTiming> ts;
  for (const auto& l : p.layers) {
    ts.push_back(l.timing);
  }
  return ts;
}

double
gain_at(const SoundPlan& p, const std::vector<LayerTiming>& ts,
        std::size_t i, double t)
{
  const LayerWeights w = layer_weights(ts, p.transitions, t);
  const LayerTiming& lt = ts[i];
  const double local = std::max(0.0, t - lt.start) * fps_of(lt.rate);
  const double v = p.layers[i].sound.keys.empty()
                       ? 1.0 : p.layers[i].sound.at(local).volume;
  return std::clamp(v, 0.0, kMaxVolume) * w.gain[i];
}

bool
shifts_pitch(const SoundLayer& l)
{
  return std::ranges::any_of(l.sound.keys, [](const auto& key) {
           return key.value.pitch != 0;
         }) ||
         (l.follow_speed && !l.timing.speed.identity());
}

// Layer `l`'s equalizer sets a band in some key.
bool
equalizes(const SoundLayer& l)
{
  return std::ranges::any_of(l.sound.keys, [](const auto& key) {
    return !key.value.flat();
  });
}


// Something of layer `l` goes into its room in some key.
bool
reverberates(const SoundLayer& l)
{
  return std::ranges::any_of(l.sound.keys, [](const auto& key) {
    return key.value.reverberates();
  });
}

// Layer `l` is put somewhere else in the field: panned, widened or
// narrowed, set farther, its lows made mono or crossfed.
bool
places(const SoundLayer& l)
{
  return !l.field.at_rest() ||
         std::ranges::any_of(l.sound.keys, [](const auto& key) {
           return !key.value.placed_at_rest();
         });
}

// Where a reverberating layer's shaped span ends in its source: its
// out mark, or sooner where the layer is cut short (its room's tail
// rings on from where it stops sounding).
double
shaped_out(const SoundLayer& l)
{
  const LayerTiming& t = l.timing;
  if (!reverberates(l) || !std::isfinite(t.length)) {
    return t.out;
  }
  return std::min(t.out, t.source_at_local(t.length));
}

// Its shaped span's tail, after the layer's end at the speed it ended
// at, up to `end`; none where it has none, or the layer runs on to
// `end`.
std::optional<TimeSegment>
tail_segment(const SoundLayer& l, double end)
{
  const LayerTiming& t = l.timing;
  const double b = t.end();
  if (!(l.tail > 0) || !std::isfinite(b) || !(b < end) || !(b > t.start)) {
    return std::nullopt;
  }
  const double s0 = t.source_at(b);
  const double rest = std::isfinite(t.out) ? t.out + l.tail - s0 : l.tail;
  const double speed = std::max(kMinSpeed, t.speed_at_local(b - t.start));
  if (!(rest > 0)) {
    return std::nullopt;
  }
  const double len = std::min(rest / speed, end - b);
  return TimeSegment{b, len, s0, len * speed};
}


// Layer `l`'s pitch, in semitones, `u` seconds after its start: its keys',
// and -- following its speed -- the octaves the speed makes.
double
pitch_at(const SoundLayer& l, double u)
{
  double semis = l.sound.at(u * fps_of(l.timing.rate)).pitch;
  if (l.follow_speed) {
    semis += 12.0 * std::log2(l.timing.speed_at_local(u));
  }
  return std::clamp(semis, -kMaxPitch, kMaxPitch);
}

}

bool
has_sound(const fs::path& file)
{
  if (file.empty()) {
    return false;
  }
  @autoreleasepool {
    AVURLAsset* a = [AVURLAsset
        URLAssetWithURL:[NSURL fileURLWithPath:@(file.c_str())]
                options:nil];
    return audio_track(a) != nil;
  }
}

std::vector<TimeSegment>
time_segments(const LayerTiming& t, double end, double step)
{
  std::vector<TimeSegment> out;
  const double a = t.start;
  const double b = std::min(t.end(), end);
  if (!(b > a)) {
    return out;
  }
  if (t.constant_speed()) {
    const double s0 = t.source_at(a);
    out.push_back({a, b - a, s0, t.source_at(b) - s0});
    return out;
  }
  step = std::max(step, 1e-3);
  for (double x = a; x < b - 1e-9;) {
    const double y = std::min(b, x + step);
    const double s0 = t.source_at(x);
    const double s1 = t.source_at(y);
    TimeSegment seg{x, y - x, s0, s1 - s0};
    // A run at one rate is one segment.
    if (!out.empty()) {
      TimeSegment& last = out.back();
      const double r0 = last.source_length / last.length;
      const double r1 = seg.source_length / seg.length;
      if (std::abs(r0 - r1) < 1e-6 &&
          std::abs(last.source + last.source_length - s0) < 1e-6) {
        last.length += seg.length;
        last.source_length += seg.source_length;
        x = y;
        continue;
      }
    }
    out.push_back(seg);
    x = y;
  }
  return out;
}

std::vector<std::pair<double, double>>
volume_points(const SoundPlan& p, std::size_t i, double step)
{
  std::vector<std::pair<double, double>> out;
  if (i >= p.layers.size()) {
    return out;
  }
  const auto ts = timings_of(p);
  const LayerTiming& lt = ts[i];
  const double a = lt.start;
  const double b = std::min(lt.end(), p.seconds > 0 ? p.seconds : lt.end());
  if (!(b > a) || !std::isfinite(b)) {
    return out;
  }
  std::set<double> at = {a, b};
  for (double x : weight_breaks(ts, p.transitions, i)) {
    if (x > a && x < b) {
      at.insert(x);
    }
  }
  const double fps = fps_of(lt.rate);
  for (const auto& k : p.layers[i].sound.keys) {
    const double x = a + static_cast<double>(k.frame) / fps;
    if (x > a && x < b) {
      at.insert(x);
    }
  }
  // Ramps sampled; flat stretches left as they are. A step (a cut) is a
  // point just before it and one at it.
  const std::vector<double> marks(at.begin(), at.end());
  constexpr double kJust = 1e-3;
  auto g = [&](double t) { return gain_at(p, ts, i, std::min(t, b - 1e-9)); };
  for (std::size_t k = 0; k + 1 < marks.size(); ++k) {
    const double x = marks[k];
    const double y = marks[k + 1];
    out.push_back({x, g(x)});
    const double gx = g(x);
    const double gm = g(0.5 * (x + y));
    const double gy = g(y - kJust);
    if (gx != gm || gm != gy) {
      for (double t = x + step; t < y - kJust; t += step) {
        out.push_back({t, g(t)});
      }
    }
    if (y - kJust > x) {
      out.push_back({y - kJust, gy});
    }
  }
  out.push_back({b, g(b)});
  // Its room ringing on: the gain it ended with.
  if (auto tail = tail_segment(p.layers[i],
                               p.seconds > 0 ? p.seconds : kForever)) {
    out.push_back({tail->at + tail->length, g(b)});
  }
  return out;
}

bool
shapes(const SoundLayer& l)
{
  return shifts_pitch(l) || equalizes(l) || reverberates(l) || places(l);
}

double
reverb_tail(const SoundLayer& l)
{
  if (!reverberates(l)) {
    return 0;
  }
  double decay = 0;
  for (const auto& key : l.sound.keys) {
    decay = std::max(decay, key.value.reverb_decay);
  }
  double fastest = l.timing.speed.keys.empty() ? 1.0 : 0.0;
  for (const auto& key : l.timing.speed.keys) {
    fastest = std::max(fastest, key.value.rate);
  }
  return std::min(kMaxReverbLength,
                  (std::clamp(l.room.pre_delay, 0.0, kMaxPreDelay) + decay) *
                      fastest);
}

std::vector<TimeSegment>
sound_segments(const SoundLayer& l, double end, double step)
{
  std::vector<TimeSegment> out = time_segments(l.timing, end, step);
  if (auto tail = tail_segment(l, end)) {
    out.push_back(*tail);
  }
  return out;
}

bool
needs_render(const SoundPlan& p)
{
  return std::ranges::any_of(p.layers, [](const SoundLayer& l) {
    return shapes(l);
  });
}

namespace {

// The standard format the mixer and the offline pass work in.
AVAudioFormat*
mix_format()
{
  return [[[AVAudioFormat alloc] initStandardFormatWithSampleRate:kMixRate
                                                         channels:2]
      autorelease];
}

// `file`'s sound from `in` to `out` seconds, as float PCM at the mix's
// rate: what the offline pass shapes.
Result<AVAudioPCMBuffer*>
read_span(const fs::path& file, double in, double out)
{
  AVURLAsset* asset = [AVURLAsset
      URLAssetWithURL:[NSURL fileURLWithPath:@(file.c_str())]
              options:nil];
  AVAssetTrack* track = audio_track(asset);
  if (!track) {
    return make_error(Code::Corrupt, std::format(
        "{} has no sound", file.filename().string()));
  }
  NSError* err = nil;
  AVAssetReader* r = [AVAssetReader assetReaderWithAsset:asset error:&err];
  if (!r) {
    return make_error(Code::Io, std::format("cannot read {}",
                                            file.string()));
  }
  AVAssetReaderTrackOutput* o = [AVAssetReaderTrackOutput
      assetReaderTrackOutputWithTrack:track
                       outputSettings:@{
    AVFormatIDKey : @(kAudioFormatLinearPCM),
    AVSampleRateKey : @(kMixRate),
    AVNumberOfChannelsKey : @2,
    AVLinearPCMBitDepthKey : @32,
    AVLinearPCMIsFloatKey : @YES,
    AVLinearPCMIsNonInterleaved : @YES,
    AVLinearPCMIsBigEndianKey : @NO,
  }];
  [r addOutput:o];
  const double dur = std::isfinite(out) ? out - in
                                        : track_seconds(track) - in;
  r.timeRange = CMTimeRangeMake(CMTimeMakeWithSeconds(in, kMixRate),
                                CMTimeMakeWithSeconds(std::max(0.0, dur),
                                                      kMixRate));
  if (![r startReading]) {
    return make_error(Code::Io, std::format("cannot read {}",
                                            file.string()));
  }
  const auto cap = static_cast<AVAudioFrameCount>(
      std::ceil(std::max(0.0, dur) * kMixRate) + kMixRate);
  AVAudioPCMBuffer* buf = [[[AVAudioPCMBuffer alloc]
      initWithPCMFormat:mix_format() frameCapacity:cap] autorelease];
  while (CMSampleBufferRef sb = [o copyNextSampleBuffer]) {
    const auto n = static_cast<AVAudioFrameCount>(
        CMSampleBufferGetNumSamples(sb));
    const AVAudioFrameCount room = buf.frameCapacity - buf.frameLength;
    const AVAudioFrameCount take = std::min(n, room);
    AudioBufferList* abl = nullptr;
    size_t size = 0;
    CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
        sb, &size, nullptr, 0, nullptr, nullptr, 0, nullptr);
    abl = static_cast<AudioBufferList*>(malloc(size));
    CMBlockBufferRef block = nullptr;
    if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
            sb, nullptr, abl, size, nullptr, nullptr,
            kCMSampleBufferFlag_AudioBufferList_Assure16ByteAlignment,
            &block) == noErr) {
      for (UInt32 c = 0; c < 2 && c < abl->mNumberBuffers; ++c) {
        std::copy_n(static_cast<const float*>(abl->mBuffers[c].mData), take,
                    buf.floatChannelData[c] + buf.frameLength);
      }
      buf.frameLength += take;
    }
    if (block) {
      CFRelease(block);
    }
    free(abl);
    CFRelease(sb);
  }
  return buf;
}

// The voices layer `l`'s room speaks with along its span (`span` source
// seconds from its start) -- at its ends, and between where its keys or
// its speed change it more than an ear would let pass a straight mix of
// the two either side -- 16 at most. Its times are the source's: a decay
// stretched later by a speed, made longer by it first.
struct VoiceAt {
  double      at = 0;
  ReverbVoice voice;
};

std::vector<VoiceAt>
reverb_voices(const SoundLayer& l, double span)
{
  const LayerTiming& t = l.timing;
  const double fps = fps_of(t.rate);
  auto voice = [&](double s) {
    double u = t.local_at_source(t.in + s);
    if (!std::isfinite(u)) {
      u = 0;
    }
    const Sound v = l.sound.at(u * fps);
    const double speed = t.speed_at_local(u);
    return ReverbVoice{v.reverb_decay * speed,
                       std::clamp(v.reverb_damping, 0.0, 1.0),
                       std::clamp(l.room.pre_delay, 0.0, kMaxPreDelay) *
                           speed};
  };
  span = std::max(0.0, std::isfinite(span) ? span : 0.0);
  std::vector<double> times;
  const int n = std::clamp(static_cast<int>(std::ceil(span / 0.5)), 1, 256);
  for (int i = 0; i <= n; ++i) {
    times.push_back(span * i / n);
  }
  auto add_key = [&](std::int64_t frame) {
    const double s = t.source_at_local(static_cast<double>(frame) / fps) -
                     t.in;
    if (s > 0 && s < span) {
      times.push_back(s);
    }
  };
  for (const auto& k : l.sound.keys) {
    add_key(k.frame);
  }
  for (const auto& k : t.speed.keys) {
    add_key(k.frame);
  }
  std::ranges::sort(times);
  times.erase(std::unique(times.begin(), times.end(),
                          [](double a, double b) { return b - a < 1e-3; }),
              times.end());
  std::vector<VoiceAt> all;
  for (double s : times) {
    all.push_back({s, voice(s)});
  }
  // How far a voice is from the two kept either side, mixed where it
  // lies: 1 the most an ear lets pass (5% of the decay, 0.03 of the
  // damping, 2 ms of pre-delay).
  auto off = [&](std::size_t i, std::size_t a, std::size_t b) {
    const double w = (all[i].at - all[a].at) /
                     std::max(1e-9, all[b].at - all[a].at);
    const ReverbVoice& x = all[i].voice;
    const ReverbVoice& va = all[a].voice;
    const ReverbVoice& vb = all[b].voice;
    const double decay = va.decay + (vb.decay - va.decay) * w;
    const double damping = va.damping + (vb.damping - va.damping) * w;
    const double pre = va.pre_delay + (vb.pre_delay - va.pre_delay) * w;
    return std::max({std::abs(std::log(x.decay / std::max(1e-6, decay))) /
                         0.05,
                     std::abs(x.damping - damping) / 0.03,
                     std::abs(x.pre_delay - pre) / 0.002});
  };
  std::vector<std::size_t> kept = {0};
  if (all.size() > 1) {
    kept.push_back(all.size() - 1);
  }
  while (kept.size() < 16) {
    double worst = 1;
    std::size_t at = 0, slot = 0;
    for (std::size_t j = 0; j + 1 < kept.size(); ++j) {
      for (std::size_t i = kept[j] + 1; i < kept[j + 1]; ++i) {
        const double d = off(i, kept[j], kept[j + 1]);
        if (d > worst) {
          worst = d;
          at = i;
          slot = j + 1;
        }
      }
    }
    if (at == 0) {
      break;
    }
    kept.insert(kept.begin() + static_cast<std::ptrdiff_t>(slot), at);
  }
  std::vector<VoiceAt> out;
  for (std::size_t i : kept) {
    out.push_back(all[i]);
  }
  // One voice throughout: one response.
  if (out.size() == 2 && out[0].voice == out[1].voice) {
    out.pop_back();
  }
  return out;
}

// `left` and `right` (`n` frames) written to `file` (its processing
// format the mix's), a piece at a time.
Status
write_planar(AVAudioFile* file, const float* left, const float* right,
             std::size_t n, const fs::path& out)
{
  constexpr AVAudioFrameCount kPiece = 65536;
  AVAudioPCMBuffer* buf = [[[AVAudioPCMBuffer alloc]
      initWithPCMFormat:file.processingFormat frameCapacity:kPiece]
      autorelease];
  NSError* err = nil;
  for (std::size_t i = 0; i < n; i += kPiece) {
    const auto k = static_cast<AVAudioFrameCount>(
        std::min<std::size_t>(kPiece, n - i));
    std::copy_n(left + i, k, buf.floatChannelData[0]);
    std::copy_n(right + i, k, buf.floatChannelData[1]);
    buf.frameLength = k;
    if (![file writeFromBuffer:buf error:&err]) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              out.string()));
    }
  }
  return ok_status();
}

// One of layer `l`'s keyed numbers along its shaped span, a value a frame
// for `n` frames (the span and its tail): its keys read every 64 frames,
// straight between -- past the span's end, its last.
std::vector<float>
along(const SoundLayer& l, std::size_t n, double Sound::*number)
{
  std::vector<float> out(n);
  const LayerTiming& t = l.timing;
  const double fps = fps_of(t.rate);
  auto at = [&](std::size_t i) {
    double u = t.local_at_source(t.in + static_cast<double>(i) / kMixRate);
    if (!std::isfinite(u)) {
      u = std::isfinite(t.length) ? t.length : 0;
    }
    return l.sound.keys.empty() ? Sound{}.*number
                                : l.sound.at(u * fps).*number;
  };
  constexpr std::size_t kStep = 64;
  double v0 = at(0);
  for (std::size_t i = 0; i < n; i += kStep) {
    const std::size_t e = std::min(n, i + kStep);
    const double v1 = at(e);
    for (std::size_t j = i; j < e; ++j) {
      out[j] = static_cast<float>(
          v0 + (v1 - v0) * static_cast<double>(j - i) / kStep);
    }
    v0 = v1;
  }
  return out;
}

// The room laid on `dry` (`left`, `right`: the span, pitch and equalizer
// already on it): what its keys' mix sends of it -- the sine of a
// quarter turn by the mix, the dry the cosine: as loud together at any
// mix -- convolved with the responses its keys and speed give along it
// (media/reverb.h), the wet added. Its tail after it: `tail` frames.
Status
add_room(const SoundLayer& l, std::vector<float>& left,
         std::vector<float>& right, std::size_t tail, ReverbStats* stats)
{
  const std::size_t n = left.size();
  const LayerTiming& t = l.timing;
  const double fps = fps_of(t.rate);
  // The mix every 64 frames, straight between.
  constexpr std::size_t kStep = 64;
  auto mix_at = [&](std::size_t i) {
    double u = t.local_at_source(t.in + static_cast<double>(i) / kMixRate);
    if (!std::isfinite(u)) {
      u = 0;
    }
    return std::clamp(l.sound.at(u * fps).reverb_mix, 0.0, 1.0);
  };
  std::vector<float> send(n);
  double m0 = mix_at(0);
  for (std::size_t i = 0; i < n; i += kStep) {
    const std::size_t e = std::min(n, i + kStep);
    const double m1 = mix_at(e);
    for (std::size_t j = i; j < e; ++j) {
      const double m = m0 + (m1 - m0) * static_cast<double>(j - i) / kStep;
      const double quarter = 0.5 * std::numbers::pi * m;
      send[j] = static_cast<float>(0.5 * (left[j] + right[j]) *
                                   std::sin(quarter));
      const auto dry = static_cast<float>(std::cos(quarter));
      left[j] *= dry;
      right[j] *= dry;
    }
    m0 = m1;
  }
  const auto voices = reverb_voices(l, static_cast<double>(n) / kMixRate);
  std::vector<std::array<std::vector<float>, 2>> responses;
  for (const auto& v : voices) {
    responses.push_back({reverb_impulse(v.voice, l.room.size, 0, kMixRate),
                         reverb_impulse(v.voice, l.room.size, 1, kMixRate)});
  }
  // Each send block: the voices either side of its middle, mixed.
  std::vector<ReverbPick> picks;
  for (std::size_t k = 0; k * kReverbBlock < std::max<std::size_t>(1, n);
       ++k) {
    const double at = (static_cast<double>(k) + 0.5) * kReverbBlock /
                      kMixRate;
    ReverbPick pk;
    for (std::size_t i = 0; i + 1 < voices.size(); ++i) {
      if (at >= voices[i].at) {
        pk.a = static_cast<std::uint32_t>(i);
        pk.b = static_cast<std::uint32_t>(i + 1);
        pk.w = static_cast<float>(std::clamp(
            (at - voices[i].at) /
                std::max(1e-9, voices[i + 1].at - voices[i].at),
            0.0, 1.0));
      }
    }
    if (voices.size() > 1 && at >= voices.back().at) {
      pk = {static_cast<std::uint32_t>(voices.size() - 1),
            static_cast<std::uint32_t>(voices.size() - 1), 0};
    }
    picks.push_back(pk);
  }
  VALTZ_ASSIGN(auto wet, convolve_reverb(send.data(), n, responses, picks,
                                         n + tail, true, stats));
  left.resize(n + tail, 0.0f);
  right.resize(n + tail, 0.0f);
  for (std::size_t i = 0; i < n + tail; ++i) {
    left[i] += wet[0][i];
    right[i] += wet[1][i];
  }
  return ok_status();
}

// Layer `l`'s span with its pitch shifted, its equalizer and its room on
// it -- each block at what its keys give where it falls -- written to
// `out` (CAF), its room's tail after it. Its timing then starts the span
// from 0.
Status
render_shaped(const SoundLayer& l, const fs::path& out, ReverbStats* stats)
{
  const bool room = reverberates(l);
  // Anything after the equalizer: the span in memory, worked on there.
  const bool after = room || places(l);
  VALTZ_ASSIGN(AVAudioPCMBuffer* src,
               read_span(l.file, l.timing.in, shaped_out(l)));
  std::error_code ec;
  fs::remove(out, ec);
  // The file interleaved (a CAF holds no other), written from the
  // engine's planar blocks.
  NSDictionary* settings = @{
    AVFormatIDKey : @(kAudioFormatLinearPCM),
    AVSampleRateKey : @(kMixRate),
    AVNumberOfChannelsKey : @2,
    AVLinearPCMBitDepthKey : @32,
    AVLinearPCMIsFloatKey : @YES,
    AVLinearPCMIsNonInterleaved : @NO,
    AVLinearPCMIsBigEndianKey : @NO,
  };
  NSError* err = nil;
  AVAudioFile* file = [[[AVAudioFile alloc]
      initForWriting:[NSURL fileURLWithPath:@(out.c_str())]
            settings:settings
        commonFormat:AVAudioPCMFormatFloat32
         interleaved:NO
               error:&err] autorelease];
  if (!file) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            out.string()));
  }
  const AVAudioFramePosition total = src.frameLength;
  // The room alone: the span as read, into it.
  const bool pitch = shifts_pitch(l);
  const bool bands = equalizes(l);
  std::vector<float> left, right;
  if (after) {
    left.reserve(static_cast<std::size_t>(total));
    right.reserve(static_cast<std::size_t>(total));
  }
  if (after && !pitch && !bands) {
    left.assign(src.floatChannelData[0], src.floatChannelData[0] + total);
    right.assign(src.floatChannelData[1], src.floatChannelData[1] + total);
  } else {
    AVAudioEngine* engine = [[[AVAudioEngine alloc] init] autorelease];
    AVAudioPlayerNode* player =
        [[[AVAudioPlayerNode alloc] init] autorelease];
    AVAudioUnitTimePitch* tp =
        [[[AVAudioUnitTimePitch alloc] init] autorelease];
    AVAudioUnitEQ* eq = [[[AVAudioUnitEQ alloc]
        initWithNumberOfBands:kEqBands] autorelease];
    // What does nothing here is passed by as it is.
    tp.bypass = !pitch;
    eq.bypass = !bands;
    eq.globalGain = 0;
    for (std::size_t i = 0; i < kEqFrequencies.size(); ++i) {
      AVAudioUnitEQFilterParameters* b = eq.bands[i];
      b.filterType = AVAudioUnitEQFilterTypeParametric;
      b.frequency = static_cast<float>(kEqFrequencies[i]);
      b.bandwidth = 1;   // octaves
      b.gain = 0;
      b.bypass = NO;
    }
    [engine attachNode:player];
    [engine attachNode:tp];
    [engine attachNode:eq];
    AVAudioFormat* fmt = mix_format();
    [engine connect:player to:tp format:fmt];
    [engine connect:tp to:eq format:fmt];
    [engine connect:eq to:engine.mainMixerNode format:fmt];
    constexpr AVAudioFrameCount kBlock = 2048;
    if (![engine
            enableManualRenderingMode:AVAudioEngineManualRenderingModeOffline
                               format:fmt
                    maximumFrameCount:kBlock
                                error:&err] ||
        ![engine startAndReturnError:&err]) {
      return make_error(Code::Internal, std::format(
          "cannot shift the pitch: {}",
          err ? err.localizedDescription.UTF8String : "?"));
    }
    [player scheduleBuffer:src completionHandler:nil];
    [player play];
    AVAudioPCMBuffer* block = [[[AVAudioPCMBuffer alloc]
        initWithPCMFormat:engine.manualRenderingFormat
            frameCapacity:kBlock] autorelease];
    Status st = ok_status();
    while (engine.manualRenderingSampleTime < total) {
      const double s = l.timing.in +
          static_cast<double>(engine.manualRenderingSampleTime) / kMixRate;
      const double u = l.timing.local_at_source(s);
      tp.pitch = static_cast<float>(pitch_at(l, u) * 100.0);
      if (bands) {
        const auto g = l.sound.at(u * fps_of(l.timing.rate)).eq;
        for (std::size_t i = 0; i < g.size(); ++i) {
          eq.bands[i].gain = static_cast<float>(
              std::clamp(g[i], -kMaxEqGain, kMaxEqGain));
        }
      }
      const auto n = static_cast<AVAudioFrameCount>(std::min<std::int64_t>(
          kBlock, total - engine.manualRenderingSampleTime));
      const AVAudioEngineManualRenderingStatus rs =
          [engine renderOffline:n toBuffer:block error:&err];
      if (rs != AVAudioEngineManualRenderingStatusSuccess) {
        st = make_error(Code::Internal, std::format(
            "cannot shift the pitch: {}",
            err ? err.localizedDescription.UTF8String : "?"));
        break;
      }
      if (after) {
        const AVAudioFrameCount k = block.frameLength;
        left.insert(left.end(), block.floatChannelData[0],
                    block.floatChannelData[0] + k);
        right.insert(right.end(), block.floatChannelData[1],
                     block.floatChannelData[1] + k);
      } else if (![file writeFromBuffer:block error:&err]) {
        st = make_error(Code::Io, std::format("cannot write {}",
                                              out.string()));
        break;
      }
    }
    [player stop];
    [engine stop];
    VALTZ_TRY(st);
  }
  if (!after) {
    return ok_status();
  }
  // Panned, into the room, then placed: as wide, as far, its lows in the
  // middle, crossfed.
  pan_sound(left, right, along(l, left.size(), &Sound::pan));
  if (room) {
    const auto tail = static_cast<std::size_t>(
        std::llround(reverb_tail(l) * kMixRate));
    VALTZ_TRY(add_room(l, left, right, tail, stats));
  }
  widen_sound(left, right, along(l, left.size(), &Sound::width));
  place_sound_far(left, right, along(l, left.size(), &Sound::distance),
                  kMixRate);
  mono_bass(left, right, l.field.bass_mono, kMixRate);
  crossfeed_sound(left, right, l.field.crossfeed, kMixRate);
  return write_planar(file, left.data(), right.data(), left.size(), out);
}

}

Status
shape_sound(const SoundLayer& l, const fs::path& out, ReverbStats* stats)
{
  @autoreleasepool {
    return render_shaped(l, out, stats);
  }
}

SoundLayer
as_shaped(SoundLayer l, const fs::path& file)
{
  const double span = shaped_out(l) - l.timing.in;
  l.tail = reverb_tail(l);
  l.file = file;
  l.timing.out = std::isfinite(span) ? span : kForever;
  // Keys as they were, read along the shaped span's time (from its
  // start, as before); what they shaped is in the file now.
  l.timing.in = 0;
  for (auto& key : l.sound.keys) {
    key.value.pitch = 0;
    key.value.eq = {};
    key.value.reverb_mix = 0;
    key.value.reverb_decay = kReverbDecay;
    key.value.reverb_damping = kReverbDamping;
    key.value.pan = 0;
    key.value.width = 1;
    key.value.distance = 1;
  }
  l.follow_speed = false;
  l.field = {};
  return l;
}

Result<bool>
mix_sound(const SoundPlan& plan, const fs::path& out)
{
  @autoreleasepool {
    AVMutableComposition* comp = [AVMutableComposition composition];
    NSMutableArray<AVAudioMixInputParameters*>* params =
        [NSMutableArray array];
    NSMutableArray<AVAssetTrack*>* tracks = [NSMutableArray array];
    std::vector<fs::path> scratch;
    const double end = plan.seconds;
    for (std::size_t i = 0; i < plan.layers.size(); ++i) {
      SoundLayer l = plan.layers[i];
      if (l.file.empty() || !has_sound(l.file)) {
        continue;
      }
      if (shapes(l)) {
        // The span shaped first; then it is a sound from 0.
        const fs::path shifted = fs::temp_directory_path() /
            std::format("valtz-shaped-{}-{}.caf", ::getpid(),
                        reinterpret_cast<std::uintptr_t>(&l) + i);
        VALTZ_TRY(render_shaped(l, shifted, nullptr));
        scratch.push_back(shifted);
        l = as_shaped(std::move(l), shifted);
      }
      NSDictionary* precise =
          @{AVURLAssetPreferPreciseDurationAndTimingKey : @YES};
      AVURLAsset* asset = [AVURLAsset
          URLAssetWithURL:[NSURL fileURLWithPath:@(l.file.c_str())]
                  options:precise];
      AVAssetTrack* src = audio_track(asset);
      if (!src) {
        continue;
      }
      AVMutableCompositionTrack* t = [comp
          addMutableTrackWithMediaType:AVMediaTypeAudio
                      preferredTrackID:kCMPersistentTrackID_Invalid];
      const double own = track_seconds(src);
      bool any = false;
      for (const auto& seg : sound_segments(l, end, 1.0 / 24)) {
        // Never past the source's own end.
        const double sl = std::min(seg.source_length, own - seg.source);
        if (sl <= 0) {
          continue;
        }
        const double len = seg.length * sl / seg.source_length;
        const CMTimeScale ts = kMixRate;
        NSError* err = nil;
        const CMTimeRange from = CMTimeRangeMake(
            CMTimeMakeWithSeconds(seg.source, ts),
            CMTimeMakeWithSeconds(sl, ts));
        const CMTime at = CMTimeMakeWithSeconds(seg.at, ts);
        if (![t insertTimeRange:from ofTrack:src atTime:at error:&err]) {
          VALTZ_LOG_WARN("media", "a sound's span would not go in: {}",
                         err ? err.localizedDescription.UTF8String : "?");
          continue;
        }
        if (std::abs(len - sl) > 1e-6) {
          [t scaleTimeRange:CMTimeRangeMake(at, CMTimeMakeWithSeconds(sl, ts))
                 toDuration:CMTimeMakeWithSeconds(len, ts)];
        }
        any = true;
      }
      if (!any) {
        [comp removeTrack:t];
        continue;
      }
      SoundPlan one = plan;
      one.layers[i] = l;
      AVMutableAudioMixInputParameters* p =
          [AVMutableAudioMixInputParameters
              audioMixInputParametersWithTrack:t];
      const auto pts = volume_points(one, i);
      for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
        const double a = pts[k].first;
        const double b = pts[k + 1].first;
        if (b <= a) {
          continue;
        }
        [p setVolumeRampFromStartVolume:static_cast<float>(pts[k].second)
                            toEndVolume:static_cast<float>(pts[k + 1].second)
                              timeRange:CMTimeRangeMake(
                                  CMTimeMakeWithSeconds(a, kMixRate),
                                  CMTimeMakeWithSeconds(b - a, kMixRate))];
      }
      [params addObject:p];
      [tracks addObject:t];
    }
    auto cleanup = [&] {
      std::error_code ec;
      for (const auto& f : scratch) {
        fs::remove(f, ec);
      }
    };
    if (tracks.count == 0) {
      cleanup();
      return false;
    }
    AVMutableAudioMix* mix = [AVMutableAudioMix audioMix];
    mix.inputParameters = params;
    NSError* err = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:comp
                                                          error:&err];
    if (!reader) {
      cleanup();
      return make_error(Code::Internal, "cannot mix the sound");
    }
    AVAssetReaderAudioMixOutput* o = [AVAssetReaderAudioMixOutput
        assetReaderAudioMixOutputWithAudioTracks:tracks
                                   audioSettings:@{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVSampleRateKey : @(kMixRate),
      AVNumberOfChannelsKey : @2,
      AVLinearPCMBitDepthKey : @32,
      AVLinearPCMIsFloatKey : @YES,
      AVLinearPCMIsNonInterleaved : @NO,
      AVLinearPCMIsBigEndianKey : @NO,
    }];
    o.audioMix = mix;
    o.audioTimePitchAlgorithm = AVAudioTimePitchAlgorithmSpectral;
    [reader addOutput:o];
    if (end > 0) {
      reader.timeRange = CMTimeRangeMake(
          kCMTimeZero, CMTimeMakeWithSeconds(end, kMixRate));
    }
    if (![reader startReading]) {
      cleanup();
      return make_error(Code::Internal, std::format(
          "cannot mix the sound: {}",
          reader.error ? reader.error.localizedDescription.UTF8String : "?"));
    }
    // 24-bit WAV out; float in.
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    fs::remove(out, ec);
    AudioStreamBasicDescription file_fmt{};
    file_fmt.mSampleRate = kMixRate;
    file_fmt.mFormatID = kAudioFormatLinearPCM;
    file_fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger |
                            kAudioFormatFlagIsPacked;
    file_fmt.mChannelsPerFrame = 2;
    file_fmt.mBitsPerChannel = 24;
    file_fmt.mBytesPerFrame = 6;
    file_fmt.mFramesPerPacket = 1;
    file_fmt.mBytesPerPacket = 6;
    AudioStreamBasicDescription client{};
    client.mSampleRate = kMixRate;
    client.mFormatID = kAudioFormatLinearPCM;
    client.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    client.mChannelsPerFrame = 2;
    client.mBitsPerChannel = 32;
    client.mBytesPerFrame = 8;
    client.mFramesPerPacket = 1;
    client.mBytesPerPacket = 8;
    ExtAudioFileRef ef = nullptr;
    NSURL* url = [NSURL fileURLWithPath:@(out.c_str())];
    if (ExtAudioFileCreateWithURL((__bridge CFURLRef)url, kAudioFileWAVEType,
                                  &file_fmt, nullptr,
                                  kAudioFileFlags_EraseFile, &ef) != noErr ||
        ExtAudioFileSetProperty(ef, kExtAudioFileProperty_ClientDataFormat,
                                sizeof(client), &client) != noErr) {
      if (ef) {
        ExtAudioFileDispose(ef);
      }
      [reader cancelReading];
      cleanup();
      return make_error(Code::Io, std::format("cannot write {}",
                                              out.string()));
    }
    const auto want = static_cast<std::int64_t>(std::llround(end * kMixRate));
    std::int64_t written = 0;
    Status st = ok_status();
    while (CMSampleBufferRef sb = [o copyNextSampleBuffer]) {
      CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sb);
      const auto n = static_cast<std::int64_t>(
          CMSampleBufferGetNumSamples(sb));
      std::int64_t take = n;
      if (want > 0) {
        take = std::min(n, want - written);
      }
      if (block && take > 0) {
        size_t len = 0;
        char* data = nullptr;
        // Contiguous for the reader's own buffers; copied when not.
        std::vector<char> copy;
        if (CMBlockBufferGetDataPointer(block, 0, nullptr, &len, &data) !=
                kCMBlockBufferNoErr ||
            len < static_cast<size_t>(take) * 8) {
          copy.resize(static_cast<size_t>(n) * 8);
          CMBlockBufferCopyDataBytes(block, 0, copy.size(), copy.data());
          data = copy.data();
        }
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(take * 8);
        abl.mBuffers[0].mData = data;
        if (ExtAudioFileWrite(ef, static_cast<UInt32>(take), &abl) !=
            noErr) {
          st = make_error(Code::Io, std::format("cannot write {}",
                                                out.string()));
        }
        written += take;
      }
      CFRelease(sb);
      if (!st.ok()) {
        break;
      }
    }
    // Silence to its length: the mix ends where its last sound does.
    if (st.ok() && want > written) {
      std::vector<float> zeros(8192 * 2, 0.0f);
      while (written < want) {
        const auto n = std::min<std::int64_t>(8192, want - written);
        AudioBufferList abl;
        abl.mNumberBuffers = 1;
        abl.mBuffers[0].mNumberChannels = 2;
        abl.mBuffers[0].mDataByteSize = static_cast<UInt32>(n * 8);
        abl.mBuffers[0].mData = zeros.data();
        if (ExtAudioFileWrite(ef, static_cast<UInt32>(n), &abl) != noErr) {
          st = make_error(Code::Io, std::format("cannot write {}",
                                                out.string()));
          break;
        }
        written += n;
      }
    }
    ExtAudioFileDispose(ef);
    if (reader.status == AVAssetReaderStatusFailed && st.ok()) {
      st = make_error(Code::Internal, std::format(
          "cannot mix the sound: {}",
          reader.error ? reader.error.localizedDescription.UTF8String : "?"));
    }
    cleanup();
    if (!st.ok()) {
      return st.error();
    }
    return true;
  }
}


Result<double>
mono_sound(const fs::path& in, const fs::path& out, int rate, double seconds)
{
  if (rate < 8000) {
    return make_error(Code::InvalidArgument, std::format(
        "{} Hz is not a sound to write", rate));
  }
  @autoreleasepool {
    AVURLAsset* asset = [AVURLAsset
        URLAssetWithURL:[NSURL fileURLWithPath:@(in.c_str())]
                options:nil];
    // Kept past the handler (this file is not ARC's: retained by hand).
    __block NSArray<AVAssetTrack*>* tracks = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeAudio
                 completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*) {
                   tracks = [t retain];
                   dispatch_semaphore_signal(sem);
                 }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    [tracks autorelease];
    if (tracks.count == 0) {
      return make_error(Code::Corrupt, std::format(
          "{} has no sound", in.filename().string()));
    }
    // Each track at the rate asked and its own channel count (none is
    // named: the reader keeps the track's), interleaved float.
    std::vector<float> sum;
    std::vector<std::uint8_t> heard;
    for (AVAssetTrack* track in tracks) {
      NSError* err = nil;
      AVAssetReader* r = [AVAssetReader assetReaderWithAsset:asset
                                                       error:&err];
      if (!r) {
        return make_error(Code::Io, std::format("cannot read {}",
                                                in.string()));
      }
      AVAssetReaderTrackOutput* o = [AVAssetReaderTrackOutput
          assetReaderTrackOutputWithTrack:track
                           outputSettings:@{
        AVFormatIDKey : @(kAudioFormatLinearPCM),
        AVSampleRateKey : @(rate),
        AVLinearPCMBitDepthKey : @32,
        AVLinearPCMIsFloatKey : @YES,
        AVLinearPCMIsNonInterleaved : @NO,
        AVLinearPCMIsBigEndianKey : @NO,
      }];
      [r addOutput:o];
      if (seconds > 0) {
        r.timeRange = CMTimeRangeMake(
            kCMTimeZero, CMTimeMakeWithSeconds(seconds, 48000));
      }
      if (![r startReading]) {
        return make_error(Code::Io, std::format("cannot read {}",
                                                in.string()));
      }
      std::size_t at = 0;
      std::vector<float> chunk;
      while (CMSampleBufferRef sb = [o copyNextSampleBuffer]) {
        const auto* asbd = CMAudioFormatDescriptionGetStreamBasicDescription(
            CMSampleBufferGetFormatDescription(sb));
        const std::size_t ch = asbd && asbd->mChannelsPerFrame > 0
                                   ? asbd->mChannelsPerFrame : 1;
        const auto frames = static_cast<std::size_t>(
            CMSampleBufferGetNumSamples(sb));
        CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
        const std::size_t bytes = bb ? CMBlockBufferGetDataLength(bb) : 0;
        chunk.resize(bytes / sizeof(float));
        if (bb && CMBlockBufferCopyDataBytes(bb, 0, bytes, chunk.data()) ==
                      kCMBlockBufferNoErr) {
          const std::size_t n = std::min(frames, chunk.size() / ch);
          if (sum.size() < at + n) {
            sum.resize(at + n, 0.0f);
            heard.resize(at + n, 0);
          }
          for (std::size_t f = 0; f < n; ++f) {
            float s = 0;
            for (std::size_t c = 0; c < ch; ++c) {
              s += chunk[f * ch + c];
            }
            sum[at + f] += s / static_cast<float>(ch);
            ++heard[at + f];
          }
          at += n;
        }
        CFRelease(sb);
      }
      if (r.status == AVAssetReaderStatusFailed) {
        return make_error(Code::Io, std::format(
            "cannot read {}: {}", in.string(),
            r.error ? r.error.localizedDescription.UTF8String : "?"));
      }
    }
    for (std::size_t i = 0; i < sum.size(); ++i) {
      if (heard[i] > 1) {
        sum[i] /= static_cast<float>(heard[i]);
      }
    }
    if (seconds > 0) {
      sum.resize(std::min(sum.size(), static_cast<std::size_t>(
                                          std::llround(seconds * rate))));
    }
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    fs::remove(out, ec);
    NSError* err = nil;
    // Closed before it is read: a file AVAudioFile writes is finished as
    // it closes.
    AVAudioFile* dst = [[[AVAudioFile alloc]
        initForWriting:[NSURL fileURLWithPath:@(out.c_str())]
              settings:@{
                AVFormatIDKey : @(kAudioFormatLinearPCM),
                AVSampleRateKey : @(rate),
                AVNumberOfChannelsKey : @1,
                AVLinearPCMBitDepthKey : @16,
                AVLinearPCMIsFloatKey : @NO,
                AVLinearPCMIsBigEndianKey : @NO,
              }
          commonFormat:AVAudioPCMFormatFloat32
           interleaved:NO
                 error:&err] autorelease];
    if (!dst) {
      return make_error(Code::Io, std::format(
          "cannot write {}: {}", out.string(),
          err ? err.localizedDescription.UTF8String : "?"));
    }
    const AVAudioFrameCount chunk = 65536;
    AVAudioPCMBuffer* buf = [[[AVAudioPCMBuffer alloc]
        initWithPCMFormat:dst.processingFormat frameCapacity:chunk]
        autorelease];
    for (std::size_t i = 0; i < sum.size(); i += chunk) {
      const auto n = static_cast<AVAudioFrameCount>(
          std::min<std::size_t>(chunk, sum.size() - i));
      std::copy_n(sum.data() + i, n, buf.floatChannelData[0]);
      buf.frameLength = n;
      if (![dst writeFromBuffer:buf error:&err]) {
        return make_error(Code::Io, std::format(
            "cannot write {}: {}", out.string(),
            err ? err.localizedDescription.UTF8String : "?"));
      }
    }
    [dst close];
    return static_cast<double>(sum.size()) / rate;
  }
}

Result<double>
stereo_sound(const fs::path& in, const fs::path& out, int rate)
{
  if (rate < 8000) {
    return make_error(Code::InvalidArgument, std::format(
        "{} Hz is not a sound to write", rate));
  }
  @autoreleasepool {
    AVURLAsset* asset = [AVURLAsset
        URLAssetWithURL:[NSURL fileURLWithPath:@(in.c_str())]
                options:nil];
    // Kept past the handler (this file is not ARC's: retained by hand).
    __block NSArray<AVAssetTrack*>* tracks = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:AVMediaTypeAudio
                 completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*) {
                   tracks = [t retain];
                   dispatch_semaphore_signal(sem);
                 }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    dispatch_release(sem);
    [tracks autorelease];
    if (tracks.count == 0) {
      return make_error(Code::Corrupt, std::format(
          "{} has no sound", in.filename().string()));
    }
    NSError* err = nil;
    AVAssetReader* r = [AVAssetReader assetReaderWithAsset:asset
                                                     error:&err];
    if (!r) {
      return make_error(Code::Io, std::format("cannot read {}",
                                              in.string()));
    }
    // Every track through the mixer, as two channels named stereo: a
    // mono track goes to both sides, a 5.1 is folded down by its layout.
    AudioChannelLayout layout{};
    layout.mChannelLayoutTag = kAudioChannelLayoutTag_Stereo;
    AVAssetReaderAudioMixOutput* o = [AVAssetReaderAudioMixOutput
        assetReaderAudioMixOutputWithAudioTracks:tracks
                                   audioSettings:@{
      AVFormatIDKey : @(kAudioFormatLinearPCM),
      AVSampleRateKey : @(rate),
      AVNumberOfChannelsKey : @2,
      AVChannelLayoutKey : [NSData dataWithBytes:&layout
                                          length:sizeof layout],
      AVLinearPCMBitDepthKey : @32,
      AVLinearPCMIsFloatKey : @YES,
      AVLinearPCMIsNonInterleaved : @NO,
      AVLinearPCMIsBigEndianKey : @NO,
    }];
    [r addOutput:o];
    if (![r startReading]) {
      return make_error(Code::Io, std::format("cannot read {}",
                                              in.string()));
    }
    FloatWav wav;
    VALTZ_TRY(wav.open(out, 2, rate));
    std::vector<float> chunk;
    while (CMSampleBufferRef sb = [o copyNextSampleBuffer]) {
      const auto frames = static_cast<std::size_t>(
          CMSampleBufferGetNumSamples(sb));
      CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
      const std::size_t bytes = bb ? CMBlockBufferGetDataLength(bb) : 0;
      chunk.resize(bytes / sizeof(float));
      Status st = ok_status();
      if (bb && CMBlockBufferCopyDataBytes(bb, 0, bytes, chunk.data()) ==
                    kCMBlockBufferNoErr) {
        st = wav.append_interleaved(chunk.data(),
                                    std::min(frames, chunk.size() / 2));
      }
      CFRelease(sb);
      VALTZ_TRY(st);
    }
    if (r.status == AVAssetReaderStatusFailed) {
      return make_error(Code::Io, std::format(
          "cannot read {}: {}", in.string(),
          r.error ? r.error.localizedDescription.UTF8String : "?"));
    }
    VALTZ_TRY(wav.close());
    return static_cast<double>(wav.frames()) / rate;
  }
}

namespace {

// Little-endian, as a WAV is.
void
put_u32(std::uint8_t* at, std::uint32_t v)
{
  for (int i = 0; i < 4; ++i) {
    at[i] = static_cast<std::uint8_t>(v >> (8 * i));
  }
}

void
put_u16(std::uint8_t* at, std::uint16_t v)
{
  at[0] = static_cast<std::uint8_t>(v);
  at[1] = static_cast<std::uint8_t>(v >> 8);
}

// RIFF, a "fmt " of IEEE float (format 3), a "fact" (its frames: what
// a non-PCM WAV carries) and the "data" chunk's header: 58 bytes.
constexpr std::size_t kFloatWavHeader = 58;

void
float_wav_header(std::uint8_t* h, int channels, int rate,
                 std::uint64_t frames)
{
  const auto block = static_cast<std::uint32_t>(channels * 4);
  const auto data = static_cast<std::uint32_t>(frames * block);
  std::memcpy(h, "RIFF", 4);
  put_u32(h + 4, static_cast<std::uint32_t>(kFloatWavHeader - 8 + data));
  std::memcpy(h + 8, "WAVEfmt ", 8);
  put_u32(h + 16, 18);
  put_u16(h + 20, 3);
  put_u16(h + 22, static_cast<std::uint16_t>(channels));
  put_u32(h + 24, static_cast<std::uint32_t>(rate));
  put_u32(h + 28, static_cast<std::uint32_t>(rate) * block);
  put_u16(h + 32, static_cast<std::uint16_t>(block));
  put_u16(h + 34, 32);
  put_u16(h + 36, 0);
  std::memcpy(h + 38, "fact", 4);
  put_u32(h + 42, 4);
  put_u32(h + 46, static_cast<std::uint32_t>(frames));
  std::memcpy(h + 50, "data", 4);
  put_u32(h + 54, data);
}

}

FloatWav::~FloatWav()
{
  (void)close();
}

Status
FloatWav::open(const fs::path& path, int channels, int rate)
{
  if (_f) {
    return make_error(Code::InvalidArgument, "the WAV is open already");
  }
  if (channels < 1 || channels > 8 || rate < 8000) {
    return make_error(Code::InvalidArgument, std::format(
        "{} channels at {} Hz is not a sound to write", channels, rate));
  }
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  _f = std::fopen(path.c_str(), "wb");
  if (!_f) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            path.string()));
  }
  _path = path;
  _channels = channels;
  _rate = rate;
  _frames = 0;
  std::uint8_t h[kFloatWavHeader];
  float_wav_header(h, channels, rate, 0);
  if (std::fwrite(h, 1, sizeof h, _f) != sizeof h) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            path.string()));
  }
  return ok_status();
}

Status
FloatWav::write_(const float* interleaved, std::size_t frames)
{
  if (!_f) {
    return make_error(Code::InvalidArgument, "the WAV is not open");
  }
  const std::uint64_t block = static_cast<std::uint64_t>(_channels) * 4;
  if ((_frames + frames) * block + kFloatWavHeader >= 0xFFFFFFFFull) {
    return make_error(Code::Unsupported, std::format(
        "{} is too long for a WAV", _path.filename().string()));
  }
  const std::size_t n = frames * static_cast<std::size_t>(_channels);
  if (std::fwrite(interleaved, sizeof(float), n, _f) != n) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            _path.string()));
  }
  _frames += frames;
  return ok_status();
}

Status
FloatWav::append_interleaved(const float* samples, std::size_t frames)
{
  return frames == 0 ? ok_status() : write_(samples, frames);
}

Status
FloatWav::append_planar(const float* samples, std::size_t frames)
{
  if (frames == 0) {
    return ok_status();
  }
  const auto ch = static_cast<std::size_t>(_channels);
  _scratch.resize(frames * ch);
  for (std::size_t c = 0; c < ch; ++c) {
    const float* plane = samples + c * frames;
    for (std::size_t i = 0; i < frames; ++i) {
      _scratch[i * ch + c] = plane[i];
    }
  }
  return write_(_scratch.data(), frames);
}

Status
FloatWav::close()
{
  if (!_f) {
    return ok_status();
  }
  std::uint8_t h[kFloatWavHeader];
  float_wav_header(h, _channels, _rate, _frames);
  const bool ok = std::fseek(_f, 0, SEEK_SET) == 0 &&
                  std::fwrite(h, 1, sizeof h, _f) == sizeof h;
  const bool closed = std::fclose(_f) == 0;
  _f = nullptr;
  if (!ok || !closed) {
    return make_error(Code::Io, std::format("cannot write {}",
                                            _path.string()));
  }
  return ok_status();
}

Status
convert_sound(const fs::path& in, const fs::path& out, int channels,
              int rate)
{
  if ((channels != 1 && channels != 2) || rate < 8000) {
    return make_error(Code::InvalidArgument, std::format(
        "{} channels at {} Hz is not a sound to write", channels, rate));
  }
  @autoreleasepool {
    NSError* err = nil;
    NSURL* src_url = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:in.c_str()]];
    AVAudioFile* src = [[AVAudioFile alloc] initForReading:src_url
                                                     error:&err];
    if (!src) {
      return make_error(Code::Io, std::format(
          "cannot read {}: {}", in.string(),
          err ? err.localizedDescription.UTF8String : "?"));
    }
    AVAudioFormat* from = src.processingFormat;
    AVAudioFormat* to = [[AVAudioFormat alloc]
        initWithCommonFormat:AVAudioPCMFormatFloat32
                  sampleRate:rate
                    channels:static_cast<AVAudioChannelCount>(channels)
                 interleaved:NO];
    AVAudioConverter* conv = [[AVAudioConverter alloc] initFromFormat:from
                                                             toFormat:to];
    if (!conv) {
      return make_error(Code::Unsupported, std::format(
          "{} cannot be made {} channels at {} Hz", in.string(), channels,
          rate));
    }
    // Stereo to mono: the two mixed, not one dropped.
    conv.downmix = YES;
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    fs::remove(out, ec);
    NSURL* dst_url = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:out.c_str()]];
    AVAudioFile* dst = [[AVAudioFile alloc]
        initForWriting:dst_url
              settings:@{
                AVFormatIDKey : @(kAudioFormatLinearPCM),
                AVSampleRateKey : @(rate),
                AVNumberOfChannelsKey : @(channels),
                AVLinearPCMBitDepthKey : @24,
                AVLinearPCMIsFloatKey : @NO,
                AVLinearPCMIsBigEndianKey : @NO,
              }
          commonFormat:AVAudioPCMFormatFloat32
           interleaved:NO
                 error:&err];
    if (!dst) {
      return make_error(Code::Io, std::format(
          "cannot write {}: {}", out.string(),
          err ? err.localizedDescription.UTF8String : "?"));
    }
    const AVAudioFrameCount chunk = 8192;
    AVAudioPCMBuffer* in_buf =
        [[AVAudioPCMBuffer alloc] initWithPCMFormat:from
                                      frameCapacity:chunk];
    const auto out_cap = static_cast<AVAudioFrameCount>(
        std::ceil(chunk * rate / from.sampleRate) + 1024);
    AVAudioPCMBuffer* out_buf =
        [[AVAudioPCMBuffer alloc] initWithPCMFormat:to
                                      frameCapacity:out_cap];
    __block bool ended = false;
    for (;;) {
      out_buf.frameLength = 0;
      NSError* cerr = nil;
      const AVAudioConverterOutputStatus st = [conv
          convertToBuffer:out_buf
                    error:&cerr
       withInputFromBlock:^AVAudioBuffer*(AVAudioPacketCount n,
                                          AVAudioConverterInputStatus* is) {
        if (ended) {
          *is = AVAudioConverterInputStatus_EndOfStream;
          return nil;
        }
        in_buf.frameLength = 0;
        NSError* rerr = nil;
        if (![src readIntoBuffer:in_buf
                      frameCount:std::min<AVAudioFrameCount>(n, chunk)
                           error:&rerr] ||
            in_buf.frameLength == 0) {
          ended = true;
          *is = AVAudioConverterInputStatus_EndOfStream;
          return nil;
        }
        *is = AVAudioConverterInputStatus_HaveData;
        return in_buf;
      }];
      if (st == AVAudioConverterOutputStatus_Error) {
        return make_error(Code::Io, std::format(
            "{} could not be converted: {}", in.string(),
            cerr ? cerr.localizedDescription.UTF8String : "?"));
      }
      if (out_buf.frameLength > 0 &&
          ![dst writeFromBuffer:out_buf error:&err]) {
        return make_error(Code::Io, std::format(
            "cannot write {}: {}", out.string(),
            err ? err.localizedDescription.UTF8String : "?"));
      }
      if (st == AVAudioConverterOutputStatus_EndOfStream) {
        break;
      }
    }
    // Its header written whole now, not when the object happens to go.
    [dst close];
  }
  return ok_status();
}

}
