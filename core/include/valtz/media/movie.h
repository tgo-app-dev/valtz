// Movie files, put together with AVFoundation.
//
// vpipe's Apple-native writer (avf-save-video: HEVC Main10, ProRes) is
// VIDEO ONLY, and a model that makes a soundtrack with its picture
// (MiniMax H3) hands the sound over separately. Rather than take the
// FFmpeg path for the sake of the mux -- 8-bit, and a library the app
// would have to ship -- the two are written apart and joined here: the
// video's samples copied as they are (no re-encode, so its 10 bits and
// colour tags survive), the sound encoded once, to AAC. A sound alone --
// a song saved, trimmed -- is written out here too.

#ifndef VALTZ_MEDIA_MOVIE_H
#define VALTZ_MEDIA_MOVIE_H

#include "valtz/base/result.h"

#include <filesystem>
#include <string_view>

namespace valtz::media {

// `video`'s first video track and `audio`'s first audio track into
// `out` (.mp4, .m4v or .mov; replaced if there). The video is passed
// through; the sound becomes AAC at its own rate and channel count (AAC
// already is passed through too). With a `duration` (seconds), only the
// sound from `start` for that long, moved to the start -- a trimmed
// clip's: decoded and encoded again, so it is cut to the sample.
// Blocking.
Status add_soundtrack(const std::filesystem::path& video,
                      const std::filesystem::path& audio,
                      const std::filesystem::path& out, double start = 0,
                      double duration = 0);

// A SOUND written out (a song saved, trimmed): `src`'s first audio track
// as `format` -- "wav" (PCM, as deep as the sound: its own integer
// depth, else 24 bits) or "m4a" (AAC, 128 kb/s a channel) -- into `out`
// (replaced if there). With a `duration` (seconds), only the sound from
// `start` for that long, moved to the start: decoded, so cut to the
// SAMPLE -- vpipe's load-audio cuts to the packet, and its save-audio
// writes AAC through FFmpeg, which the app does not ship. Blocking.
Status write_sound(const std::filesystem::path& src,
                   const std::filesystem::path& out,
                   std::string_view format, double start = 0,
                   double duration = 0);

}

#endif
