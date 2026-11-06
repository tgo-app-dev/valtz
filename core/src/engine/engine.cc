#include "valtz/engine/engine.h"

#include <algorithm>
#include <format>

namespace valtz::engine {

const char*
to_str(JobEventKind k)
{
  switch (k) {
  case JobEventKind::Started:   return "started";
  case JobEventKind::Progress:  return "progress";
  case JobEventKind::Preview:   return "preview";
  case JobEventKind::Text:      return "text";
  case JobEventKind::Output:    return "output";
  case JobEventKind::Finished:  return "finished";
  case JobEventKind::Failed:    return "failed";
  case JobEventKind::Cancelled: return "cancelled";
  }
  return "?";
}

namespace {

constexpr ExportFormat kExportFormats[] = {
  {"png16", "png", false, "PNG, 16 bits per channel"},
  {"tiff16", "tif", false, "TIFF, 16 bits per channel"},
  {"exr", "exr", false, "OpenEXR, half float, linear light"},
  {"png", "png", false, "PNG, 8 bits per channel"},
  {"tiff", "tif", false, "TIFF, 8 bits per channel"},
  {"jpeg", "jpg", false, "JPEG"},
  {"pdf", "pdf", false,
   "PDF, shapes and words kept as vectors (a still's pages, one file)"},
  {"prores4444", "mov", true, "ProRes 4444, keeps alpha (12-bit)"},
  {"prores422hq", "mov", true, "ProRes 422 HQ (10-bit)"},
  {"hevc10", "mov", true, "HEVC Main10, keeps HDR (PQ / HLG)"},
  {"h264", "mp4", true, "H.264 (8-bit)"},
  {"wav", "wav", false, "WAV, uncompressed, as deep as the sound", true},
  {"m4a", "m4a", false, "AAC (.m4a), 128 kb/s a channel", true},
};

}

std::span<const ExportFormat>
export_formats()
{
  return kExportFormats;
}

const ExportFormat*
export_format(std::string_view name)
{
  for (const auto& f : kExportFormats) {
    if (f.name == name) { return &f; }
  }
  return nullptr;
}

bool
VideoEncoding::empty() const
{
  return bitrate <= 0 && max_bitrate <= 0 && quality <= 0 &&
         keyframe_seconds <= 0 && b_frames < 0 && profile.empty() &&
         level.empty() && entropy.empty() && prores.empty();
}

Json
to_json(const VideoEncoding& e)
{
  Json j = Json::object();
  if (e.bitrate > 0) { j["bitrate"] = e.bitrate; }
  if (e.max_bitrate > 0) { j["max_bitrate"] = e.max_bitrate; }
  if (e.quality > 0) { j["quality"] = e.quality; }
  if (e.keyframe_seconds > 0) { j["keyframe_seconds"] = e.keyframe_seconds; }
  if (e.b_frames >= 0) { j["b_frames"] = e.b_frames > 0; }
  if (!e.profile.empty()) { j["profile"] = e.profile; }
  if (!e.level.empty()) { j["level"] = e.level; }
  if (!e.entropy.empty()) { j["entropy"] = e.entropy; }
  if (!e.prores.empty()) { j["prores"] = e.prores; }
  return j;
}

VideoEncoding
video_encoding_from_json(const Json& j)
{
  VideoEncoding e;
  if (!j.is_object()) {
    return e;
  }
  e.bitrate = std::max<std::int64_t>(0, jget<std::int64_t>(j, "bitrate", 0));
  e.max_bitrate = std::max<std::int64_t>(
      0, jget<std::int64_t>(j, "max_bitrate", 0));
  e.quality = std::clamp(jget(j, "quality", 0.0), 0.0, 1.0);
  e.keyframe_seconds = std::max(0.0, jget(j, "keyframe_seconds", 0.0));
  if (j.contains("b_frames") && j["b_frames"].is_boolean()) {
    e.b_frames = j["b_frames"].get<bool>() ? 1 : 0;
  }
  e.profile = jget<std::string>(j, "profile", "");
  e.level = jget<std::string>(j, "level", "");
  e.entropy = jget<std::string>(j, "entropy", "");
  e.prores = jget<std::string>(j, "prores", "");
  if (e.level == "auto") { e.level.clear(); }
  if (e.entropy == "auto") { e.entropy.clear(); }
  return e;
}

std::string
video_encoding_problem(const VideoEncoding& e, std::string_view format)
{
  const bool prores = format.starts_with("prores");
  const bool h264 = format == "h264";
  if (!e.prores.empty()) {
    static constexpr std::string_view k4444[] = {"4444", "4444xq"};
    static constexpr std::string_view k422[] = {"422hq", "422", "422lt",
                                                "422proxy"};
    const auto in = [&](auto& list) {
      return std::ranges::find(list, e.prores) != std::end(list);
    };
    if (!((format == "prores4444" && in(k4444)) ||
          (format == "prores422hq" && in(k422)))) {
      return std::format("no ProRes '{}' as {}", e.prores, format);
    }
  }
  if (prores && (e.bitrate > 0 || e.max_bitrate > 0 || e.quality > 0 ||
                 e.keyframe_seconds > 0 || e.b_frames >= 0 ||
                 !e.profile.empty())) {
    return "ProRes has no bitrate, keyframes or profile to set";
  }
  if (format == "hevc10" && !e.profile.empty() && e.profile != "main10" &&
      e.profile != "main") {
    return std::format("no HEVC profile '{}'", e.profile);
  }
  if (!h264 && (!e.level.empty() || !e.entropy.empty())) {
    return "a level and entropy coding are H.264's";
  }
  if (h264) {
    static constexpr std::string_view kProfiles[] = {"", "baseline", "main",
                                                     "high"};
    static constexpr std::string_view kLevels[] = {
        "", "3.0", "3.1", "3.2", "4.0", "4.1", "4.2", "5.0", "5.1", "5.2"};
    if (std::ranges::find(kProfiles, e.profile) == std::end(kProfiles)) {
      return std::format("no H.264 profile '{}'", e.profile);
    }
    if (std::ranges::find(kLevels, e.level) == std::end(kLevels)) {
      return std::format("no H.264 level '{}'", e.level);
    }
    if (!e.entropy.empty() && e.entropy != "cabac" && e.entropy != "cavlc") {
      return std::format("no entropy coding '{}'", e.entropy);
    }
    if (e.profile == "baseline" && (e.entropy == "cabac" || e.b_frames > 0)) {
      return "H.264 baseline has neither CABAC nor B-frames";
    }
  }
  if (e.max_bitrate > 0 && e.bitrate > e.max_bitrate) {
    return "the average bitrate is above the peak";
  }
  return {};
}

namespace {

// H.264's levels (ITU-T H.264 Table A-1): the most 16 x 16 macroblocks a
// frame holds, and a frame of that size as delivery specs name it.
struct H264Level {
  std::string_view name;
  std::int64_t     max_fs;
  std::string_view example;
};
constexpr H264Level kH264Levels[] = {
    {"3.0", 1620, "720 x 576"},    {"3.1", 3600, "1280 x 720"},
    {"3.2", 5120, "1280 x 1024"},  {"4.0", 8192, "2048 x 1024"},
    {"4.1", 8192, "2048 x 1024"},  {"4.2", 8704, "2048 x 1080"},
    {"5.0", 22080, "2560 x 1920"}, {"5.1", 36864, "4096 x 2304"},
    {"5.2", 36864, "4096 x 2304"},
};

// Whether a frame of `mbs` macroblocks, `w` x `h` of them, fits: its
// count, and each side within sqrt(8 x MaxFS) (Table A-1's note).
bool
h264_level_holds(const H264Level& l, std::int64_t w, std::int64_t h)
{
  std::int64_t side = 1;
  while ((side + 1) * (side + 1) <= 8 * l.max_fs) {
    ++side;
  }
  return w * h <= l.max_fs && w <= side && h <= side;
}

}  // namespace

std::string
h264_level_problem(std::string_view level, std::int64_t width,
                   std::int64_t height)
{
  if (level.empty() || level == "auto" || width <= 0 || height <= 0) {
    return {};
  }
  const std::int64_t w = (width + 15) / 16, h = (height + 15) / 16;
  const H264Level* at = nullptr;
  for (const H264Level& l : kH264Levels) {
    if (l.name == level) {
      at = &l;
    }
  }
  if (at == nullptr || h264_level_holds(*at, w, h)) {
    return {};
  }
  const H264Level* fits = nullptr;
  for (const H264Level& l : kH264Levels) {
    if (fits == nullptr && h264_level_holds(l, w, h)) {
      fits = &l;
    }
  }
  const std::string frame = std::format(
      "H.264 level {} holds a frame of {} macroblocks at most ({}); this "
      "movie's {} x {} is {}", at->name, at->max_fs, at->example, width,
      height, w * h);
  return fits != nullptr
      ? std::format("{}: choose level {} or Automatic", frame, fits->name)
      : std::format("{}: no H.264 level holds it -- choose HEVC or "
                    "ProRes", frame);
}

std::string
video_codec(std::string_view format, const VideoEncoding& e)
{
  if (format.starts_with("prores") && !e.prores.empty()) {
    return "prores" + e.prores;
  }
  if (format == "hevc10") {
    return e.profile == "main" ? "hevc8" : "hevc";
  }
  return std::string(format);
}

std::unique_ptr<Engine>
make_default_engine(const EngineConfig& cfg)
{
#if VALTZ_HAVE_VPIPE
  return make_vpipe_engine(cfg);
#else
  return make_null_engine();
#endif
}

#if !VALTZ_HAVE_VPIPE
std::unique_ptr<Engine>
make_vpipe_engine(const EngineConfig&)
{
  return make_null_engine();
}

std::string
linked_engine_software()
{
  return {};
}
#endif

}
