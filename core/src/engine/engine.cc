#include "valtz/engine/engine.h"

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
#endif

}
