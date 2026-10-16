// A composition's SOUND (DESIGN §6a): the mix of its layers' sounds, each
// through where it lies in time (its marks, its start, its speed), its
// volume and pitch (keyed), and the transitions -- at 48 kHz stereo.
//
// One description, two players of it:
//   * the MIXER here (`mix_sound`) writes it to a file -- exports, a
//     nested composition, a sound-like reference, a flattened one --
//     through AVFoundation's own edit and mix machinery (an
//     AVMutableComposition of the layers' spans, scaled for speed with
//     pitch kept, and an AVAudioMix's volume ramps), read out once;
//   * the app's PLAYER builds the same composition and ramps from the
//     plan's JSON (`time_segments`, `volume_points`) and plays it as it
//     goes -- an hour-long timeline starts at once. Only PITCH has no
//     real-time path there: a plan that shifts it is played from the
//     mixer's file (`needs_render`).
// Pitch is applied to a layer's span first (AVAudioUnitTimePitch, offline:
// its keys read at the source time they fall on), then the span is a
// sound like any other. A pitch that FOLLOWS the speed is shifted there
// too: the stretch after keeps the pitch it is given.

#ifndef VALTZ_MEDIA_SOUND_H
#define VALTZ_MEDIA_SOUND_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/media/keyframes.h"
#include "valtz/media/timing.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

namespace valtz::media {

inline constexpr int kMixRate = 48000;

struct SoundLayer {
  std::filesystem::path file;  // a sound, or a clip with one
  LayerTiming           timing;
  KeyedSound            sound;  // keyed at its own frames of timing.rate
  // Its pitch FOLLOWS its speed, as a tape's does (twice as fast, an
  // octave up: 12 log2 of the speed, on top of its keys' pitch, held to
  // kMaxPitch either way) -- rather than being held while it is stretched.
  bool                  follow_speed = false;
};

struct SoundPlan {
  // Every layer of the composition, in order (transitions name them by
  // index); a layer with no file has no sound.
  std::vector<SoundLayer>     layers;
  std::vector<TransitionSpec> transitions;
  double                      seconds = 0;  // the timeline's length
};

// {"layers": [{"file", "timing", "sound"}], "transitions", "seconds"}.
Json to_json(const SoundPlan&);
SoundPlan sound_plan_from_json(const Json&);

// Does `file` hold a sound (an audio track)?
bool has_sound(const std::filesystem::path& file);

// Where layer `i`'s source plays on the timeline: each piece of its span
// at a constant rate -- one for a constant speed, a frame's worth each
// along a ramp. Seconds; up to `end`.
struct TimeSegment {
  double at = 0;             // timeline
  double length = 0;         // timeline
  double source = 0;         // source
  double source_length = 0;  // source
};
std::vector<TimeSegment> time_segments(const LayerTiming&, double end,
                                       double step);

// Layer `i`'s gain over the timeline, as (second, gain) points with
// linear ramps between: its volume keys times the transitions' weights,
// at least every `step` seconds where they change.
std::vector<std::pair<double, double>>
volume_points(const SoundPlan&, std::size_t i, double step = 0.1);

// Any layer's pitch shifted -- by its keys, or following a speed?
bool needs_render(const SoundPlan&);

// The mix written to `out` as a 24-bit WAV, `plan.seconds` long. False
// when no layer has a sound (nothing written). Blocking.
Result<bool> mix_sound(const SoundPlan& plan,
                       const std::filesystem::path& out);

// A file's SOUND as ONE channel at `rate` Hz, written as a 16-bit WAV --
// what a speech model hears (DESIGN §4h): every sound track of it (a
// clip's too) read at its own channel count, each sample's channels
// AVERAGED -- every channel heard, none dropped or weighted -- then the
// tracks averaged where they overlap. Its length, in seconds. Blocking.
Result<double> mono_sound(const std::filesystem::path& in,
                          const std::filesystem::path& out, int rate);

// A file's SOUND as TWO channels at `rate` Hz, written as a 32-bit float
// WAV -- what a source separator hears (DESIGN §4j): every sound track of
// it (a clip's too) mixed by AVFoundation's own mixer, a mono one on both
// sides, more channels folded down by their layout. Its length, in
// seconds. Blocking.
Result<double> stereo_sound(const std::filesystem::path& in,
                            const std::filesystem::path& out, int rate);

// A WAV WRITTEN AS IT COMES -- 32-bit float -- so a stream of PCM (a
// separator's output) goes to disk piece by piece: nothing held, nothing
// clipped. The header is sized when it is closed (a WAV holds at most
// 4 GB of samples). One writer a stream; not thread-safe.
class FloatWav {
public:
  FloatWav() = default;
  ~FloatWav();  // closes
  FloatWav(const FloatWav&) = delete;
  FloatWav& operator=(const FloatWav&) = delete;

  Status open(const std::filesystem::path&, int channels, int rate);
  // `frames` frames, the channels side by side (L R L R ...).
  Status append_interleaved(const float* samples, std::size_t frames);
  // `frames` frames, a channel after another (L L ... R R ...).
  Status append_planar(const float* samples, std::size_t frames);
  Status close();
  std::uint64_t frames() const noexcept { return _frames; }
  int channels() const noexcept { return _channels; }
  int rate() const noexcept { return _rate; }

private:
  Status write_(const float* interleaved, std::size_t frames);
  std::FILE*            _f = nullptr;
  std::filesystem::path _path;
  int                   _channels = 0;
  int                   _rate = 0;
  std::uint64_t         _frames = 0;
  std::vector<float>    _scratch;
};

// A sound as `channels` (1, mixed down; 2) at `rate` Hz -- a project's
// output (project::OutputSettings) -- written as a 24-bit WAV. Blocking.
Status convert_sound(const std::filesystem::path& in,
                     const std::filesystem::path& out, int channels,
                     int rate);

}

#endif
