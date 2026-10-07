#include "valtz/media/format.h"

#include <CoreVideo/CoreVideo.h>

#include <array>

namespace valtz::media {

namespace {

constexpr std::array<PixelFormatInfo, 16> kInfo = {{
  //  name              ch bits float  yuv    alpha  tensor fourcc
  {"unknown",           0, 0,  false, false, false, false, 0},
  {"BGRA8",             4, 8,  false, false, true,  false,
   kCVPixelFormatType_32BGRA},
  {"RGBA8",             4, 8,  false, false, true,  false,
   kCVPixelFormatType_32RGBA},
  {"RGBA16",            4, 16, false, false, true,  false,
   kCVPixelFormatType_64RGBALE},
  {"RGBAHalf",          4, 16, true,  false, true,  false,
   kCVPixelFormatType_64RGBAHalf},
  {"RGBAFloat",         4, 32, true,  false, true,  false,
   kCVPixelFormatType_128RGBAFloat},
  {"YCbCr420_8",        3, 8,  false, true,  false, false,
   kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange},
  {"YCbCr420_10",       3, 10, false, true,  false, false,
   kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange},
  {"YCbCr422_10",       3, 10, false, true,  false, false,
   kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange},
  {"YCbCr444_10",       3, 10, false, true,  false, false,
   kCVPixelFormatType_444YpCbCr10BiPlanarVideoRange},
  {"AYCbCr4444_16",     4, 16, false, true,  true,  false,
   kCVPixelFormatType_4444AYpCbCr16},
  {"PlanarRGB8",        3, 8,  false, false, false, true, 0},
  {"PlanarRGBA8",       4, 8,  false, false, true,  true, 0},
  {"PlanarRGBHalf",     3, 16, true,  false, false, true, 0},
  {"PlanarRGBAHalf",    4, 16, true,  false, true,  true, 0},
  {"unknown",           0, 0,  false, false, false, false, 0},
}};

}

const PixelFormatInfo&
info(PixelFormat f)
{
  auto i = static_cast<std::size_t>(f);
  return i < kInfo.size() - 1 ? kInfo[i] : kInfo[0];
}

PixelFormat
from_cv_fourcc(std::uint32_t fourcc)
{
  switch (fourcc) {
  case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange:
    return PixelFormat::YCbCr420_8;
  case kCVPixelFormatType_420YpCbCr10BiPlanarFullRange:
    return PixelFormat::YCbCr420_10;
  case kCVPixelFormatType_422YpCbCr10BiPlanarFullRange:
    return PixelFormat::YCbCr422_10;
  case kCVPixelFormatType_444YpCbCr10BiPlanarFullRange:
    return PixelFormat::YCbCr444_10;
  default:
    break;
  }
  for (std::size_t i = 1; i + 1 < kInfo.size(); ++i) {
    if (kInfo[i].cv_fourcc && kInfo[i].cv_fourcc == fourcc) {
      return static_cast<PixelFormat>(i);
    }
  }
  return PixelFormat::Unknown;
}

const char*
to_str(MediaType t)
{
  switch (t) {
  case MediaType::Unknown: return "unknown";
  case MediaType::Image:   return "image";
  case MediaType::Video:   return "video";
  case MediaType::Audio:   return "audio";
  case MediaType::Text:    return "text";
  }
  return "?";
}

}
