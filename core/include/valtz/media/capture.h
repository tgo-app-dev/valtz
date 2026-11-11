// Sound CAPTURED on this Mac (DESIGN §7b): a microphone, or the SYSTEM's
// audio -- what every app plays, Valtz's own left out -- as macOS's
// Screenshot app records it (ScreenCaptureKit).
//
// A capture writes a WAV as the sound comes (24-bit PCM, at the source's
// own rate and channels), so a long one is never held in memory; stopped,
// the file is whole. A microphone is asked for 32-bit float at its own
// rate, and each buffer is converted from ITS OWN format: an external
// microphone's first buffer said 16-bit and its samples were float, and
// all were read as the first said -- each float's halves as two loud
// samples, the take unusable (2026-11-10). Its level -- the peak of the
// last tenth of a second -- and its length are read while it runs: what
// a meter polls (the bridge calls nothing back).
//
// PERMISSIONS are macOS's: a microphone needs the app's
// NSMicrophoneUsageDescription (and, hardened, the audio-input
// entitlement) and the person's leave; the system's audio, Screen &
// System Audio Recording. Refused, start() says so by name.

#ifndef VALTZ_MEDIA_CAPTURE_H
#define VALTZ_MEDIA_CAPTURE_H

#include "valtz/base/result.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace valtz::media {

struct CaptureSource {
  // "system", or "mic:<the device's unique id>".
  std::string id;
  std::string name;   // as macOS names it: "MacBook Pro Microphone"
  std::string kind;   // "microphone" | "system"
  bool        preferred = false;  // the system's default input
  // A microphone's INPUT VOLUME as macOS sets it -- System Settings ›
  // Sound › Input's slider, 0...1 -- where it has one to set (many USB
  // microphones do; one with a knob of its own may not).
  std::optional<double> input_volume;
};

// The microphones there now (the default first), then the system's
// audio.
std::vector<CaptureSource> capture_sources();

// A microphone's input volume set (0...1), as System Settings sets it --
// for every app, kept by macOS. Refused for one that has none to set.
Status set_input_volume(const std::string& source, double volume);

// For tests: buffers as a source hands them -- each in its OWN format,
// float or integer, interleaved samples -1...1 -- through a capture's
// writer into `out` (a microphone whose format changes as it starts: its
// first buffers 16-bit, the rest float, 2026-11-10).
struct PcmPiece {
  bool                is_float = true;
  int                 bits = 32;      // 16, 24 (packed) or 32
  int                 channels = 1;
  double              rate = 48000;
  std::vector<double> samples;        // interleaved
};
Status write_capture_pieces(const std::vector<PcmPiece>& pieces,
                            const std::filesystem::path& out);

// May Valtz use the microphones: "granted", "denied", "undetermined" --
// the person not asked yet (the app asks before it starts).
std::string microphone_permission();

class SoundCapture {
public:
  // Started on `source` (an id of capture_sources), writing `out`.
  static Result<std::unique_ptr<SoundCapture>> start(
      const std::string& source, const std::filesystem::path& out);
  ~SoundCapture();

  // Seconds written so far; the peak of the last tenth of a second
  // (0..1, linear).
  double seconds() const;
  double level() const;
  // When its FIRST SAMPLE was heard, as its source stamped it: seconds of
  // the host clock (CMClockGetHostTimeClock, CACurrentMediaTime's) -- NaN
  // until one has come. What a voice-over's recording is lined up with
  // the picture by: the player started at a host time it names.
  double started_at() const;
  const std::string& source() const;
  const std::filesystem::path& file() const;
  // Stopped: the file whole and closed (an error when nothing came).
  Status stop();

private:
  SoundCapture();
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}

#endif
