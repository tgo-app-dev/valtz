// Sound CAPTURED on this Mac (DESIGN §7b): a microphone, or the SYSTEM's
// audio -- what every app plays, Valtz's own left out -- as macOS's
// Screenshot app records it (ScreenCaptureKit).
//
// A capture writes a WAV as the sound comes (24-bit PCM, at the source's
// own rate and channels), so a long one is never held in memory; stopped,
// the file is whole. Its level -- the peak of the last tenth of a second
// -- and its length are read while it runs: what a meter polls (the
// bridge calls nothing back).
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
#include <string>
#include <vector>

namespace valtz::media {

struct CaptureSource {
  // "system", or "mic:<the device's unique id>".
  std::string id;
  std::string name;   // as macOS names it: "MacBook Pro Microphone"
  std::string kind;   // "microphone" | "system"
  bool        preferred = false;  // the system's default input
};

// The microphones there now (the default first), then the system's
// audio.
std::vector<CaptureSource> capture_sources();

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
