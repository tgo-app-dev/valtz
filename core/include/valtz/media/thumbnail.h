// Thumbnails: a JPEG no larger than `max_px` on its long edge, keeping
// the source's color space (the ICC profile is embedded, so a P3 still
// stays P3). Stills via ImageIO; video via AVFoundation, from a frame
// ~10% in (past black leaders and slates). Blocking; call off the main
// thread. The controller caches results in the managed cache keyed by
// the source's content hash, so each is made once per content.

#ifndef VALTZ_MEDIA_THUMBNAIL_H
#define VALTZ_MEDIA_THUMBNAIL_H

#include "valtz/base/result.h"
#include "valtz/media/format.h"

#include <filesystem>

namespace valtz::media {

Status write_thumbnail(const std::filesystem::path& src, MediaType type,
                       int max_px, const std::filesystem::path& dst_jpeg);

}

#endif
