#include "valtz/media/exif.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <array>
#include <optional>
#include <string_view>

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

NSDictionary*
image_properties(const fs::path& src)
{
  NSURL* url = [NSURL fileURLWithPath:@(src.c_str())];
  CGImageSourceRef s = CGImageSourceCreateWithURL((__bridge CFURLRef)url,
                                                  nullptr);
  if (!s) {
    return nil;
  }
  NSDictionary* props = CFBridgingRelease(
      CGImageSourceCopyPropertiesAtIndex(s, 0, nullptr));
  CFRelease(s);
  return props;
}

// A property as JSON: strings and numbers as they are; an array as its
// first element (ISO), which is the value a person reads.
Json
to_json_value(id v)
{
  if ([v isKindOfClass:[NSString class]]) {
    NSString* s = v;
    return s.length ? Json(std::string(s.UTF8String)) : Json();
  }
  if ([v isKindOfClass:[NSNumber class]]) {
    NSNumber* n = v;
    const char* t = n.objCType;
    if (t[0] == 'f' || t[0] == 'd') {
      return n.doubleValue;
    }
    return n.longLongValue;
  }
  if ([v isKindOfClass:[NSArray class]]) {
    NSArray* a = v;
    return a.count ? to_json_value(a[0]) : Json();
  }
  return Json();
}

void
copy_fields(NSDictionary* from, Json& to,
            std::initializer_list<std::string_view> keys)
{
  if (![from isKindOfClass:[NSDictionary class]]) {
    return;
  }
  for (auto k : keys) {
    NSString* key = [[NSString alloc] initWithBytes:k.data()
                                             length:k.size()
                                           encoding:NSUTF8StringEncoding];
    if (id v = from[key]) {
      if (Json j = to_json_value(v); !j.is_null()) {
        to[std::string(k)] = std::move(j);
      }
    }
  }
}

// ---- a TIFF block, in place ------------------------------------------

// The Exif sub-IFD's pixel dimensions, rewritten in a TIFF block of
// either byte order. ImageIO records the size of the image it wrote --
// the one-pixel carrier -- so the real size is put back here.
bool
set_pixel_dimensions(std::vector<std::uint8_t>& t, PixelSize size)
{
  if (t.size() < 8) {
    return false;
  }
  const bool le = t[0] == 'I' && t[1] == 'I';
  if (!le && !(t[0] == 'M' && t[1] == 'M')) {
    return false;
  }
  auto rd = [&](std::size_t at, int n) -> std::uint32_t {
    std::uint32_t v = 0;
    for (int i = 0; i < n; ++i) {
      const std::uint32_t b = t[at + (le ? i : n - 1 - i)];
      v |= b << (8 * i);
    }
    return v;
  };
  auto wr = [&](std::size_t at, int n, std::uint32_t v) {
    for (int i = 0; i < n; ++i) {
      t[at + (le ? i : n - 1 - i)] = static_cast<std::uint8_t>(v >> (8 * i));
    }
  };
  // The 12-byte entry for `tag` in the IFD at `ifd`.
  auto entry = [&](std::size_t ifd, std::uint16_t tag)
      -> std::optional<std::size_t> {
    if (ifd + 2 > t.size()) {
      return std::nullopt;
    }
    const std::uint32_t n = rd(ifd, 2);
    for (std::uint32_t i = 0; i < n; ++i) {
      const std::size_t e = ifd + 2 + 12 * i;
      if (e + 12 > t.size()) {
        break;
      }
      if (rd(e, 2) == tag) {
        return e;
      }
    }
    return std::nullopt;
  };
  auto exif = entry(rd(4, 4), 0x8769);
  if (!exif) {
    return false;
  }
  const std::size_t sub = rd(*exif + 8, 4);
  const std::array<std::pair<std::uint16_t, std::int32_t>, 2> dims = {{
      {0xA002, size.width}, {0xA003, size.height}}};
  for (auto [tag, v] : dims) {
    auto e = entry(sub, tag);
    if (!e || rd(*e + 4, 4) != 1) {
      continue;
    }
    // SHORT (3) or LONG (4), one value, held in the entry itself (a
    // SHORT left-justified in the four bytes).
    const std::uint32_t type = rd(*e + 2, 2);
    if (type == 3 && v <= 0xFFFF) {
      wr(*e + 8, 2, static_cast<std::uint32_t>(v));
    } else if (type == 4) {
      wr(*e + 8, 4, static_cast<std::uint32_t>(v));
    }
  }
  return true;
}

// The payload of `jpeg`'s APP1 "Exif\0\0" segment: the TIFF block.
std::vector<std::uint8_t>
app1_exif(NSData* jpeg)
{
  const auto* b = static_cast<const std::uint8_t*>(jpeg.bytes);
  const std::size_t n = jpeg.length;
  static constexpr std::uint8_t kSig[6] = {'E', 'x', 'i', 'f', 0, 0};
  std::size_t i = 2;
  while (i + 4 <= n && b[i] == 0xFF) {
    const std::uint8_t m = b[i + 1];
    const std::size_t len = (std::size_t(b[i + 2]) << 8) | b[i + 3];
    if (m == 0xDA || len < 2 || i + 2 + len > n) {
      break;  // the image data, or a broken segment
    }
    if (m == 0xE1 && len >= 8 && std::equal(kSig, kSig + 6, b + i + 4)) {
      return {b + i + 10, b + i + 2 + len};
    }
    i += 2 + len;
  }
  return {};
}

}

Json
exif_fields(const fs::path& src)
{
  Json out = Json::object();
  @autoreleasepool {
    NSDictionary* p = image_properties(src);
    if (!p) {
      return out;
    }
    copy_fields(p[(id)kCGImagePropertyTIFFDictionary], out,
                {"Make", "Model", "Software", "DateTime", "Artist",
                 "Copyright", "ImageDescription"});
    copy_fields(p[(id)kCGImagePropertyExifDictionary], out,
                {"DateTimeOriginal", "LensMake", "LensModel",
                 "ExposureTime", "FNumber", "ISOSpeedRatings",
                 "ExposureBiasValue", "FocalLength", "FocalLenIn35mmFilm",
                 "ExposureProgram", "MeteringMode", "Flash",
                 "WhiteBalance"});
    // A PNG without an eXIf chunk may still say who wrote it.
    if (!out.contains("Software")) {
      copy_fields(p[(id)kCGImagePropertyPNGDictionary], out, {"Software"});
    }
    NSDictionary* gps = p[(id)kCGImagePropertyGPSDictionary];
    if ([gps isKindOfClass:[NSDictionary class]]) {
      auto coord = [&](CFStringRef value, CFStringRef ref,
                       const char* negative, const char* key) {
        NSNumber* v = gps[(__bridge id)value];
        NSString* r = gps[(__bridge id)ref];
        if (![v isKindOfClass:[NSNumber class]]) {
          return;
        }
        const bool neg = [r isKindOfClass:[NSString class]] &&
                         [r isEqualToString:@(negative)];
        out[key] = neg ? -v.doubleValue : v.doubleValue;
      };
      coord(kCGImagePropertyGPSLatitude, kCGImagePropertyGPSLatitudeRef,
            "S", "GPSLatitude");
      coord(kCGImagePropertyGPSLongitude, kCGImagePropertyGPSLongitudeRef,
            "W", "GPSLongitude");
      if (NSNumber* alt = gps[(id)kCGImagePropertyGPSAltitude];
          [alt isKindOfClass:[NSNumber class]]) {
        NSNumber* below = gps[(id)kCGImagePropertyGPSAltitudeRef];
        const bool neg = [below isKindOfClass:[NSNumber class]] &&
                         below.intValue == 1;
        out["GPSAltitude"] = neg ? -alt.doubleValue : alt.doubleValue;
      }
    }
  }
  return out;
}

std::vector<std::uint8_t>
exif_block_for(const fs::path& src, PixelSize size)
{
  @autoreleasepool {
    NSDictionary* p = image_properties(src);
    if (!p) {
      return {};
    }
    // What the file recorded, without what ImageIO reports of any
    // image -- its size and color space in {Exif}, its layout in {TIFF}
    // -- and without the turn: the result is upright, its pixels turned
    // as they were read.
    auto recorded = [](id d, std::initializer_list<CFStringRef> drop)
        -> NSMutableDictionary* {
      if (![d isKindOfClass:[NSDictionary class]]) {
        return nil;
      }
      NSMutableDictionary* m = [(NSDictionary*)d mutableCopy];
      for (CFStringRef k : drop) {
        [m removeObjectForKey:(__bridge id)k];
      }
      return m.count ? m : nil;
    };
    NSMutableDictionary* tiff = recorded(
        p[(id)kCGImagePropertyTIFFDictionary],
        {kCGImagePropertyTIFFOrientation, kCGImagePropertyTIFFXResolution,
         kCGImagePropertyTIFFYResolution,
         kCGImagePropertyTIFFResolutionUnit});
    NSMutableDictionary* exif = recorded(
        p[(id)kCGImagePropertyExifDictionary],
        {kCGImagePropertyExifPixelXDimension,
         kCGImagePropertyExifPixelYDimension,
         kCGImagePropertyExifColorSpace});
    NSMutableDictionary* gps = recorded(
        p[(id)kCGImagePropertyGPSDictionary], {});
    if (!tiff && !exif && !gps) {
      return {};
    }
    NSMutableDictionary* props = [NSMutableDictionary dictionary];
    if (tiff) {
      props[(id)kCGImagePropertyTIFFDictionary] = tiff;
    }
    // An Exif sub-IFD is always written, so the pixel dimensions have a
    // place to go.
    props[(id)kCGImagePropertyExifDictionary] =
        exif ?: [NSMutableDictionary dictionary];
    if (gps) {
      props[(id)kCGImagePropertyGPSDictionary] = gps;
    }
    props[(id)kCGImagePropertyOrientation] = @1;

    CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
    CGContextRef ctx = CGBitmapContextCreate(nullptr, 1, 1, 8, 0, gray,
                                             kCGImageAlphaNone);
    CGColorSpaceRelease(gray);
    if (!ctx) {
      return {};
    }
    CGImageRef pixel = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    NSMutableData* jpeg = [NSMutableData data];
    CGImageDestinationRef dst = CGImageDestinationCreateWithData(
        (__bridge CFMutableDataRef)jpeg,
        (__bridge CFStringRef)UTTypeJPEG.identifier, 1, nullptr);
    bool written = false;
    if (dst && pixel) {
      CGImageDestinationAddImage(dst, pixel,
                                 (__bridge CFDictionaryRef)props);
      written = CGImageDestinationFinalize(dst);
    }
    if (dst) {
      CFRelease(dst);
    }
    if (pixel) {
      CGImageRelease(pixel);
    }
    if (!written) {
      return {};
    }
    std::vector<std::uint8_t> block = app1_exif(jpeg);
    if (!block.empty()) {
      set_pixel_dimensions(block, size);
    }
    return block;
  }
}

}
