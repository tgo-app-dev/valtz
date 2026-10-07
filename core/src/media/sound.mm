#include "valtz/media/sound.h"

#include "valtz/base/log.h"

#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <set>

namespace valtz::media {

namespace fs = std::filesystem;

// ---- the plan ---------------------------------------------------------

Json
to_json(const SoundPlan& p)
{
  Json ls = Json::array();
  for (const auto& l : p.layers) {
    ls.push_back({{"file", l.file.string()},
                  {"timing", to_json(l.timing)},
                  {"sound", to_json(l.sound)},
                  {"follow_speed", l.follow_speed}});
  }
  Json ts = Json::array();
  for (const auto& t : p.transitions) {
    ts.push_back({{"from", t.from}, {"to", t.to},
                  {"dissolve", t.dissolve}});
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
    p.layers.push_back(std::move(sl));
  }
  for (const auto& t : jget(j, "transitions", Json::array())) {
    p.transitions.push_back(
        {jget<std::size_t>(t, "from", 0), jget<std::size_t>(t, "to", 0),
         jget(t, "dissolve", false)});
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
  return out;
}

bool
needs_render(const SoundPlan& p)
{
  return std::ranges::any_of(p.layers, [](const SoundLayer& l) {
    return shifts_pitch(l);
  });
}

namespace {

// The standard format the mixer and the pitch pass work in.
AVAudioFormat*
mix_format()
{
  return [[AVAudioFormat alloc] initStandardFormatWithSampleRate:kMixRate
                                                        channels:2];
}

// `file`'s sound from `in` to `out` seconds, as float PCM at the mix's
// rate: what the pitch pass shifts.
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
  AVAudioPCMBuffer* buf = [[AVAudioPCMBuffer alloc]
      initWithPCMFormat:mix_format() frameCapacity:cap];
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

// Layer `l`'s span with its pitch shifted -- each block at the pitch its
// keys give where it falls -- written to `out` (CAF). Its timing then
// starts the span from 0.
Status
render_pitch(const SoundLayer& l, const fs::path& out)
{
  VALTZ_ASSIGN(AVAudioPCMBuffer* src,
               read_span(l.file, l.timing.in, l.timing.out));
  AVAudioEngine* engine = [[AVAudioEngine alloc] init];
  AVAudioPlayerNode* player = [[AVAudioPlayerNode alloc] init];
  AVAudioUnitTimePitch* tp = [[AVAudioUnitTimePitch alloc] init];
  [engine attachNode:player];
  [engine attachNode:tp];
  AVAudioFormat* fmt = mix_format();
  [engine connect:player to:tp format:fmt];
  [engine connect:tp to:engine.mainMixerNode format:fmt];
  NSError* err = nil;
  constexpr AVAudioFrameCount kBlock = 2048;
  if (![engine enableManualRenderingMode:AVAudioEngineManualRenderingModeOffline
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
  std::error_code ec;
  fs::remove(out, ec);
  AVAudioFile* file = [[AVAudioFile alloc]
      initForWriting:[NSURL fileURLWithPath:@(out.c_str())]
            settings:fmt.settings
               error:&err];
  if (!file) {
    [engine stop];
    return make_error(Code::Io, std::format("cannot write {}",
                                            out.string()));
  }
  AVAudioPCMBuffer* block = [[AVAudioPCMBuffer alloc]
      initWithPCMFormat:engine.manualRenderingFormat frameCapacity:kBlock];
  const AVAudioFramePosition total = src.frameLength;
  Status st = ok_status();
  while (engine.manualRenderingSampleTime < total) {
    const double s = l.timing.in +
        static_cast<double>(engine.manualRenderingSampleTime) / kMixRate;
    tp.pitch = static_cast<float>(
        pitch_at(l, l.timing.local_at_source(s)) * 100.0);
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
    if (![file writeFromBuffer:block error:&err]) {
      st = make_error(Code::Io, std::format("cannot write {}",
                                            out.string()));
      break;
    }
  }
  [player stop];
  [engine stop];
  return st;
}

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
      if (shifts_pitch(l)) {
        // The span shifted first; then it is a sound from 0.
        const fs::path shifted = fs::temp_directory_path() /
            std::format("valtz-pitch-{}-{}.caf", ::getpid(),
                        reinterpret_cast<std::uintptr_t>(&l) + i);
        VALTZ_TRY(render_pitch(l, shifted));
        scratch.push_back(shifted);
        const double span = l.timing.out - l.timing.in;
        l.file = shifted;
        l.timing.out = std::isfinite(span) ? span : kForever;
        // Keys as they were, read along the shifted span's time.
        const double shift = l.timing.in;
        l.timing.in = 0;
        (void)shift;
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
      for (const auto& seg : time_segments(l.timing, end, 1.0 / 24)) {
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
