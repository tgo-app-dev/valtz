// The Mac's CAMERA (DESIGN §7b): a still or a clip taken with it -- the
// MacBook's own, a Continuity Camera, one plugged in -- as an asset.
//
// A camera is SHOWN once started: its frames come into IOSurfaces, and
// the newest is kept, for a preview to show as it is (no copy). A STILL
// is that frame written as a picture; a CLIP is recorded to a movie
// (QuickTime, the default microphone's sound with it when Valtz may use
// it) until it is stopped.
//
// PERMISSIONS are macOS's: the app's NSCameraUsageDescription (hardened:
// the camera entitlement) and the person's leave, asked by the app before
// it starts; refused, start() says so by name.

#ifndef VALTZ_MEDIA_CAMERA_H
#define VALTZ_MEDIA_CAMERA_H

#include "valtz/base/result.h"

#include <IOSurface/IOSurfaceRef.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace valtz::media {

struct CameraSource {
  std::string id;      // the device's unique id
  std::string name;    // as macOS names it: "MacBook Pro Camera"
  bool        preferred = false;  // the system's default camera
};

// The cameras there now, the default first.
std::vector<CameraSource> camera_sources();

// May Valtz use the cameras: "granted", "denied", "undetermined".
std::string camera_permission();
// macOS asks the person (once), waiting for the answer: a command line's
// way -- the app asks without waiting. True when granted.
bool request_camera_access();

class CameraCapture {
public:
  // `camera` (an id of camera_sources; "" the default) started and shown;
  // `sound`: a clip recorded with the default microphone's sound.
  static Result<std::unique_ptr<CameraCapture>> start(
      const std::string& camera, bool sound);
  ~CameraCapture();

  const std::string& name() const;
  // The newest frame: its surface (RETAINED, the caller's to release;
  // null before the first), its size and its count.
  struct Frame {
    IOSurfaceRef  surface = nullptr;
    int           width = 0;
    int           height = 0;
    std::uint64_t count = 0;
  };
  Frame newest() const;
  // A STILL: the newest frame written as `out` -- JPEG, PNG or HEIC by
  // its extension.
  Status snap(const std::filesystem::path& out);
  // A CLIP recorded to `out` (.mov) until stop_recording; its seconds so
  // far.
  Status record(const std::filesystem::path& out);
  bool recording() const;
  double seconds() const;
  // The clip's file, while it is recorded (and once stopped).
  const std::filesystem::path& file() const;
  // The movie whole and closed.
  Status stop_recording();

private:
  CameraCapture();
  struct Impl;
  std::unique_ptr<Impl> _impl;
};

}

#endif
