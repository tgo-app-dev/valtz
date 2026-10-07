// Probe a media file: what it is, how big, how it is encoded, and its
// color. Uses ImageIO for stills and AVFoundation for movies -- the
// system frameworks read ProRes (incl. 4444 alpha), HEVC Main10/HDR,
// HEIC with gain maps, EXR and 16-bit PNG/TIFF natively, which the
// FFmpeg path in vpipe does not.
//
// Blocking; call from a worker thread.

#ifndef VALTZ_MEDIA_PROBE_H
#define VALTZ_MEDIA_PROBE_H

#include "valtz/base/result.h"
#include "valtz/media/format.h"

#include <filesystem>

namespace valtz::media {

Result<MediaInfo> probe_file(const std::filesystem::path&);

// Guess the media type from the extension / UTI alone (no I/O).
MediaType media_type_for_path(const std::filesystem::path&);

}

#endif
