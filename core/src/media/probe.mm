#include "valtz/media/probe.h"
#include "valtz/media/exif.h"

#import <AVFoundation/AVFoundation.h>
#import <ColorSync/ColorSync.h>
#import <CoreMedia/CoreMedia.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <cmath>
#include <format>

namespace valtz::media {

namespace {

std::string
to_std(CFStringRef s)
{
  if (!s) {
    return {};
  }
  return std::string([(__bridge NSString*)s UTF8String] ?: "");
}

std::string
fourcc_str(FourCharCode c)
{
  std::string s(4, ' ');
  for (int i = 0; i < 4; ++i) {
    char ch = static_cast<char>((c >> (8 * (3 - i))) & 0xff);
    s[i] = (ch >= 32 && ch < 127) ? ch : '?';
  }
  return s;
}

bool
cf_equal(CFTypeRef a, CFStringRef b)
{
  return a && CFGetTypeID(a) == CFStringGetTypeID() &&
         CFStringCompare(static_cast<CFStringRef>(a), b, 0) ==
             kCFCompareEqualTo;
}

// ---- CGColorSpace (stills) ------------------------------------------

void
color_from_cg(CGColorSpaceRef cs, ColorInfo& out)
{
  if (!cs) {
    return;
  }
  CFStringRef name = CGColorSpaceGetName(cs);
  struct Map {
    CFStringRef name;
    Primaries   p;
    Transfer    t;
  };
  const Map maps[] = {
    {kCGColorSpaceSRGB, Primaries::BT709, Transfer::SRGB},
    {kCGColorSpaceExtendedSRGB, Primaries::BT709, Transfer::SRGB},
    {kCGColorSpaceLinearSRGB, Primaries::BT709, Transfer::Linear},
    {kCGColorSpaceExtendedLinearSRGB, Primaries::BT709, Transfer::Linear},
    {kCGColorSpaceDisplayP3, Primaries::SMPTE432, Transfer::SRGB},
    {kCGColorSpaceExtendedDisplayP3, Primaries::SMPTE432, Transfer::SRGB},
    {kCGColorSpaceLinearDisplayP3, Primaries::SMPTE432, Transfer::Linear},
    {kCGColorSpaceExtendedLinearDisplayP3, Primaries::SMPTE432,
     Transfer::Linear},
    {kCGColorSpaceITUR_709, Primaries::BT709, Transfer::BT709},
    {kCGColorSpaceITUR_2020, Primaries::BT2020, Transfer::BT2020_10},
    {kCGColorSpaceITUR_2100_PQ, Primaries::BT2020, Transfer::PQ},
    {kCGColorSpaceITUR_2100_HLG, Primaries::BT2020, Transfer::HLG},
    {kCGColorSpaceLinearITUR_2020, Primaries::BT2020, Transfer::Linear},
    {kCGColorSpaceExtendedLinearITUR_2020, Primaries::BT2020,
     Transfer::Linear},
  };
  if (name) {
    for (const auto& m : maps) {
      if (CFStringCompare(name, m.name, 0) == kCFCompareEqualTo) {
        out.primaries = m.p;
        out.transfer = m.t;
        out.matrix = Matrix::Identity;
        out.range = Range::Full;
        return;
      }
    }
    out.icc_name = to_std(name);
    return;
  }
  // An embedded profile with no system name: keep its description.
  CFDataRef icc = CGColorSpaceCopyICCData(cs);
  if (!icc) {
    return;
  }
  ColorSyncProfileRef prof = ColorSyncProfileCreate(icc, nullptr);
  CFRelease(icc);
  if (!prof) {
    out.icc_name = "embedded ICC";
    return;
  }
  CFStringRef desc = ColorSyncProfileCopyDescriptionString(prof);
  out.icc_name = desc ? to_std(desc) : "embedded ICC";
  if (desc) {
    CFRelease(desc);
  }
  CFRelease(prof);
}

AlphaMode
alpha_from_cg(CGImageAlphaInfo a)
{
  switch (a) {
  case kCGImageAlphaPremultipliedLast:
  case kCGImageAlphaPremultipliedFirst:
    return AlphaMode::Premultiplied;
  case kCGImageAlphaLast:
  case kCGImageAlphaFirst:
  case kCGImageAlphaOnly:
    return AlphaMode::Straight;
  default:
    return AlphaMode::None;
  }
}

// A person's name for a still's format, from ImageIO's type.
std::string
still_format_name(const std::string& uti)
{
  static const std::pair<const char*, const char*> k[] = {
    {"public.png", "PNG"},
    {"public.tiff", "TIFF"},
    {"com.ilm.openexr-image", "OpenEXR"},
    {"public.jpeg", "JPEG"},
    {"public.heic", "HEIC"},
    {"public.heif", "HEIF"},
    {"public.avif", "AVIF"},
    {"org.webmproject.webp", "WebP"},
    {"com.compuserve.gif", "GIF"},
    {"com.microsoft.bmp", "BMP"},
    {"com.adobe.photoshop-image", "Photoshop"},
    {"com.adobe.raw-image", "DNG"},
  };
  for (const auto& [id, name] : k) {
    if (uti == id) { return name; }
  }
  // Camera raw and the rest: the type's own description.
  UTType* t = [UTType typeWithIdentifier:@(uti.c_str())];
  return t.localizedDescription ? std::string(t.localizedDescription.UTF8String)
                                : uti;
}

// A person's name for a video codec; empty for one not named here.
std::string
video_codec_name(FourCharCode sub, int bits)
{
  switch (sub) {
  case kCMVideoCodecType_AppleProRes4444XQ: return "Apple ProRes 4444 XQ";
  case kCMVideoCodecType_AppleProRes4444:   return "Apple ProRes 4444";
  case kCMVideoCodecType_AppleProRes422HQ:  return "Apple ProRes 422 HQ";
  case kCMVideoCodecType_AppleProRes422:    return "Apple ProRes 422";
  case kCMVideoCodecType_AppleProRes422LT:  return "Apple ProRes 422 LT";
  case kCMVideoCodecType_AppleProRes422Proxy:
    return "Apple ProRes 422 Proxy";
  case kCMVideoCodecType_HEVC:
  case 'hev1':
    return bits > 8 ? "HEVC Main 10" : "HEVC";
  case kCMVideoCodecType_H264:              return "H.264";
  default:                                  return "";
  }
}

Result<MediaInfo>
probe_image(NSURL* url)
{
  CGImageSourceRef src = CGImageSourceCreateWithURL(
      (__bridge CFURLRef)url, nullptr);
  if (!src) {
    return make_error(Code::Unsupported, "not a readable image");
  }
  MediaInfo mi;
  mi.type = MediaType::Image;
  mi.uti = to_std(CGImageSourceGetType(src));
  mi.image_count = static_cast<std::int32_t>(CGImageSourceGetCount(src));
  if (mi.image_count < 1) {
    CFRelease(src);
    return make_error(Code::Corrupt, "image has no frames");
  }

  NSDictionary* props = CFBridgingRelease(
      CGImageSourceCopyPropertiesAtIndex(src, 0, nullptr));
  mi.frame.width = [props[(id)kCGImagePropertyPixelWidth] intValue];
  mi.frame.height = [props[(id)kCGImagePropertyPixelHeight] intValue];
  int depth = [props[(id)kCGImagePropertyDepth] intValue];
  bool is_float = [props[(id)kCGImagePropertyIsFloat] boolValue];
  mi.frame.bits_per_component = static_cast<std::uint8_t>(
      depth > 0 ? depth : 8);
  if (is_float) {
    mi.frame.format = depth > 16 ? PixelFormat::RGBAFloat
                                 : PixelFormat::RGBAHalf;
  } else {
    mi.frame.format = depth > 8 ? PixelFormat::RGBA16 : PixelFormat::RGBA8;
  }
  mi.codec = mi.uti;
  mi.codec_name = still_format_name(mi.uti);

  // Creating the CGImage does not decode pixels; it exposes the color
  // space and alpha layout ImageIO will deliver.
  CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
  if (img) {
    color_from_cg(CGImageGetColorSpace(img), mi.frame.color);
    mi.frame.alpha = alpha_from_cg(CGImageGetAlphaInfo(img));
    CGImageRelease(img);
  }
  if (mi.frame.alpha == AlphaMode::None &&
      [props[(id)kCGImagePropertyHasAlpha] boolValue]) {
    mi.frame.alpha = AlphaMode::Straight;
  }

  // HDR stills: Apple's gain map or the ISO 21496-1 one.
  CFDictionaryRef gm = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
      src, 0, kCGImageAuxiliaryDataTypeHDRGainMap);
  if (!gm) {
    gm = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        src, 0, kCGImageAuxiliaryDataTypeISOGainMap);
  }
  if (gm) {
    mi.has_hdr_gain_map = true;
    CFRelease(gm);
  }
  CFRelease(src);
  return mi;
}

// ---- CMFormatDescription (movies) -----------------------------------

void
color_from_cm(CMFormatDescriptionRef fd, ColorInfo& c)
{
  CFTypeRef p = CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_ColorPrimaries);
  if (cf_equal(p, kCMFormatDescriptionColorPrimaries_ITU_R_709_2)) {
    c.primaries = Primaries::BT709;
  } else if (cf_equal(p, kCMFormatDescriptionColorPrimaries_ITU_R_2020)) {
    c.primaries = Primaries::BT2020;
  } else if (cf_equal(p, kCMFormatDescriptionColorPrimaries_P3_D65)) {
    c.primaries = Primaries::SMPTE432;
  } else if (cf_equal(p, kCMFormatDescriptionColorPrimaries_DCI_P3)) {
    c.primaries = Primaries::SMPTE431;
  } else if (cf_equal(p, kCMFormatDescriptionColorPrimaries_SMPTE_C)) {
    c.primaries = Primaries::SMPTE170M;
  } else if (cf_equal(p, kCMFormatDescriptionColorPrimaries_EBU_3213)) {
    c.primaries = Primaries::BT470BG;
  }

  CFTypeRef t = CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_TransferFunction);
  if (cf_equal(t, kCMFormatDescriptionTransferFunction_ITU_R_709_2)) {
    c.transfer = Transfer::BT709;
  } else if (cf_equal(t,
                      kCMFormatDescriptionTransferFunction_SMPTE_ST_2084_PQ)) {
    c.transfer = Transfer::PQ;
  } else if (cf_equal(t,
                      kCMFormatDescriptionTransferFunction_ITU_R_2100_HLG)) {
    c.transfer = Transfer::HLG;
  } else if (cf_equal(t, kCMFormatDescriptionTransferFunction_Linear)) {
    c.transfer = Transfer::Linear;
  } else if (cf_equal(t, kCMFormatDescriptionTransferFunction_sRGB)) {
    c.transfer = Transfer::SRGB;
  } else if (cf_equal(t, kCMFormatDescriptionTransferFunction_ITU_R_2020)) {
    c.transfer = Transfer::BT2020_10;
  } else if (cf_equal(t,
                      kCMFormatDescriptionTransferFunction_SMPTE_240M_1995)) {
    c.transfer = Transfer::SMPTE240M;
  }

  CFTypeRef m = CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_YCbCrMatrix);
  if (cf_equal(m, kCMFormatDescriptionYCbCrMatrix_ITU_R_709_2)) {
    c.matrix = Matrix::BT709;
  } else if (cf_equal(m, kCMFormatDescriptionYCbCrMatrix_ITU_R_601_4)) {
    c.matrix = Matrix::SMPTE170M;
  } else if (cf_equal(m, kCMFormatDescriptionYCbCrMatrix_ITU_R_2020)) {
    c.matrix = Matrix::BT2020NCL;
  } else if (cf_equal(m, kCMFormatDescriptionYCbCrMatrix_SMPTE_240M_1995)) {
    c.matrix = Matrix::SMPTE240M;
  }

  CFTypeRef full = CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_FullRangeVideo);
  if (full && CFGetTypeID(full) == CFBooleanGetTypeID()) {
    c.range = CFBooleanGetValue(static_cast<CFBooleanRef>(full))
                  ? Range::Full
                  : Range::Limited;
  } else if (c.matrix != Matrix::Unspecified) {
    c.range = Range::Limited;  // the default for tagged YCbCr video
  }

  // ST 2086 and CTA-861.3 payloads, big-endian as in the HEVC SEI.
  auto be16 = [](const std::uint8_t* b) {
    return static_cast<std::uint16_t>((b[0] << 8) | b[1]);
  };
  auto be32 = [](const std::uint8_t* b) {
    return (std::uint32_t{b[0]} << 24) | (std::uint32_t{b[1]} << 16) |
           (std::uint32_t{b[2]} << 8) | std::uint32_t{b[3]};
  };
  CFTypeRef mdcv = CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_MasteringDisplayColorVolume);
  if (mdcv && CFGetTypeID(mdcv) == CFDataGetTypeID() &&
      CFDataGetLength(static_cast<CFDataRef>(mdcv)) >= 24) {
    const std::uint8_t* b = CFDataGetBytePtr(static_cast<CFDataRef>(mdcv));
    MasteringDisplay md;
    // SEI order is G, B, R.
    md.green_x = be16(b + 0);
    md.green_y = be16(b + 2);
    md.blue_x = be16(b + 4);
    md.blue_y = be16(b + 6);
    md.red_x = be16(b + 8);
    md.red_y = be16(b + 10);
    md.white_x = be16(b + 12);
    md.white_y = be16(b + 14);
    md.max_luminance = be32(b + 16);
    md.min_luminance = be32(b + 20);
    c.mastering = md;
  }
  CFTypeRef clli = CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_ContentLightLevelInfo);
  if (clli && CFGetTypeID(clli) == CFDataGetTypeID() &&
      CFDataGetLength(static_cast<CFDataRef>(clli)) >= 4) {
    const std::uint8_t* b = CFDataGetBytePtr(static_cast<CFDataRef>(clli));
    c.content_light = ContentLight{be16(b), be16(b + 2)};
  }
}

// AVFoundation loads asynchronously. Probing runs on a worker thread, so
// block on each load; never call these from the main thread.

struct TrackLoad {
  NSArray<AVAssetTrack*>* tracks = nil;
  NSError*                error = nil;
};

TrackLoad
load_tracks(AVAsset* asset, AVMediaType type)
{
  TrackLoad out;
  TrackLoad* outp = &out;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [asset loadTracksWithMediaType:type
               completionHandler:^(NSArray<AVAssetTrack*>* t, NSError* e) {
                 outp->tracks = t;
                 outp->error = e;
                 dispatch_semaphore_signal(sem);
               }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  return out;
}

void
load_keys(id<AVAsynchronousKeyValueLoading> obj, NSArray<NSString*>* keys)
{
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [obj loadValuesAsynchronouslyForKeys:keys
                     completionHandler:^{
                       dispatch_semaphore_signal(sem);
                     }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
}

Result<MediaInfo>
probe_movie(NSURL* url)
{
  AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{
    AVURLAssetPreferPreciseDurationAndTimingKey : @YES
  }];
  TrackLoad v = load_tracks(asset, AVMediaTypeVideo);
  if (v.error) {
    return make_error(Code::Unsupported,
                      std::format("cannot read movie: {}",
                                  [[v.error localizedDescription]
                                      UTF8String]));
  }
  NSArray<AVAssetTrack*>* vtracks = v.tracks;
  NSArray<AVAssetTrack*>* atracks =
      load_tracks(asset, AVMediaTypeAudio).tracks;
  load_keys(asset, @[ @"duration" ]);

  MediaInfo mi;
  mi.type = vtracks.count ? MediaType::Video : MediaType::Audio;
  CMTime d = asset.duration;
  if (CMTIME_IS_NUMERIC(d)) {
    mi.duration = MediaTime{d.value, d.timescale};
  }

  if (vtracks.count) {
    AVAssetTrack* t = vtracks.firstObject;
    load_keys(t, @[
      @"naturalSize", @"formatDescriptions", @"nominalFrameRate",
      @"minFrameDuration", @"timeRange"
    ]);
    CGSize sz = t.naturalSize;
    mi.frame.width = static_cast<std::int32_t>(sz.width);
    mi.frame.height = static_cast<std::int32_t>(sz.height);

    // Exact rate for constant-rate media; the float average otherwise.
    CMTime mfd = t.minFrameDuration;
    if (CMTIME_IS_NUMERIC(mfd) && mfd.value > 0) {
      mi.frame_rate = Rational(mfd.timescale, mfd.value);
    } else if (t.nominalFrameRate > 0) {
      mi.frame_rate = Rational(
          std::llround(t.nominalFrameRate * 1000.0), 1000);
    }
    // Counted over the PICTURE: a movie's duration is its longest track,
    // and a soundtrack that runs on past the last frame would count frames
    // that do not exist.
    const CMTime picture = t.timeRange.duration;
    const double secs = CMTIME_IS_NUMERIC(picture) && picture.value > 0
                            ? CMTimeGetSeconds(picture)
                        : mi.duration.valid() ? mi.duration.seconds()
                                              : 0;
    if (mi.frame_rate.num > 0 && secs > 0) {
      mi.frame_count = std::llround(secs * mi.frame_rate.to_double());
    }

    auto fd = (__bridge CMFormatDescriptionRef)
                  t.formatDescriptions.firstObject;
    if (fd) {
      FourCharCode sub = CMFormatDescriptionGetMediaSubType(fd);
      mi.codec = fourcc_str(sub);
      CFTypeRef fname = CMFormatDescriptionGetExtension(
          fd, kCMFormatDescriptionExtension_FormatName);
      mi.codec_name = fname && CFGetTypeID(fname) == CFStringGetTypeID()
                          ? to_std(static_cast<CFStringRef>(fname))
                          : mi.codec;
      color_from_cm(fd, mi.frame.color);

      CFTypeRef bpc = CMFormatDescriptionGetExtension(
          fd, kCMFormatDescriptionExtension_BitsPerComponent);
      int bits = 8;
      if (bpc && CFGetTypeID(bpc) == CFNumberGetTypeID()) {
        CFNumberGetValue(static_cast<CFNumberRef>(bpc), kCFNumberIntType,
                         &bits);
      }
      // ProRes carries its depth in the codec, not an extension.
      bool prores_4444 = sub == kCMVideoCodecType_AppleProRes4444 ||
                         sub == kCMVideoCodecType_AppleProRes4444XQ;
      bool prores = prores_4444 ||
                    sub == kCMVideoCodecType_AppleProRes422 ||
                    sub == kCMVideoCodecType_AppleProRes422HQ ||
                    sub == kCMVideoCodecType_AppleProRes422LT ||
                    sub == kCMVideoCodecType_AppleProRes422Proxy;
      if (prores) {
        bits = prores_4444 ? 12 : 10;
      }
      mi.frame.bits_per_component = static_cast<std::uint8_t>(bits);
      // The codec's name, not the encoder's: FFmpeg's files name their
      // encoder ("Lavc63.1.101 libx265") in the format description.
      if (std::string n = video_codec_name(sub, bits); !n.empty()) {
        mi.codec_name = n;
      }

      // Alpha: an explicit flag, or ProRes 4444 at depth 32.
      bool alpha = false;
      CFTypeRef ca = CMFormatDescriptionGetExtension(
          fd, kCMFormatDescriptionExtension_ContainsAlphaChannel);
      if (ca && CFGetTypeID(ca) == CFBooleanGetTypeID()) {
        alpha = CFBooleanGetValue(static_cast<CFBooleanRef>(ca));
      }
      CFTypeRef depth = CMFormatDescriptionGetExtension(
          fd, kCMFormatDescriptionExtension_Depth);
      int depth_v = 0;
      if (depth && CFGetTypeID(depth) == CFNumberGetTypeID()) {
        CFNumberGetValue(static_cast<CFNumberRef>(depth), kCFNumberIntType,
                         &depth_v);
      }
      if (prores_4444 && depth_v == 32) {
        alpha = true;
      }
      if (alpha) {
        CFTypeRef mode = CMFormatDescriptionGetExtension(
            fd, kCMFormatDescriptionExtension_AlphaChannelMode);
        mi.frame.alpha =
            cf_equal(mode,
                     kCMFormatDescriptionAlphaChannelMode_PremultipliedAlpha)
                ? AlphaMode::Premultiplied
                : AlphaMode::Straight;
      }

      // The format a decoder would natively hand back.
      if (prores_4444) {
        mi.frame.format = PixelFormat::AYCbCr4444_16;
      } else if (prores) {
        mi.frame.format = PixelFormat::YCbCr422_10;
      } else {
        mi.frame.format = bits > 8 ? PixelFormat::YCbCr420_10
                                   : PixelFormat::YCbCr420_8;
      }
    }
  }

  if (atracks.count) {
    mi.has_audio = true;
    AVAssetTrack* a = atracks.firstObject;
    load_keys(a, @[ @"formatDescriptions" ]);
    auto afd = (__bridge CMAudioFormatDescriptionRef)
                   a.formatDescriptions.firstObject;
    if (afd) {
      const AudioStreamBasicDescription* asbd =
          CMAudioFormatDescriptionGetStreamBasicDescription(afd);
      if (asbd) {
        mi.audio_channels = static_cast<std::int32_t>(asbd->mChannelsPerFrame);
        mi.audio_sample_rate = static_cast<std::int32_t>(asbd->mSampleRate);
      }
    }
  }
  return mi;
}

}

MediaType
media_type_for_path(const std::filesystem::path& path)
{
  @autoreleasepool {
    NSString* ext = [NSString stringWithUTF8String:
                                  path.extension().string().c_str()];
    if (ext.length > 0) {
      ext = [ext substringFromIndex:1];
    }
    UTType* t = [UTType typeWithFilenameExtension:ext];
    if (!t) {
      return MediaType::Unknown;
    }
    if ([t conformsToType:UTTypeImage]) {
      return MediaType::Image;
    }
    if ([t conformsToType:UTTypeMovie] ||
        [t conformsToType:UTTypeVideo]) {
      return MediaType::Video;
    }
    if ([t conformsToType:UTTypeAudio]) {
      return MediaType::Audio;
    }
    if ([t conformsToType:UTTypeText]) {
      return MediaType::Text;
    }
    return MediaType::Unknown;
  }
}

Result<MediaInfo>
probe_file(const std::filesystem::path& path)
{
  @autoreleasepool {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
      return make_error(Code::NotFound,
                        std::format("no such file: {}", path.string()));
    }
    NSURL* url = [NSURL fileURLWithPath:
                            [NSString stringWithUTF8String:path.c_str()]];
    MediaType t = media_type_for_path(path);
    Result<MediaInfo> r = make_error(Code::Unsupported, "");
    switch (t) {
    case MediaType::Image:
      r = probe_image(url);
      if (r.ok()) {
        r->exif = exif_fields(path);
      }
      break;
    case MediaType::Video:
    case MediaType::Audio:
      r = probe_movie(url);
      break;
    case MediaType::Text: {
      MediaInfo mi;
      mi.type = MediaType::Text;
      mi.uti = "public.plain-text";
      return mi;
    }
    case MediaType::Unknown:
      // Try the image reader: ImageIO sniffs content, not extensions.
      r = probe_image(url);
      if (r.ok()) {
        r->exif = exif_fields(path);
      }
      break;
    }
    if (!r.ok() && r.code() == Code::Unsupported) {
      return make_error(Code::Unsupported,
                        std::format("unsupported media: {}",
                                    path.filename().string()));
    }
    return r;
  }
}

}
