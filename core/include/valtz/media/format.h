// Pixel formats and media descriptors.
//
// Three families of pixel format meet in Valtz, and conversions happen
// only at the seams between them:
//
//   * CODEC-FACING (what VideoToolbox decodes to / encodes from):
//     8/10-bit bi-planar YCbCr, and 16-bit AYpCbCr for ProRes 4444/XQ
//     with its alpha plane.
//   * WORKING: RGBA half float, premultiplied, linear BT.2020
//     (media/color.h). Every edit and every on-screen pixel is this.
//   * MODEL-FACING: planar tensors as vpipe consumes/produces them
//     ([C,H,W], u8 or f16, straight alpha, sRGB-encoded).
//
// Frames live in IOSurface-backed CVPixelBuffers so the decoder, Metal,
// Core Animation and (later) a helper process can share them without a
// copy.

#ifndef VALTZ_MEDIA_FORMAT_H
#define VALTZ_MEDIA_FORMAT_H

#include "valtz/base/json.h"
#include "valtz/base/rational.h"
#include "valtz/media/color.h"

#include <cstdint>
#include <string>

namespace valtz::media {

// A picture's size in pixels.
struct PixelSize {
  std::int32_t width = 0;
  std::int32_t height = 0;

  bool operator==(const PixelSize&) const = default;
};

enum class PixelFormat : std::uint16_t {
  Unknown = 0,

  // Interleaved RGB(A)
  BGRA8,          // kCVPixelFormatType_32BGRA
  RGBA8,          // kCVPixelFormatType_32RGBA
  RGBA16,         // kCVPixelFormatType_64RGBALE (16-bit unorm)
  RGBAHalf,       // kCVPixelFormatType_64RGBAHalf -- the working format
  RGBAFloat,      // kCVPixelFormatType_128RGBAFloat

  // Codec-facing YCbCr
  YCbCr420_8,     // NV12, '420v' / '420f'
  YCbCr420_10,    // P010, 'x420' / 'xf20' -- HEVC Main10 / HDR
  YCbCr422_10,    // 'x422' -- ProRes 422 family
  YCbCr444_10,    // 'x444'
  AYCbCr4444_16,  // 'y416' (kCVPixelFormatType_4444AYpCbCr16) -- ProRes 4444

  // Model-facing planar tensors ([C,H,W])
  PlanarRGB8,
  PlanarRGBA8,
  PlanarRGBHalf,
  PlanarRGBAHalf,
};

struct PixelFormatInfo {
  const char*   name;
  std::uint8_t  channels;
  std::uint8_t  bits_per_component;
  bool          is_float;
  bool          is_ycbcr;
  bool          has_alpha;
  bool          is_planar_tensor;
  std::uint32_t cv_fourcc;  // 0 when there is no CoreVideo equivalent
};

const PixelFormatInfo& info(PixelFormat);
PixelFormat from_cv_fourcc(std::uint32_t);

// A still image or a single frame.
struct FrameDesc {
  std::int32_t width = 0;
  std::int32_t height = 0;
  PixelFormat  format = PixelFormat::Unknown;
  ColorInfo    color;
  AlphaMode    alpha = AlphaMode::None;
  Rational     pixel_aspect{1, 1};
  std::uint8_t bits_per_component = 8;  // as stored at the source
};

enum class MediaType : std::uint8_t {
  Unknown,
  Image,
  Video,
  Audio,
  Text,
};

const char* to_str(MediaType);

// What probing a file on import tells us. Everything a later stage needs
// to decide HOW to decode it is here, so a project can be browsed and
// planned without touching the media again.
struct MediaInfo {
  MediaType   type = MediaType::Unknown;
  std::string uti;           // e.g. "com.apple.quicktime-movie"
  std::string codec;         // FourCC or format name, e.g. "ap4h", "png"
  std::string codec_name;    // human-readable, e.g. "Apple ProRes 4444"
  FrameDesc   frame;         // first/representative frame
  // Video / audio
  Rational     frame_rate{0, 1};
  MediaTime    duration;
  std::int64_t frame_count = 0;
  bool         has_audio = false;
  std::int32_t audio_channels = 0;
  std::int32_t audio_sample_rate = 0;
  // Stills
  bool         has_hdr_gain_map = false;
  std::int32_t image_count = 1;  // >1 for multi-frame (HEIF sequence, GIF)
  // What the camera or editor recorded (media/exif.h: exif_fields): an
  // object, empty when there is nothing.
  Json         exif = Json::object();
};

}

#endif
