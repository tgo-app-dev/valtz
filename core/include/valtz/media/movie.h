// Movie files, put together with AVFoundation.
//
// vpipe's Apple-native writer (avf-save-video: HEVC Main10, ProRes) is
// VIDEO ONLY, and a model that makes a soundtrack with its picture
// (MiniMax H3) hands the sound over separately. Rather than take the
// FFmpeg path for the sake of the mux -- 8-bit, and a library the app
// would have to ship -- the two are written apart and joined here: the
// video's samples copied as they are (no re-encode, so its 10 bits and
// colour tags survive), the sound encoded once, to AAC -- or, in a
// ProRes intermediate, kept as PCM. A sound alone -- a song saved,
// trimmed -- is written out here too.

#ifndef VALTZ_MEDIA_MOVIE_H
#define VALTZ_MEDIA_MOVIE_H

#include "valtz/base/result.h"

#include <filesystem>
#include <span>
#include <string_view>

namespace valtz::media {

// The INTERMEDIATE formats a project keeps the clips it makes in
// (project::OutputSettings::intermediate), best first: vpipe's
// avf-save-video codec names, which encode_movie_stack takes too. ProRes
// goes in a .mov, its sound PCM; HEVC (Main10) in an .mp4, AAC.
inline constexpr std::string_view kDefaultIntermediate = "prores422hq";
std::span<const std::string_view> intermediate_formats();
bool is_intermediate_format(std::string_view);
bool is_prores(std::string_view format);

// How a movie's sound is stored: AAC (as it is, when it is AAC already),
// or linear PCM -- 24-bit integer, nothing lost -- for a ProRes movie.
enum class SoundCodec { Aac, Pcm };

// `video`'s first video track and `audio`'s first audio track into
// `out` (.mp4, .m4v or .mov; replaced if there). The video is passed
// through; the sound becomes AAC at its own rate and channel count (AAC
// already is passed through too) -- or, `Pcm` (a .mov only), 24-bit PCM
// at its rate and channels, whatever it was. With a `duration`
// (seconds), only the sound from `start` for that long, moved to the
// start -- a trimmed clip's: decoded and encoded again, so it is cut to
// the sample. Blocking.
Status add_soundtrack(const std::filesystem::path& video,
                      const std::filesystem::path& audio,
                      const std::filesystem::path& out, double start = 0,
                      double duration = 0,
                      SoundCodec sound = SoundCodec::Aac);

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
