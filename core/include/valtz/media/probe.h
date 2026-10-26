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

// The degrees clockwise a movie's picture track turns its stored picture
// by to show it (MediaInfo::turn): 0, 90, 180, 270; 0 without a picture.
// Its header alone, nothing decoded -- for a record that does not say.
std::int32_t movie_turn(const std::filesystem::path&);

// A movie's LAST PICTURE, counted from its first at `rate` (its own, for
// 0/1): its last frame's own time, not its picture track's length -- a
// track a little longer than its frames (more than half a frame) counted
// one that does not exist, and a grab there drew nothing. -1 where the
// file says neither (no picture, no sample timing).
std::int64_t last_picture_frame(const std::filesystem::path&,
                                Rational rate = {0, 1});

// Guess the media type from the extension / UTI alone (no I/O).
MediaType media_type_for_path(const std::filesystem::path&);

}

#endif
