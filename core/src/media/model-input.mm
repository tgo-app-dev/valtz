#include "valtz/media/model-input.h"
#include "valtz/base/log.h"
#include "valtz/media/layers.h"
#include "valtz/media/movie.h"
#include "valtz/media/probe.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <mutex>
#include <vector>

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

CGImageSourceRef
open_source(const fs::path& src)
{
  NSURL* url = [NSURL fileURLWithPath:@(src.c_str())];
  return CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
}

struct Stored {
  std::int32_t width = 0;
  std::int32_t height = 0;
  bool         swapped = false;  // EXIF orientation 5..8: a quarter turn
};

bool
stored_size(CGImageSourceRef s, Stored* out)
{
  NSDictionary* p = CFBridgingRelease(
      CGImageSourceCopyPropertiesAtIndex(s, 0, nullptr));
  NSNumber* w = p[(__bridge NSString*)kCGImagePropertyPixelWidth];
  NSNumber* h = p[(__bridge NSString*)kCGImagePropertyPixelHeight];
  NSNumber* o = p[(__bridge NSString*)kCGImagePropertyOrientation];
  if (!w || !h || w.intValue <= 0 || h.intValue <= 0) {
    return false;
  }
  out->width = w.intValue;
  out->height = h.intValue;
  out->swapped = o && o.intValue >= 5 && o.intValue <= 8;
  return true;
}

}

Result<PixelSize>
oriented_size(const fs::path& src)
{
  @autoreleasepool {
    CGImageSourceRef s = open_source(src);
    if (!s) {
      return make_error(Code::Io, std::format("cannot open {}",
                                              src.string()));
    }
    Stored st;
    const bool ok = stored_size(s, &st);
    CFRelease(s);
    if (!ok) {
      return make_error(Code::Corrupt, std::format(
          "{} has no readable image", src.string()));
    }
    return st.swapped ? PixelSize{st.height, st.width}
                      : PixelSize{st.width, st.height};
  }
}

namespace {

// A context for ONE decode, released with it. Not a shared one: a Core
// Image context keeps the Metal memory of its largest render for as long
// as it lives -- clearCaches does not give it back -- and the engine runs
// in this process beside vpipe, which budgets the same GPU working set.
// MEASURED: one 30 MP RAW development held 2.4 GB of it, and a Qwen-Image
// 2.1 edit of that RAW then failed its VAE decode with "0 MB free"; the
// same render in a context of its own returns all of it, for ~80 ms.
//
// Its working space is extended LINEAR sRGB -- said, not defaulted --
// because the adjustments are defined in it (media/adjust.h, and the app
// renders its preview in the same space) and because the final clamp to
// 0..1 is only the sRGB clamp in a space with sRGB's primaries.
// One, for the process: a context is thread-safe, and making one per
// picture -- per frame, in a long clip's export -- built its caches again
// each time.
CIContext*
decode_context()
{
  static CIContext* const c = [] {
    CGColorSpaceRef ws =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    CIContext* made = [CIContext contextWithOptions:@{
      kCIContextWorkingColorSpace : (__bridge id)ws,
      kCIContextWorkingFormat : @(kCIFormatRGBAh),
      kCIContextCacheIntermediates : @NO,
    }];
    CGColorSpaceRelease(ws);
    return made;
  }();
  return c;
}

// The picture as Core Image reads it, upright and colour-managed: a
// camera RAW DEVELOPED by its RAW engine in the camera's own look (as the
// stage shows it -- ImageIO renders a RAW the same way), anything else
// decoded with its EXIF orientation applied. A RAW is developed no larger
// than `need` (the scale the caller will draw it at, at most 1): the
// engine does less work, and holds less memory while it does.
CIImage*
source_image(const fs::path& src, PixelSize target, bool linear,
             double zoom)
{
  NSURL* url = [NSURL fileURLWithPath:@(src.c_str())];
  CGImageSourceRef s = open_source(src);
  if (!s) { return nil; }
  CFStringRef type = CGImageSourceGetType(s);
  UTType* t = type ? [UTType typeWithIdentifier:(__bridge NSString*)type]
                   : nil;
  const bool raw = t && [t conformsToType:UTTypeRAWImage];
  CFRelease(s);
  if (raw) {
    CIRAWFilter* f = [CIRAWFilter filterWithImageURL:url];
    if (!f) { return nil; }
    CGSize n = f.nativeSize;
    if ((int)f.orientation >= 5) {                // a quarter turn
      n = CGSizeMake(n.height, n.width);
    }
    if (n.width > 0 && n.height > 0) {
      const double kx = target.width / n.width;
      const double ky = target.height / n.height;
      // Crop covers the target (the larger scale); Stretch needs each
      // axis's, and the RAW engine scales both alike.
      // A crop that zooms in needs the content that much larger.
      const double need = std::max(kx, ky) * zoom;
      f.scaleFactor = (float)std::min(1.0, need);
    }
    if (linear) {
      f.boostAmount = 0;                       // no global tone curve
      if (f.localToneMapSupported) { f.localToneMapAmount = 0; }
      f.extendedDynamicRangeAmount = 2;        // keep the headroom
    }
    return f.outputImage;
  }
  return [CIImage imageWithContentsOfURL:url options:@{
    kCIImageApplyOrientationProperty : @YES,
  }];
}

// The crop (media/crop.h): the content placed on its canvas -- scaled,
// turned about its centre, offset -- over the padding colour, and cut to
// the canvas. The padding is sRGB, straight alpha, as the panel's colour
// well picks it; it is not adjusted.
CIImage*
apply_crop(CIImage* img, const Crop& crop)
{
  const CGRect e = img.extent;
  img = [img imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                          -e.origin.x, -e.origin.y)];
  const CropPlacement p = crop_placement(crop, e.size.width,
                                         e.size.height);
  const Affine& m = p.transform;
  img = [img imageByApplyingTransform:CGAffineTransformMake(
                                          m.a, m.b, m.c, m.d, m.tx, m.ty)];
  const CGRect canvas = CGRectMake(0, 0, p.canvas_w, p.canvas_h);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CIColor* color = [CIColor colorWithRed:crop.pad[0]
                                   green:crop.pad[1]
                                    blue:crop.pad[2]
                                   alpha:crop.pad[3]
                              colorSpace:srgb];
  CGColorSpaceRelease(srgb);
  CIImage* pad = [[CIImage imageWithColor:color]
      imageByCroppingToRect:canvas];
  return [[img imageByCompositingOverImage:pad]
      imageByCroppingToRect:canvas];
}

// The content through its crop's PLACEMENT alone -- scaled, turned and
// moved on the crop's canvas, in its coordinates, but neither padded nor
// cut to it: a layer moved past its frame still shows wherever the
// stack's canvas reaches.
CIImage*
place_by_crop(CIImage* img, const Crop& crop)
{
  const CGRect e = img.extent;
  img = [img imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                          -e.origin.x, -e.origin.y)];
  const Affine& m =
      crop_placement(crop, e.size.width, e.size.height).transform;
  return [img imageByApplyingTransform:CGAffineTransformMake(
                                           m.a, m.b, m.c, m.d, m.tx, m.ty)];
}

// The adjustment chain (media/adjust.h) as Core Image filters.
CIImage*
apply_chain(CIImage* img, const std::vector<FilterStep>& chain)
{
  for (const FilterStep& step : chain) {
    CIFilter* f = [CIFilter filterWithName:@(step.filter.c_str())];
    if (!f) { continue; }
    [f setValue:img forKey:kCIInputImageKey];
    for (const auto& [key, v] : step.params) {
      id value = nil;
      if (v.size() == 1) {
        value = @(v[0]);
      } else {
        std::vector<CGFloat> c(v.begin(), v.end());
        value = [CIVector vectorWithValues:c.data() count:c.size()];
      }
      [f setValue:value forKey:@(key.c_str())];
    }
    if (CIImage* out = f.outputImage) { img = out; }
  }
  return img;
}

// ---- the planes on the GPU ---------------------------------------------
//
// What the CPU loop in render_planar does -- premultiplied, interleaved
// RGBA half floats in; straight alpha, planar, out -- as a compute
// kernel reading the texture Core Image drew. Precise math (no fast
// reciprocal), so it writes the samples the CPU loop writes.

const char* const kPlanarKernel = R"(
#include <metal_stdlib>
using namespace metal;

struct Params {
  uint width;
  uint height;
  uint channels;
  uint clamp;
};

kernel void to_planar(texture2d<half, access::read> src [[texture(0)]],
                      device half* dst [[buffer(0)]],
                      constant Params& p [[buffer(1)]],
                      uint2 g [[thread_position_in_grid]])
{
  if (g.x >= p.width || g.y >= p.height) { return; }
  const half4 px = src.read(g);
  const float a = float(px.a);
  const uint plane = p.width * p.height;
  const uint i = g.y * p.width + g.x;
  for (uint c = 0; c < p.channels; ++c) {
    float v = c == 3 ? a : float(px[c]);
    if (c < 3 && a < 1.0f) {
      v = a > 0.0f ? (p.clamp != 0 ? min(1.0f, v / a) : v / a) : 0.0f;
    }
    dst[c * plane + i] = half(v);
  }
}
)";

struct PlanarParams {
  std::uint32_t width, height, channels, clamp;
};

// The kernel and a queue, for the device a target buffer lives on --
// made once (compiling the kernel takes a moment); nil where Metal
// cannot.
struct Gpu {
  id<MTLDevice>               device = nil;
  id<MTLCommandQueue>         queue = nil;
  id<MTLComputePipelineState> pipeline = nil;
  // On the kernel's queue, so Core Image's work and the kernel share one
  // command buffer; made once (per frame, a long export rebuilt it
  // thousands of times).
  CIContext*                  context = nil;
};

Gpu*
gpu_for(id<MTLDevice> device)
{
  static std::mutex mu;
  static Gpu* made = nullptr;
  std::lock_guard lk(mu);
  if (made && made->device == device) {
    return made->pipeline ? made : nullptr;
  }
  auto* g = new Gpu;  // one device, for the process: kept
  g->device = device;
  MTLCompileOptions* opts = [MTLCompileOptions new];
  opts.mathMode = MTLMathModeSafe;
  NSError* err = nil;
  id<MTLLibrary> lib = [device newLibraryWithSource:@(kPlanarKernel)
                                            options:opts
                                              error:&err];
  id<MTLFunction> fn = [lib newFunctionWithName:@"to_planar"];
  g->pipeline = fn ? [device newComputePipelineStateWithFunction:fn
                                                           error:&err]
                   : nil;
  g->queue = g->pipeline ? [device newCommandQueue] : nil;
  if (!g->queue) {
    g->pipeline = nil;
  } else {
    CGColorSpaceRef ws =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    g->context = [CIContext contextWithMTLCommandQueue:g->queue options:@{
      kCIContextWorkingColorSpace : (__bridge id)ws,
      kCIContextWorkingFormat : @(kCIFormatRGBAh),
      kCIContextCacheIntermediates : @NO,
    }];
    CGColorSpaceRelease(ws);
  }
  made = g;
  return g->pipeline ? g : nullptr;
}

// `img`, a W x H frame from (0, 0), drawn by Core Image into a half-float
// texture and written by the kernel as planes into `target` -- on the
// GPU, the CPU only waiting. False when the GPU could not take it (the
// caller then draws on the CPU).
bool
render_planar_gpu(CIImage* img, std::int32_t W, std::int32_t H,
                  int channels, bool clamp, CGColorSpaceRef cs,
                  const GpuTarget& target)
{
  id<MTLBuffer> buf = (__bridge id<MTLBuffer>)target.buffer;
  const std::size_t bytes =
      static_cast<std::size_t>(W) * H * channels * sizeof(std::uint16_t);
  // Metal binds a buffer at a 4-byte-aligned offset.
  if (!buf || target.offset % 4 != 0 ||
      target.offset + bytes > buf.length) {
    return false;
  }
  Gpu* g = gpu_for(buf.device);
  if (!g) {
    return false;
  }
  MTLTextureDescriptor* td = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                   width:static_cast<NSUInteger>(W)
                                  height:static_cast<NSUInteger>(H)
                               mipmapped:NO];
  td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
  td.storageMode = MTLStorageModePrivate;
  id<MTLTexture> tex = [g->device newTextureWithDescriptor:td];
  if (!tex) {
    return false;
  }
  CIContext* ctx = g->context;
  id<MTLCommandBuffer> cb = [g->queue commandBuffer];
  CIRenderDestination* dest =
      [[CIRenderDestination alloc] initWithMTLTexture:tex commandBuffer:cb];
  dest.colorSpace = cs;
  dest.clamped = NO;
  // Row 0 at the top, as the planes are.
  dest.flipped = YES;
  NSError* err = nil;
  if (![ctx startTaskToRender:img
                     fromRect:CGRectMake(0, 0, W, H)
                toDestination:dest
                      atPoint:CGPointZero
                        error:&err]) {
    return false;
  }
  id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
  [enc setComputePipelineState:g->pipeline];
  [enc setTexture:tex atIndex:0];
  [enc setBuffer:buf offset:target.offset atIndex:0];
  const PlanarParams p{static_cast<std::uint32_t>(W),
                       static_cast<std::uint32_t>(H),
                       static_cast<std::uint32_t>(channels),
                       clamp ? 1u : 0u};
  [enc setBytes:&p length:sizeof p atIndex:1];
  const NSUInteger tw = g->pipeline.threadExecutionWidth;
  const NSUInteger th =
      std::max<NSUInteger>(1, g->pipeline.maxTotalThreadsPerThreadgroup / tw);
  [enc dispatchThreads:MTLSizeMake(static_cast<NSUInteger>(W),
                                   static_cast<NSUInteger>(H), 1)
      threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
  [enc endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  return cb.status == MTLCommandBufferStatusCompleted;
}

// `img` scaled to `size` (Lanczos; Crop covers it and cuts the overhang
// evenly, Stretch scales each axis on its own), rendered in `cs` -- held
// to 0..1 when `clamp` -- and written to `dst` as planar [channels, h, w]
// with straight alpha: u8, or F16 when `f16`. F16 into a Metal buffer
// (`gpu`) is drawn there by the GPU, with no copy through the CPU.
Status
render_planar(CIImage* img, PixelSize size, int channels, Fit fit, bool f16,
              bool clamp, CGColorSpaceRef cs, void* dst,
              const GpuTarget& gpu = {})
{
  const std::int32_t W = size.width;
  const std::int32_t H = size.height;
  const std::size_t plane = static_cast<std::size_t>(W) * H;
  // The edges clamped, so no transparent border bleeds in.
  const CGRect e = img.extent;
  const double iw = e.size.width;
  const double ih = e.size.height;
  double sx = W / iw;
  double sy = H / ih;
  if (fit == Fit::Crop) { sx = sy = std::max(sx, sy); }
  if (std::abs(sx - 1) > 1e-9 || std::abs(sy - 1) > 1e-9) {
    CIFilter* scale = [CIFilter filterWithName:@"CILanczosScaleTransform"];
    [scale setValue:[img imageByClampingToExtent] forKey:kCIInputImageKey];
    [scale setValue:@(sy) forKey:kCIInputScaleKey];
    [scale setValue:@(sx / sy) forKey:kCIInputAspectRatioKey];
    img = scale.outputImage;
  }
  const double ox = e.origin.x * sx + (iw * sx - W) / 2;
  const double oy = e.origin.y * sy + (ih * sy - H) / 2;
  img = [[img imageByApplyingTransform:
             CGAffineTransformMakeTranslation(-ox, -oy)]
      imageByCroppingToRect:CGRectMake(0, 0, W, H)];
  if (clamp) {
    img = [img imageByApplyingFilter:@"CIColorClamp" withInputParameters:@{
      @"inputMinComponents" : [CIVector vectorWithX:0 Y:0 Z:0 W:0],
      @"inputMaxComponents" : [CIVector vectorWithX:1 Y:1 Z:1 W:1],
    }];
  }

  if (f16 && gpu.buffer) {
    if (render_planar_gpu(img, W, H, channels, clamp, cs, gpu)) {
      static std::once_flag said;
      std::call_once(said, [] {
        VALTZ_LOG_INFO("media", "model inputs drawn on the GPU, straight "
                       "into the engine's buffers");
      });
      return ok_status();
    }
    VALTZ_LOG_WARN("media", "a {}x{} model input fell back to the CPU", W,
                   H);
  }

  // Interleaved, premultiplied, row 0 at the top -- then planar and
  // straight, as the tensors expect.
  const std::size_t bpp = f16 ? 8 : 4;
  std::vector<std::uint8_t> rgba(plane * bpp);
  [decode_context() render:img toBitmap:rgba.data()
                 rowBytes:(ptrdiff_t)(W * bpp)
                   bounds:CGRectMake(0, 0, W, H)
                   format:f16 ? kCIFormatRGBAh : kCIFormatRGBA8
               colorSpace:cs];
  if (f16) {
    const auto* p = reinterpret_cast<const _Float16*>(rgba.data());
    auto* out = static_cast<_Float16*>(dst);
    for (std::size_t i = 0; i < plane; ++i) {
      const float a = (float)p[i * 4 + 3];
      for (int c = 0; c < channels; ++c) {
        float v = c == 3 ? a : (float)p[i * 4 + c];
        if (c < 3 && a < 1.0f) {
          v = a > 0.0f ? (clamp ? std::min(1.0f, v / a) : v / a) : 0;
        }
        out[static_cast<std::size_t>(c) * plane + i] = (_Float16)v;
      }
    }
    return ok_status();
  }
  auto* out = static_cast<std::uint8_t*>(dst);
  for (std::size_t i = 0; i < plane; ++i) {
    const std::uint8_t* p = &rgba[i * 4];
    const unsigned a = p[3];
    for (int c = 0; c < channels; ++c) {
      unsigned v = c == 3 ? a : p[c];
      if (c < 3 && a != 255) {
        v = a == 0 ? 0 : std::min(255u, (v * 255 + a / 2) / a);
      }
      out[static_cast<std::size_t>(c) * plane + i] =
          static_cast<std::uint8_t>(v);
    }
  }
  return ok_status();
}

}

Status
decode_planar(const fs::path& src, PixelSize size, int channels, Fit fit,
              const Adjustments& adjust, Sample sample, void* dst,
              std::size_t dst_size, Space space, const Crop& crop,
              const GpuTarget& gpu)
{
  const bool linear = space == Space::Linear;
  const std::int32_t W = size.width;
  const std::int32_t H = size.height;
  if (W <= 0 || H <= 0 || (channels != 3 && channels != 4)) {
    return make_error(Code::InvalidArgument, "bad target geometry");
  }
  const bool f16 = sample == Sample::F16;
  const std::size_t plane = static_cast<std::size_t>(W) * H;
  if (!dst || dst_size < plane * channels * (f16 ? 2 : 1)) {
    return make_error(Code::InvalidArgument, "target buffer too small");
  }
  @autoreleasepool {
    // How much larger than the target the content is drawn: a crop that
    // zooms in shows less of it, bigger.
    double zoom = 1;
    if (!crop.identity()) {
      zoom = std::max(crop.scale_x, crop.scale_y);
      if (crop.content.width > 0 && crop.canvas.width > 0) {
        zoom *= static_cast<double>(crop.content.width) / crop.canvas.width;
      }
    }
    CIImage* img = source_image(src, size, linear, zoom);
    if (!img || CGRectIsEmpty(img.extent) || CGRectIsInfinite(img.extent)) {
      return make_error(Code::Corrupt, std::format("cannot decode {}",
                                                   src.string()));
    }
    // What the stage showed: the adjustments, on top of the development,
    // in linear light.
    img = apply_chain(img, filter_chain(adjust));
    // Then the crop: the picture on its canvas.
    if (!crop.identity()) {
      img = apply_crop(img, crop);
    }

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(
        linear ? kCGColorSpaceExtendedLinearSRGB : kCGColorSpaceSRGB);
    // The model's range: sRGB 0..1. In the linear sRGB working space
    // that IS the sRGB clamp. A linear file keeps what it has.
    Status st = render_planar(img, size, channels, fit, f16,
                              /*clamp=*/!linear, cs, dst, gpu);
    CGColorSpaceRelease(cs);
    return st;
  }
}

namespace {

// A clip's picture track, loaded; nil without one.
AVAssetTrack*
video_track(AVURLAsset* asset)
{
  __block AVAssetTrack* track = nil;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [asset loadTracksWithMediaType:AVMediaTypeVideo
               completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*) {
                 track = t.firstObject;
                 dispatch_semaphore_signal(sem);
               }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  return track;
}

bool
cf_true(CFTypeRef v)
{
  return v && CFGetTypeID(v) == CFBooleanGetTypeID() &&
         CFBooleanGetValue(static_cast<CFBooleanRef>(v));
}

// Byte `at` of a sample description's configuration atom ("hvcC",
// "avcC"); -1 where there is none.
int
config_byte(CMFormatDescriptionRef fd, CFStringRef atom, CFIndex at)
{
  CFTypeRef atoms = CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
  if (!atoms || CFGetTypeID(atoms) != CFDictionaryGetTypeID()) {
    return -1;
  }
  CFTypeRef d = CFDictionaryGetValue(static_cast<CFDictionaryRef>(atoms),
                                     atom);
  if (!d || CFGetTypeID(d) != CFDataGetTypeID() ||
      CFDataGetLength(static_cast<CFDataRef>(d)) <= at) {
    return -1;
  }
  return CFDataGetBytePtr(static_cast<CFDataRef>(d))[at];
}

// What a clip's frames are read as: its decoder's own format -- no
// conversion on the way, and a fraction of F16 RGBA's bytes (a 4K frame
// is 12 MB as 8-bit 4:2:0, 25 as 10-bit, 66 as F16 RGBA; AVFoundation
// reads a few ahead, per clip). Core Image reads each, its colour tags
// with it (as close to the file as F16 RGBA is: within a level or two,
// chroma resampling aside). ProRes 4444 -- alpha, 4:4:4 -- reads as F16
// RGBA: Core Image cannot read its own 'y416'. So does a codec not known
// to be YCbCr: an RGB one, H.264 past 8 bits.
OSType
reader_format(AVAssetTrack* track)
{
  auto fd = (__bridge CMFormatDescriptionRef)
                track.formatDescriptions.firstObject;
  if (!fd) {
    return kCVPixelFormatType_64RGBAHalf;
  }
  const FourCharCode sub = CMFormatDescriptionGetMediaSubType(fd);
  const bool full = cf_true(CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_FullRangeVideo));
  const bool alpha = cf_true(CMFormatDescriptionGetExtension(
      fd, kCMFormatDescriptionExtension_ContainsAlphaChannel));
  int bits = 0;
  if (CFTypeRef b = CMFormatDescriptionGetExtension(
          fd, kCMFormatDescriptionExtension_BitsPerComponent);
      b && CFGetTypeID(b) == CFNumberGetTypeID()) {
    CFNumberGetValue(static_cast<CFNumberRef>(b), kCFNumberIntType, &bits);
  }
  switch (sub) {
  case kCMVideoCodecType_AppleProRes4444:
  case kCMVideoCodecType_AppleProRes4444XQ:
    return kCVPixelFormatType_64RGBAHalf;
  case kCMVideoCodecType_AppleProRes422:
  case kCMVideoCodecType_AppleProRes422HQ:
  case kCMVideoCodecType_AppleProRes422LT:
  case kCMVideoCodecType_AppleProRes422Proxy:
    return full ? kCVPixelFormatType_422YpCbCr10BiPlanarFullRange
                : kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange;
  case kCMVideoCodecType_HEVC:
  case 'hev1': {
    if (alpha) {
      return kCVPixelFormatType_64RGBAHalf;
    }
    // The depth and chroma from its configuration record: bitDepthLuma-
    // Minus8 (byte 17) and chromaFormat (byte 16).
    if (const int d = config_byte(fd, CFSTR("hvcC"), 17); d >= 0) {
      bits = (d & 7) + 8;
    }
    const int chroma = config_byte(fd, CFSTR("hvcC"), 16);
    if (chroma >= 0 && (chroma & 3) == 3) {
      return kCVPixelFormatType_64RGBAHalf;  // 4:4:4
    }
    if (chroma >= 0 && (chroma & 3) == 2) {
      return full ? kCVPixelFormatType_422YpCbCr10BiPlanarFullRange
                  : kCVPixelFormatType_422YpCbCr10BiPlanarVideoRange;
    }
    if (bits > 8) {
      return full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange
                  : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;
    }
    return full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  }
  case kCMVideoCodecType_H264:
  case 'avc3': {
    // High 10, 4:2:2 and 4:4:4 profiles (110, 122, 244): deeper or
    // fuller than 8-bit 4:2:0.
    const int profile = config_byte(fd, CFSTR("avcC"), 1);
    if (bits > 8 || profile == 110 || profile == 122 || profile == 244) {
      return kCVPixelFormatType_64RGBAHalf;
    }
    return full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange
                : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
  }
  default:
    return kCVPixelFormatType_64RGBAHalf;
  }
}

// `img` drawn small for the screen (FramePreview): sRGB, 8-bit, planar,
// its long edge at most `edge`.
PreviewPicture
small_picture(CIImage* img, int edge, std::int64_t frame)
{
  PreviewPicture out;
  out.frame = frame;
  const CGRect e = img.extent;
  if (edge <= 0 || CGRectIsEmpty(e) || CGRectIsInfinite(e)) {
    return out;
  }
  const double s =
      std::min(1.0, edge / std::max(e.size.width, e.size.height));
  const int w = std::max(1, static_cast<int>(std::lround(e.size.width * s)));
  const int h =
      std::max(1, static_cast<int>(std::lround(e.size.height * s)));
  img = [img imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                          -e.origin.x, -e.origin.y)];
  if (s < 1) {
    img = [img imageByApplyingFilter:@"CILanczosScaleTransform"
                 withInputParameters:@{
                   kCIInputScaleKey : @(s),
                   kCIInputAspectRatioKey : @1,
                 }];
  }
  const std::size_t plane = static_cast<std::size_t>(w) * h;
  std::vector<std::uint8_t> rgba(plane * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  [decode_context() render:img
                  toBitmap:rgba.data()
                  rowBytes:static_cast<ptrdiff_t>(w) * 4
                    bounds:CGRectMake(0, 0, w, h)
                    format:kCIFormatRGBA8
                colorSpace:cs];
  CGColorSpaceRelease(cs);
  out.size = {w, h};
  out.rgb.resize(plane * 3);
  for (std::size_t i = 0; i < plane; ++i) {
    for (std::size_t c = 0; c < 3; ++c) {
      out.rgb[c * plane + i] = rgba[i * 4 + c];
    }
  }
  return out;
}

}

Result<PreviewPicture>
preview_movie_frame(const fs::path& src, double seconds, int edge)
{
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:@(src.c_str())];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    AVAssetImageGenerator* gen =
        [[AVAssetImageGenerator alloc] initWithAsset:asset];
    gen.appliesPreferredTrackTransform = YES;
    gen.maximumSize = CGSizeMake(edge, edge);
    gen.requestedTimeToleranceBefore = CMTimeMake(1, 2);
    gen.requestedTimeToleranceAfter = CMTimeMake(1, 2);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block CGImageRef got = nullptr;
    [gen generateCGImageAsynchronouslyForTime:
             CMTimeMakeWithSeconds(std::max(0.0, seconds), 600)
                            completionHandler:^(CGImageRef img, CMTime,
                                                NSError*) {
                              if (img) {
                                got = CGImageRetain(img);
                              }
                              dispatch_semaphore_signal(sem);
                            }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (!got) {
      return make_error(Code::Corrupt, std::format(
          "no frame of {} at {:.1f} s", src.filename().string(), seconds));
    }
    CIImage* img = [CIImage imageWithCGImage:got];
    CGImageRelease(got);
    return small_picture(img, edge, 0);
  }
}

Status
decode_movie(const fs::path& src, const KeyedAdjustments& adjust,
             const KeyedCrop& crop, Rational rate, std::int64_t first,
             std::int64_t count, PixelSize size, const FrameTarget& target,
             const FrameDone& done, const FramePreview* preview,
             Sample sample)
{
  const bool f16 = sample == Sample::F16;
  if (size.width <= 0 || size.height <= 0) {
    return make_error(Code::InvalidArgument, "bad target geometry");
  }
  @autoreleasepool {
    NSURL* url = [NSURL fileURLWithPath:@(src.c_str())];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    AVAssetTrack* track = video_track(asset);
    if (!track) {
      return make_error(Code::Corrupt, std::format(
          "{} has no video track", src.filename().string()));
    }
    if (rate.num <= 0) {
      return make_error(Code::InvalidArgument, "the clip's rate is unknown");
    }
    NSError* err = nil;
    AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset
                                                          error:&err];
    if (!reader) {
      return make_error(Code::Io, std::format(
          "cannot read {}", src.filename().string()));
    }
    // The decoder's own format: Core Image reads its colour from the
    // buffer's tags.
    NSDictionary* settings = @{
      (id)kCVPixelBufferPixelFormatTypeKey : @(reader_format(track)),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    AVAssetReaderTrackOutput* out =
        [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track
                                                   outputSettings:settings];
    out.alwaysCopiesSampleData = NO;
    [reader addOutput:out];
    // Frame n starts at n * den / num seconds, exactly.
    const auto at = [&](std::int64_t n) {
      return CMTimeMake(n * rate.den, static_cast<std::int32_t>(rate.num));
    };
    if (first > 0 || count > 0) {
      reader.timeRange = CMTimeRangeMake(
          at(std::max<std::int64_t>(0, first)),
          count > 0 ? at(count) : kCMTimePositiveInfinity);
    }
    if (![reader startReading]) {
      return make_error(Code::Io, std::format(
          "cannot read {}", src.filename().string()));
    }
    const double fps = rate.to_double();
    std::int64_t written = 0;
    Status st = ok_status();
    for (;;) {
      CMSampleBufferRef sb = [out copyNextSampleBuffer];
      if (!sb) {
        break;
      }
      @autoreleasepool {
        CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(sb);
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
        const auto frame = static_cast<std::int64_t>(
            std::llround(CMTimeGetSeconds(pts) * fps));
        if (pb && frame >= first && (count < 0 || written < count)) {
          CIImage* img = [CIImage imageWithCVPixelBuffer:pb];
          img = apply_chain(img, filter_chain(adjust.at(frame)));
          const Crop c = crop.at(frame);
          if (!c.identity()) {
            img = apply_crop(img, c);
          }
          // Back into the clip's own colour, as it declares it.
          CFDictionaryRef tags = CVBufferCopyAttachments(
              pb, kCVAttachmentMode_ShouldPropagate);
          CGColorSpaceRef cs =
              tags ? CVImageBufferCreateColorSpaceFromAttachments(tags)
                   : nullptr;
          if (!cs) {
            cs = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
          }
          auto t = target(frame);
          if (!t.ok()) {
            st = t.error();
          } else if (t->size < static_cast<std::size_t>(size.width) *
                                   size.height * 3 * (f16 ? 2 : 1)) {
            st = make_error(Code::InvalidArgument, "frame buffer too small");
          } else {
            st = render_planar(img, size, 3, Fit::Crop, f16,
                               /*clamp=*/!f16, cs, t->data, t->gpu);
            if (st.ok() && preview && preview->wants &&
                preview->wants(frame)) {
              preview->take(small_picture(img, preview->edge, frame));
            }
            if (st.ok()) {
              st = done(frame);
            }
          }
          CGColorSpaceRelease(cs);
          if (tags) {
            CFRelease(tags);
          }
          ++written;
        }
      }
      CFRelease(sb);
      if (!st.ok() || (count > 0 && written >= count)) {
        break;
      }
    }
    if (!st.ok()) {
      [reader cancelReading];
      return st;
    }
    if (reader.status == AVAssetReaderStatusFailed) {
      return make_error(Code::Io, std::format(
          "reading {} failed", src.filename().string()));
    }
    if (written == 0) {
      return make_error(Code::Corrupt, std::format(
          "{} gave no frames", src.filename().string()));
    }
    [reader cancelReading];
  }
  return ok_status();
}

Status
decode_planar_srgb(const fs::path& src, PixelSize size, int channels,
                   Fit fit, std::uint8_t* dst, std::size_t dst_size)
{
  return decode_planar(src, size, channels, fit, Adjustments{}, Sample::U8,
                       dst, dst_size);
}


// ---- a layer stack, flattened (media/layers.h) ------------------------

namespace {

// One layer as compose_images takes it: its picture, as Core Image reads
// it (nil: nothing on it), and its look.
struct ComposeLayer {
  CIImage*    image = nil;
  Adjustments adjust;
  Crop        crop;
  bool        visible = true;
  bool        mask = false;
  // How much of it shows (a dissolve's ramp): its alpha, times this.
  double      opacity = 1;
};

// `img` faded to `opacity` of itself.
CIImage*
faded(CIImage* img, double opacity)
{
  if (!img || opacity >= 1) {
    return img;
  }
  return [img imageByApplyingFilter:@"CIColorMatrix"
                withInputParameters:@{
    @"inputAVector" : [CIVector vectorWithX:0 Y:0 Z:0 W:std::max(0.0,
                                                                 opacity)],
  }];
}

// The stack as one Core Image picture (media/layers.h), on its CANVAS
// from (0, 0). Pictures and a clip's frames alike. The layers lie on a
// FRAME: the stack's own (the project's: StackCanvas::framed), or the
// bottom layer through its crop. Each layer -- every one on a frame of
// its own, each above the bottom one else -- is drawn through its look
// and placed on the frame by its crop, or centred at its own size. The
// frame sits on the canvas (StackCanvas: at x, y; unset, the canvas IS
// the frame), and a layer is cut where the CANVAS ends, not the frame:
// moved off the frame, it still shows wherever the canvas reaches.
Result<CIImage*>
compose_images(const std::vector<ComposeLayer>& layers,
               const StackCanvas& sc = {})
{
  const bool framed = sc.framed();
  if (layers.empty() || (!framed && !layers.front().image)) {
    return make_error(Code::InvalidArgument,
                      "a layer stack needs a picture at the bottom");
  }
  auto look = [](const ComposeLayer& l) -> CIImage* {
    CIImage* img = l.image;
    if (!img || CGRectIsEmpty(img.extent) || CGRectIsInfinite(img.extent)) {
      return nil;
    }
    const CGRect e = img.extent;
    img = [img imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                            -e.origin.x, -e.origin.y)];
    return apply_chain(img, filter_chain(l.adjust));
  };
  // The frame, from (0, 0): the stack's own, or the bottom layer on its
  // own crop (padded with its colour, cut to its canvas).
  CIImage* bottom = nil;
  CGRect frame = CGRectMake(0, 0, sc.frame_w, sc.frame_h);
  if (!framed) {
    bottom = look(layers.front());
    if (!bottom) {
      return make_error(Code::Corrupt, "the bottom layer has no picture");
    }
    if (!layers.front().crop.identity()) {
      bottom = apply_crop(bottom, layers.front().crop);
    }
    frame = bottom.extent;
  }
  // What is SEEN: the canvas, in the frame's coordinates -- the frame's
  // top left at (x, y) from the canvas's, and Core Image's y up.
  const CGRect seen = sc.set()
      ? CGRectMake(-sc.x, frame.size.height + sc.y - sc.height, sc.width,
                   sc.height)
      : frame;
  CIImage* clear = [[CIImage imageWithColor:[CIColor clearColor]]
      imageByCroppingToRect:seen];
  CIImage* base = clear;
  if (!framed && layers.front().visible) {
    base = [faded(bottom, layers.front().opacity)
        imageByCompositingOverImage:clear];
  }
  // A layer on the frame: through its crop, or centred at its own size;
  // nil with nothing on it.
  auto placed = [&](const ComposeLayer& l) -> CIImage* {
    CIImage* img = look(l);
    if (!img) {
      return nil;
    }
    if (!l.crop.identity()) {
      Crop c = l.crop;
      if (c.canvas.width == 0 || c.canvas.height == 0) {
        c.canvas = {static_cast<std::int32_t>(frame.size.width),
                    static_cast<std::int32_t>(frame.size.height)};
      }
      if (c.content.width == 0) {
        c.content = {static_cast<std::int32_t>(img.extent.size.width),
                     static_cast<std::int32_t>(img.extent.size.height)};
      }
      img = place_by_crop(img, c);
    } else {
      img = [img imageByApplyingTransform:CGAffineTransformMakeTranslation(
          (frame.size.width - img.extent.size.width) / 2,
          (frame.size.height - img.extent.size.height) / 2)];
    }
    return [img imageByCroppingToRect:seen];
  };
  // `img` shown only where the mask above it (layer `m`) is bright: its
  // brightness as it looks (sRGB) times its alpha -- the mask's colour
  // over black is exactly that, premultiplied -- as `img`'s alpha.
  auto masked = [&](CIImage* img, std::size_t m) -> CIImage* {
    CIImage* mk = placed(layers[m]);
    CIImage* black = [[CIImage imageWithColor:[CIColor blackColor]]
        imageByCroppingToRect:seen];
    CIImage* lum = mk ? [mk imageByCompositingOverImage:black] : black;
    lum = [lum imageByApplyingFilter:@"CILinearToSRGBToneCurve"];
    lum = [lum imageByApplyingFilter:@"CIColorMatrix"
                 withInputParameters:@{
      @"inputRVector" : [CIVector vectorWithX:0 Y:0 Z:0 W:0],
      @"inputGVector" : [CIVector vectorWithX:0 Y:0 Z:0 W:0],
      @"inputBVector" : [CIVector vectorWithX:0 Y:0 Z:0 W:0],
      @"inputAVector" : [CIVector vectorWithX:0.2126 Y:0.7152 Z:0.0722
                                            W:0],
      @"inputBiasVector" : [CIVector vectorWithX:0 Y:0 Z:0 W:0],
    }];
    return [img imageByApplyingFilter:@"CIBlendWithAlphaMask"
                  withInputParameters:@{
      kCIInputBackgroundImageKey : clear,
      kCIInputMaskImageKey : [lum imageByCroppingToRect:seen],
    }];
  };
  auto masks_it = [&](std::size_t i) {
    return i + 1 < layers.size() && layers[i + 1].mask &&
           layers[i + 1].visible;
  };
  if (!framed && masks_it(0) && layers.front().visible) {
    base = masked(base, 1);
  }
  for (std::size_t i = framed ? 0 : 1; i < layers.size(); ++i) {
    const ComposeLayer& l = layers[i];
    if (!l.image || !l.visible || l.mask || l.opacity <= 0) {
      continue;
    }
    CIImage* img = placed(l);
    if (!img) {
      continue;
    }
    if (masks_it(i)) {
      img = masked(img, i + 1);
    }
    base = [faded(img, l.opacity) imageByCompositingOverImage:base];
  }
  // On the canvas, from (0, 0).
  base = [base imageByCroppingToRect:seen];
  return [base imageByApplyingTransform:CGAffineTransformMakeTranslation(
                                            -seen.origin.x, -seen.origin.y)];
}

// A still layer's picture, as Core Image reads it: drawn in memory, or a
// file read (a RAW developed, upright); nil with nothing on it.
Result<CIImage*>
layer_image(const fs::path& file, const DrawnPicture& drawn)
{
  if (drawn) {
    return [CIImage imageWithCGImage:drawn.get()];
  }
  if (file.empty()) {
    return static_cast<CIImage*>(nil);
  }
  CIImage* img = source_image(file, {1, 1}, /*linear=*/false, 1);
  if (!img || CGRectIsEmpty(img.extent) || CGRectIsInfinite(img.extent)) {
    return make_error(Code::Corrupt, std::format("cannot decode {}",
                                                 file.string()));
  }
  return img;
}

// The stack as one Core Image picture, on its canvas from (0, 0).
Result<CIImage*>
compose_layers(const std::vector<LayerPicture>& layers,
               const StackCanvas& canvas)
{
  std::vector<ComposeLayer> in;
  in.reserve(layers.size());
  for (const auto& l : layers) {
    VALTZ_ASSIGN(CIImage* img, layer_image(l.file, l.drawn));
    in.push_back({img, l.adjust, l.crop, l.visible, l.mask});
  }
  return compose_images(in, canvas);
}

}

namespace {

// A picture written as a 16-bit sRGB PNG with alpha.
Status
write_png16(CIImage* img, const fs::path& out)
{
  CIContext* ctx = decode_context();
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  NSError* err = nil;
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  const BOOL ok = [ctx writePNGRepresentationOfImage:img
      toURL:[NSURL fileURLWithPath:@(out.c_str())]
      format:kCIFormatRGBA16
      colorSpace:srgb
      options:@{}
      error:&err];
  CGColorSpaceRelease(srgb);
  if (!ok) {
    return make_error(Code::Io, std::format(
        "cannot write {}: {}", out.string(),
        err ? err.localizedDescription.UTF8String : "?"));
  }
  return ok_status();
}

}

Status
flatten_layers(const std::vector<LayerPicture>& layers,
               const fs::path& out, const StackCanvas& canvas)
{
  @autoreleasepool {
    VALTZ_ASSIGN(CIImage* base, compose_layers(layers, canvas));
    return write_png16(base, out);
  }
}

Result<IOSurfaceRef>
flatten_layers_surface(const std::vector<LayerPicture>& layers,
                       const StackCanvas& canvas)
{
  @autoreleasepool {
    VALTZ_ASSIGN(CIImage* base, compose_layers(layers, canvas));
    const CGRect canvas = base.extent;
    const auto w = static_cast<int>(canvas.size.width);
    const auto h = static_cast<int>(canvas.size.height);
    if (w <= 0 || h <= 0) {
      return make_error(Code::InvalidArgument, "the stack has no canvas");
    }
    NSDictionary* props = @{
      (id)kIOSurfaceWidth : @(w),
      (id)kIOSurfaceHeight : @(h),
      (id)kIOSurfaceBytesPerElement : @4,
      (id)kIOSurfaceBytesPerRow :
          @(IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, w * 4)),
      (id)kIOSurfacePixelFormat : @(0x42475241),  // 'BGRA'
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)props);
    if (!surface) {
      return make_error(Code::Internal, "cannot make a surface");
    }
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    if (CFPropertyListRef tags = CGColorSpaceCopyPropertyList(srgb)) {
      IOSurfaceSetValue(surface, kIOSurfaceColorSpace, tags);
      CFRelease(tags);
    }
    CIRenderDestination* dest =
        [[CIRenderDestination alloc] initWithIOSurface:
            (__bridge IOSurface*)surface];
    dest.colorSpace = srgb;
    dest.alphaMode = CIRenderDestinationAlphaPremultiplied;
    dest.flipped = YES;  // row 0 at the top, as the layer shows it
    NSError* err = nil;
    CIRenderTask* task = [decode_context() startTaskToRender:base
                                               toDestination:dest
                                                       error:&err];
    const bool ok = task && [task waitUntilCompletedAndReturnError:&err];
    CGColorSpaceRelease(srgb);
    if (!ok) {
      CFRelease(surface);
      return make_error(Code::Internal, std::format(
          "cannot draw the stack: {}",
          err ? err.localizedDescription.UTF8String : "?"));
    }
    return surface;
  }
}

// ---- a clip's stack (media::MovieStack) -------------------------------

Json
to_json(const MovieStack& m)
{
  Json ls = Json::array();
  for (const auto& l : m.layers) {
    ls.push_back({{"file", l.file.string()},
                  {"video", l.video},
                  {"audio_only", l.audio_only},
                  {"adjust", to_json(l.adjust)},
                  {"crop", to_json(l.crop)},
                  {"visible", l.visible},
                  {"mask", l.mask},
                  {"timing", to_json(l.timing)}});
  }
  Json ts = Json::array();
  for (const auto& t : m.transitions) {
    ts.push_back({{"from", t.from}, {"to", t.to},
                  {"dissolve", t.dissolve}});
  }
  return {{"layers", ls}, {"canvas", to_json(m.canvas)},
          {"frames", m.frames}, {"rate_num", m.rate.num},
          {"rate_den", m.rate.den}, {"transitions", ts}};
}

MovieStack
movie_stack_from_json(const Json& j)
{
  MovieStack m;
  for (const auto& l : jget(j, "layers", Json::array())) {
    MovieLayer ml;
    ml.file = jget<std::string>(l, "file", "");
    ml.video = jget(l, "video", false);
    ml.audio_only = jget(l, "audio_only", false);
    ml.adjust = keyed_adjustments_from_json(jget(l, "adjust",
                                                 Json::object()));
    ml.crop = keyed_crop_from_json(jget(l, "crop", Json::object()));
    ml.visible = jget(l, "visible", true);
    ml.mask = jget(l, "mask", false);
    ml.timing = layer_timing_from_json(jget(l, "timing", Json()));
    m.layers.push_back(std::move(ml));
  }
  m.canvas = stack_canvas_from_json(jget(j, "canvas", Json::object()));
  m.frames = std::max<std::int64_t>(0, jget<std::int64_t>(j, "frames", 0));
  const auto num = jget<std::int64_t>(j, "rate_num", 0);
  const auto den = jget<std::int64_t>(j, "rate_den", 1);
  m.rate = num > 0 && den > 0 ? Rational(num, den) : Rational(0, 1);
  for (const auto& t : jget(j, "transitions", Json::array())) {
    m.transitions.push_back(
        {jget<std::size_t>(t, "from", 0), jget<std::size_t>(t, "to", 0),
         jget(t, "dissolve", false)});
  }
  return m;
}

Result<std::int64_t>
timeline_frames(const MovieStack& m, Rational rate)
{
  if (m.frames > 0) {
    return m.frames;
  }
  {
    std::vector<LayerTiming> ts;
    for (const auto& l : m.layers) {
      ts.push_back(l.timing);
    }
    const double end = timeline_end(ts);
    if (end > 0) {
      return static_cast<std::int64_t>(
          std::ceil(end * rate.to_double() - 1e-6));
    }
    // Nothing timed on a frame of its own (pictures alone, or nothing
    // yet): five seconds, until a length is set.
    if (m.canvas.framed() || m.layers.empty() ||
        !m.layers.front().video) {
      return static_cast<std::int64_t>(std::llround(5 * rate.to_double()));
    }
  }
  if (m.layers.empty() || !m.layers.front().video) {
    return make_error(Code::InvalidArgument, "a clip's stack has its clip "
                                             "at the bottom");
  }
  VALTZ_ASSIGN(MediaInfo info, probe_file(m.layers.front().file));
  if (info.frame_count > 0) {
    return info.frame_count;
  }
  return static_cast<std::int64_t>(
      std::llround(info.duration.seconds() * rate.to_double()));
}

namespace {

// One clip of a stack, read in order from `start` seconds: the frame
// showing at a time -- the last to have begun -- and nothing past its
// end. Reading starts a frame before `start`, so the frame showing there
// is the first one had: an export of an hour's last minute decodes a
// minute, not the hour.
class ClipCursor {
public:
  ~ClipCursor()
  {
    if (_cur) { CFRelease(_cur); }
    if (_next) { CFRelease(_next); }
    [_reader cancelReading];
  }

  static Result<std::unique_ptr<ClipCursor>>
  open(const fs::path& src, double start = 0)
  {
    NSURL* url = [NSURL fileURLWithPath:@(src.c_str())];
    AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:nil];
    AVAssetTrack* track = video_track(asset);
    if (!track) {
      return make_error(Code::Corrupt, std::format(
          "{} has no video track", src.filename().string()));
    }
    NSError* err = nil;
    auto c = std::make_unique<ClipCursor>();
    c->_reader = [AVAssetReader assetReaderWithAsset:asset error:&err];
    if (!c->_reader) {
      return make_error(Code::Io, std::format(
          "cannot read {}", src.filename().string()));
    }
    c->_out = [AVAssetReaderTrackOutput
        assetReaderTrackOutputWithTrack:track
                         outputSettings:@{
      (id)kCVPixelBufferPixelFormatTypeKey : @(reader_format(track)),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    }];
    c->_out.alwaysCopiesSampleData = NO;
    [c->_reader addOutput:c->_out];
    const float fps = track.nominalFrameRate;
    c->_frame = fps > 0 ? 1.0 / fps : 1.0 / 24;
    if (start > c->_frame) {
      c->_reader.timeRange = CMTimeRangeMake(
          CMTimeMakeWithSeconds(start - c->_frame, 90000),
          kCMTimePositiveInfinity);
    }
    if (![c->_reader startReading]) {
      return make_error(Code::Io, std::format(
          "cannot read {}", src.filename().string()));
    }
    c->pull_();
    return c;
  }

  // The frame showing at `t` seconds; nil before its first and past its
  // last.
  CIImage*
  at(double t)
  {
    while (_next && _next_t <= t + 1e-6) {
      if (_cur) { CFRelease(_cur); }
      _cur = _next;
      _cur_t = _next_t;
      const CMTime d = CMSampleBufferGetDuration(_cur);
      _cur_end = _cur_t + (CMTIME_IS_NUMERIC(d) && CMTimeGetSeconds(d) > 0
                               ? CMTimeGetSeconds(d)
                               : _frame);
      _next = nullptr;
      pull_();
    }
    if (!_cur || (!_next && t >= _cur_end - 1e-6)) {
      return nil;
    }
    CVPixelBufferRef pb = CMSampleBufferGetImageBuffer(_cur);
    return pb ? [CIImage imageWithCVPixelBuffer:pb] : nil;
  }

  // The colour the frame on show declares (retained), else BT.709.
  CGColorSpaceRef
  copy_color_space() const
  {
    CGColorSpaceRef cs = nullptr;
    if (CVPixelBufferRef pb =
            _cur ? CMSampleBufferGetImageBuffer(_cur) : nullptr) {
      if (CFDictionaryRef tags = CVBufferCopyAttachments(
              pb, kCVAttachmentMode_ShouldPropagate)) {
        cs = CVImageBufferCreateColorSpaceFromAttachments(tags);
        CFRelease(tags);
      }
    }
    return cs ? cs : CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
  }

private:
  void
  pull_()
  {
    _next = [_out copyNextSampleBuffer];
    _next_t = _next ? CMTimeGetSeconds(
                          CMSampleBufferGetPresentationTimeStamp(_next))
                    : INFINITY;
  }

  AVAssetReader*            _reader = nil;
  AVAssetReaderTrackOutput* _out = nil;
  CMSampleBufferRef         _cur = nullptr;
  double                    _cur_t = -1;
  double                    _cur_end = -1;
  CMSampleBufferRef         _next = nullptr;
  double                    _next_t = INFINITY;
  double                    _frame = 1.0 / 24;
};

}

namespace {

// Each layer's timing, and the frame at which a layer's keys are read at
// timeline second `t` (counted from its start).
std::vector<LayerTiming>
timings_of(const MovieStack& stack)
{
  std::vector<LayerTiming> ts;
  ts.reserve(stack.layers.size());
  for (const auto& l : stack.layers) {
    ts.push_back(l.timing);
  }
  return ts;
}

double
local_frame(const LayerTiming& t, double sec, Rational rate)
{
  return std::max(0.0, sec - t.start) * rate.to_double();
}

// A stack from before has its bottom clip set the frame.
bool
bottom_sets_frame(const MovieStack& stack)
{
  return !stack.canvas.framed();
}

Status
check_stack(const MovieStack& stack)
{
  if (stack.layers.empty() && !stack.canvas.framed()) {
    return make_error(Code::InvalidArgument, "the composition is empty");
  }
  if (bottom_sets_frame(stack) && !stack.layers.front().video) {
    return make_error(Code::InvalidArgument,
                      "a clip's stack has its clip at the bottom");
  }
  return ok_status();
}

}

namespace {

// A stack's frames in order, composed (Core Image pictures on its canvas):
// the clips read forward from where each first shows, the pictures once.
// What a writer, an encoder and a single frame all draw.
class StackReader {
public:
  ~StackReader()
  {
    if (_cs) {
      CGColorSpaceRelease(_cs);
    }
  }

  static Result<std::unique_ptr<StackReader>>
  open(const MovieStack& stack, Rational rate_in)
  {
    VALTZ_TRY(check_stack(stack));
    auto r = std::make_unique<StackReader>();
    r->_stack = &stack;
    r->_rate = stack.rate.num > 0 ? stack.rate : rate_in;
    if (r->_rate.num <= 0) {
      return make_error(Code::InvalidArgument, "bad frame rate");
    }
    r->_timings = timings_of(stack);
    r->_legacy = bottom_sets_frame(stack);
    r->_live.resize(stack.layers.size());
    for (std::size_t i = 0; i < stack.layers.size(); ++i) {
      const MovieLayer& l = stack.layers[i];
      if (!l.video && !l.audio_only) {
        VALTZ_ASSIGN(r->_live[i].still, layer_image(l.file, l.drawn));
      }
    }
    VALTZ_ASSIGN(r->_frames, timeline_frames(stack, r->_rate));
    return r;
  }

  Rational rate() const { return _rate; }
  std::int64_t frames() const { return _frames; }
  // The colour the first clip shown declares; a video's until one has.
  CGColorSpaceRef
  space()
  {
    if (!_cs) {
      _cs = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
    }
    return _cs;
  }

  // Timeline frame `f` -- later than the last one asked for.
  Result<CIImage*>
  frame(std::int64_t f)
  {
    const MovieStack& stack = *_stack;
    const double t = static_cast<double>(f) * _rate.den / _rate.num;
    const LayerWeights w = layer_weights(_timings, stack.transitions, t);
    std::vector<ComposeLayer> in;
    in.reserve(stack.layers.size());
    for (std::size_t i = 0; i < stack.layers.size(); ++i) {
      const MovieLayer& l = stack.layers[i];
      const LayerTiming& lt = _timings[i];
      const bool on = lt.active(t);
      CIImage* img = nil;
      if (l.video && (on || (_legacy && i == 0))) {
        // Opened where it first shows: read from there on.
        if (!_live[i].tried) {
          _live[i].tried = true;
          VALTZ_ASSIGN(_live[i].clip,
                       ClipCursor::open(l.file, lt.source_at(t)));
        }
        img = _live[i].clip ? _live[i].clip->at(lt.source_at(t)) : nil;
        if (img && !_cs) {
          _cs = _live[i].clip->copy_color_space();
        }
      } else if (!l.video && !l.audio_only && on) {
        img = _live[i].still;
      }
      if (_legacy && i == 0) {
        if (img) {
          _bottom = img.extent;
        } else if (!CGRectIsNull(_bottom)) {
          // Past the clip's end, its frame stays -- empty.
          img = [[CIImage imageWithColor:[CIColor clearColor]]
              imageByCroppingToRect:_bottom];
        } else {
          return make_error(Code::Corrupt, "the clip gave no frames");
        }
      }
      const double lf = local_frame(lt, t, _rate);
      // A mask not on masks nothing.
      in.push_back({img, l.adjust.at(lf), l.crop.at(lf),
                    l.visible && (!l.mask || on), l.mask,
                    _legacy && i == 0 ? 1.0 : w.opacity[i]});
    }
    return compose_images(in, stack.canvas);
  }

private:
  struct Live {
    std::unique_ptr<ClipCursor> clip;
    bool                        tried = false;
    CIImage*                    still = nil;
  };
  const MovieStack*        _stack = nullptr;
  Rational                 _rate;
  std::vector<LayerTiming> _timings;
  bool                     _legacy = false;
  std::vector<Live>        _live;
  std::int64_t             _frames = 0;
  CGRect                   _bottom = CGRectNull;
  CGColorSpaceRef          _cs = nullptr;
};

}

static CGColorSpaceRef
create_output_space(const ColorInfo& c)
{
  CFStringRef name = kCGColorSpaceITUR_709;
  if (c.primaries == Primaries::BT2020) {
    name = c.transfer == Transfer::PQ    ? kCGColorSpaceITUR_2100_PQ
           : c.transfer == Transfer::HLG ? kCGColorSpaceITUR_2100_HLG
                                         : kCGColorSpaceITUR_2020;
  } else if (c.primaries == Primaries::SMPTE432) {
    name = kCGColorSpaceDisplayP3;
  } else if (c.transfer == Transfer::SRGB) {
    name = kCGColorSpaceSRGB;
  }
  return CGColorSpaceCreateWithName(name);
}

// The space a VIDEO is written in: the one Core Video gives frames
// decoded with these tags (CVImageBufferCreateColorSpaceFromAttachments),
// so a clip drawn into it comes back as it was read. The named BT.709
// space is not that one -- its curve is the camera's, the decoded frames'
// Apple's -- and every frame of a project's export came out lighter
// (mid-greys up by about a tenth).
static CGColorSpaceRef
create_video_output_space(const ColorInfo& c)
{
  CFStringRef prim = kCVImageBufferColorPrimaries_ITU_R_709_2;
  CFStringRef xfer = kCVImageBufferTransferFunction_ITU_R_709_2;
  CFStringRef mat = kCVImageBufferYCbCrMatrix_ITU_R_709_2;
  if (c.primaries == Primaries::BT2020) {
    prim = kCVImageBufferColorPrimaries_ITU_R_2020;
    mat = kCVImageBufferYCbCrMatrix_ITU_R_2020;
    xfer = c.transfer == Transfer::PQ
               ? kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ
           : c.transfer == Transfer::HLG
               ? kCVImageBufferTransferFunction_ITU_R_2100_HLG
               : kCVImageBufferTransferFunction_ITU_R_2020;
  } else if (c.primaries == Primaries::SMPTE432) {
    prim = kCVImageBufferColorPrimaries_P3_D65;
  }
  const void* keys[] = {kCVImageBufferColorPrimariesKey,
                        kCVImageBufferTransferFunctionKey,
                        kCVImageBufferYCbCrMatrixKey};
  const void* values[] = {prim, xfer, mat};
  CFDictionaryRef tags = CFDictionaryCreate(
      nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CGColorSpaceRef cs = CVImageBufferCreateColorSpaceFromAttachments(tags);
  CFRelease(tags);
  return cs ? cs : create_output_space(c);
}

Status
convert_picture(const fs::path& in, const fs::path& out,
                const ColorInfo& color)
{
  @autoreleasepool {
    NSURL* src = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:in.c_str()]];
    CIImage* img = [CIImage imageWithContentsOfURL:src options:@{
      kCIImageApplyOrientationProperty : @YES,
    }];
    if (!img) {
      return make_error(Code::Io, std::format("cannot read {}",
                                              in.string()));
    }
    CGColorSpaceRef cs = create_output_space(color);
    NSError* err = nil;
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    const BOOL ok = [decode_context()
        writePNGRepresentationOfImage:img
                                toURL:[NSURL fileURLWithPath:
                                    [NSString stringWithUTF8String:
                                        out.c_str()]]
                               format:kCIFormatRGBA16
                           colorSpace:cs
                              options:@{}
                                error:&err];
    CGColorSpaceRelease(cs);
    if (!ok) {
      return make_error(Code::Io, std::format(
          "cannot write {}: {}", out.string(),
          err ? err.localizedDescription.UTF8String : "?"));
    }
    return ok_status();
  }
}

Status
decode_movie_stack(const MovieStack& stack, Rational rate_in,
                   std::int64_t first, std::int64_t count, PixelSize size,
                   const FrameTarget& target, const FrameDone& done,
                   const FramePreview* preview, const ColorInfo* color)
{
  if (size.width <= 0 || size.height <= 0) {
    return make_error(Code::InvalidArgument, "bad target geometry");
  }
  @autoreleasepool {
    VALTZ_ASSIGN(auto reader, StackReader::open(stack, rate_in));
    // An export's own colour, else the bottom clip's.
    CGColorSpaceRef out_space = color ? create_video_output_space(*color)
                                      : nullptr;
    struct Release {
      CGColorSpaceRef cs;
      ~Release() { if (cs) CGColorSpaceRelease(cs); }
    } release{out_space};
    const std::int64_t n = reader->frames();
    const std::int64_t end = count < 0 ? n : std::min(n, first + count);
    for (std::int64_t f = std::max<std::int64_t>(0, first); f < end; ++f) {
      @autoreleasepool {
        VALTZ_ASSIGN(CIImage* framed, reader->frame(f));
        VALTZ_ASSIGN(FrameBuffer buf, target(f));
        if (buf.size < static_cast<std::size_t>(size.width) * size.height *
                           3 * 2) {
          return make_error(Code::InvalidArgument, "frame buffer too small");
        }
        VALTZ_TRY(render_planar(framed, size, 3, Fit::Crop, /*f16=*/true,
                                /*clamp=*/false,
                                out_space ? out_space : reader->space(),
                                buf.data, buf.gpu));
        if (preview && preview->wants && preview->wants(f)) {
          preview->take(small_picture(framed, preview->edge, f));
        }
        VALTZ_TRY(done(f));
      }
    }
    return ok_status();
  }
}

Status
encode_movie_stack(const MovieStack& stack, Rational rate_in, PixelSize size,
                   const fs::path& sound, const fs::path& out)
{
  if (size.width <= 0 || size.height <= 0) {
    return make_error(Code::InvalidArgument, "bad target geometry");
  }
  @autoreleasepool {
    VALTZ_ASSIGN(auto reader, StackReader::open(stack, rate_in));
    const Rational rate = reader->rate();
    std::error_code ec;
    fs::create_directories(out.parent_path(), ec);
    // The picture alone first; the sound joined after (movie.h).
    const fs::path video = sound.empty()
        ? out : fs::path(out.string() + ".video.mov");
    fs::remove(video, ec);
    NSError* err = nil;
    AVAssetWriter* w = [AVAssetWriter
        assetWriterWithURL:[NSURL fileURLWithPath:@(video.c_str())]
                  fileType:AVFileTypeQuickTimeMovie
                     error:&err];
    if (!w) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              video.string()));
    }
    AVAssetWriterInput* in = [AVAssetWriterInput
        assetWriterInputWithMediaType:AVMediaTypeVideo
                       outputSettings:@{
      AVVideoCodecKey : AVVideoCodecTypeAppleProRes422HQ,
      AVVideoWidthKey : @(size.width),
      AVVideoHeightKey : @(size.height),
      AVVideoColorPropertiesKey : @{
        AVVideoColorPrimariesKey : AVVideoColorPrimaries_ITU_R_709_2,
        AVVideoTransferFunctionKey : AVVideoTransferFunction_ITU_R_709_2,
        AVVideoYCbCrMatrixKey : AVVideoYCbCrMatrix_ITU_R_709_2,
      },
    }];
    in.expectsMediaDataInRealTime = NO;
    AVAssetWriterInputPixelBufferAdaptor* adaptor =
        [AVAssetWriterInputPixelBufferAdaptor
            assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
                                      sourcePixelBufferAttributes:@{
      (id)kCVPixelBufferPixelFormatTypeKey :
          @(kCVPixelFormatType_64RGBAHalf),
      (id)kCVPixelBufferWidthKey : @(size.width),
      (id)kCVPixelBufferHeightKey : @(size.height),
      (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    }];
    [w addInput:in];
    if (![w startWriting]) {
      return make_error(Code::Io, std::format(
          "cannot write {}: {}", video.string(),
          w.error ? w.error.localizedDescription.UTF8String : "?"));
    }
    [w startSessionAtSourceTime:kCMTimeZero];
    CGColorSpaceRef cs709 = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
    CIContext* ctx = decode_context();
    Status st = ok_status();
    for (std::int64_t f = 0; f < reader->frames() && st.ok(); ++f) {
      @autoreleasepool {
        auto img = reader->frame(f);
        if (!img.ok()) {
          st = img.error();
          break;
        }
        CIImage* pic = *img;
        const CGRect e = pic.extent;
        const double sx = size.width / e.size.width;
        const double sy = size.height / e.size.height;
        pic = [pic imageByApplyingTransform:CGAffineTransformMakeScale(sx,
                                                                       sy)];
        CIImage* black = [[CIImage imageWithColor:[CIColor blackColor]]
            imageByCroppingToRect:CGRectMake(0, 0, size.width, size.height)];
        pic = [pic imageByCompositingOverImage:black];
        while (!in.readyForMoreMediaData) {
          [NSThread sleepForTimeInterval:0.002];
        }
        CVPixelBufferRef pb = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(
                nullptr, adaptor.pixelBufferPool, &pb) != kCVReturnSuccess ||
            !pb) {
          st = make_error(Code::Internal, "no frame buffer to encode into");
          break;
        }
        [ctx render:pic toCVPixelBuffer:pb
              bounds:CGRectMake(0, 0, size.width, size.height)
          colorSpace:cs709];
        const CMTime pts = CMTimeMake(f * rate.den, static_cast<int32_t>(
                                                        rate.num));
        if (![adaptor appendPixelBuffer:pb withPresentationTime:pts]) {
          st = make_error(Code::Io, std::format(
              "cannot encode frame {}: {}", f,
              w.error ? w.error.localizedDescription.UTF8String : "?"));
        }
        CVPixelBufferRelease(pb);
      }
    }
    CGColorSpaceRelease(cs709);
    [in markAsFinished];
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [w finishWritingWithCompletionHandler:^{
      dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (st.ok() && w.status != AVAssetWriterStatusCompleted) {
      st = make_error(Code::Io, std::format(
          "cannot finish {}: {}", video.string(),
          w.error ? w.error.localizedDescription.UTF8String : "?"));
    }
    if (!st.ok() || sound.empty()) {
      return st;
    }
    st = add_soundtrack(video, sound, out);
    fs::remove(video, ec);
    return st;
  }
}

Status
write_stack_frame(const MovieStack& stack, Rational rate,
                  std::int64_t frame, const fs::path& out)
{
  @autoreleasepool {
    VALTZ_ASSIGN(auto reader, StackReader::open(stack, rate));
    // Read up to it: the clips go forward from where each first shows.
    CIImage* img = nil;
    const std::int64_t f = std::clamp<std::int64_t>(
        frame, 0, std::max<std::int64_t>(0, reader->frames() - 1));
    VALTZ_ASSIGN(img, reader->frame(f));
    return write_png16(img, out);
  }
}

Status
write_movie_frame(const fs::path& src, double seconds, const fs::path& out)
{
  @autoreleasepool {
    VALTZ_ASSIGN(auto c, ClipCursor::open(src, seconds));
    CIImage* img = c->at(seconds);
    if (!img) {
      return make_error(Code::Corrupt, std::format(
          "{} has no frame at {:.3f} s", src.filename().string(), seconds));
    }
    return write_png16(img, out);
  }
}

// ---- a clip's stack, for the screen ------------------------------------

struct StackRenderer::Impl {
  MovieStack            stack;
  Rational              rate;
  std::vector<CIImage*> stills;    // per layer; nil for a clip
  PixelSize             own;       // the bottom clip's frame
  PixelSize             size;      // what it draws
  std::int64_t          frames = 0;
  CIContext*            context = nil;
  CGColorSpaceRef       space = nullptr;

  ~Impl()
  {
    if (space) {
      CGColorSpaceRelease(space);
    }
  }

  // The layers, each with its picture at frame `f`: a clip's as the
  // player decoded it at its source time (one per video layer, in order).
  Result<CIImage*>
  compose(std::int64_t f, const std::vector<CIImage*>& clip_frames) const
  {
    const auto timings = timings_of(stack);
    const bool legacy = bottom_sets_frame(stack);
    const double t = static_cast<double>(f) * rate.den / rate.num;
    const LayerWeights w = layer_weights(timings, stack.transitions, t);
    std::vector<ComposeLayer> in;
    in.reserve(stack.layers.size());
    std::size_t next_clip = 0;
    for (std::size_t i = 0; i < stack.layers.size(); ++i) {
      const MovieLayer& l = stack.layers[i];
      const bool on = timings[i].active(t);
      CIImage* img = nil;
      if (l.video) {
        CIImage* got = next_clip < clip_frames.size()
                           ? clip_frames[next_clip] : nil;
        ++next_clip;
        img = on || (legacy && i == 0) ? got : nil;
      } else if (!l.audio_only && on) {
        img = stills[i];
      }
      if (legacy && i == 0 && !img) {
        // Past the bottom clip's end: its frame, empty.
        img = [[CIImage imageWithColor:[CIColor clearColor]]
            imageByCroppingToRect:CGRectMake(0, 0, own.width, own.height)];
      }
      const double lf = local_frame(timings[i], t, rate);
      in.push_back({img, l.adjust.at(lf), l.crop.at(lf),
                    l.visible && (!l.mask || on), l.mask,
                    legacy && i == 0 ? 1.0 : w.opacity[i]});
    }
    return compose_images(in, stack.canvas);
  }
};

StackRenderer::StackRenderer() : _impl(std::make_unique<Impl>()) {}
StackRenderer::~StackRenderer() = default;

Result<std::unique_ptr<StackRenderer>>
StackRenderer::make(MovieStack stack, Rational rate)
{
  VALTZ_TRY(check_stack(stack));
  @autoreleasepool {
    std::unique_ptr<StackRenderer> r(new StackRenderer());
    Impl& m = *r->_impl;
    m.rate = stack.rate.num > 0 ? stack.rate
             : rate.num > 0     ? rate
                                : Rational{24, 1};
    if (bottom_sets_frame(stack)) {
      VALTZ_ASSIGN(MediaInfo info, probe_file(stack.layers.front().file));
      m.own = {info.frame.width, info.frame.height};
    } else {
      m.own = stack.canvas.frame();
    }
    for (const auto& l : stack.layers) {
      if (l.video || l.audio_only) {
        m.stills.push_back(nil);
      } else {
        VALTZ_ASSIGN(CIImage* img, layer_image(l.file, l.drawn));
        m.stills.push_back(img);
      }
    }
    const auto& bottom = stack.layers.front().crop;
    if (stack.canvas.set()) {
      m.size = {stack.canvas.width, stack.canvas.height};
    } else if (stack.canvas.framed()) {
      m.size = stack.canvas.frame();
    } else if (!bottom.empty() && !bottom.identity()) {
      const auto pl = crop_placement(bottom.at(0), m.own.width,
                                     m.own.height);
      m.size = {static_cast<std::int32_t>(std::lround(pl.canvas_w)),
                static_cast<std::int32_t>(std::lround(pl.canvas_h))};
    } else {
      m.size = m.own;
    }
    m.stack = std::move(stack);
    VALTZ_ASSIGN(m.frames, timeline_frames(m.stack, m.rate));
    // One context for the player's frames: they are small, and come
    // fast.
    CGColorSpaceRef ws =
        CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    m.context = [CIContext contextWithOptions:@{
      kCIContextWorkingColorSpace : (__bridge id)ws,
      kCIContextWorkingFormat : @(kCIFormatRGBAh),
      kCIContextCacheIntermediates : @NO,
    }];
    CGColorSpaceRelease(ws);
    m.space = CGColorSpaceCreateWithName(kCGColorSpaceITUR_709);
    return r;
  }
}

PixelSize
StackRenderer::size() const
{
  return _impl->size;
}

std::int64_t
StackRenderer::frames() const
{
  return _impl->frames;
}

std::vector<std::filesystem::path>
StackRenderer::clips() const
{
  std::vector<std::filesystem::path> out;
  for (const auto& l : _impl->stack.layers) {
    if (l.video) {
      out.push_back(l.file);
    }
  }
  return out;
}

Status
StackRenderer::render(std::int64_t frame,
                      const std::vector<CVPixelBufferRef>& clips,
                      CVPixelBufferRef out) const
{
  if (!out) {
    return make_error(Code::InvalidArgument, "no buffer to draw into");
  }
  @autoreleasepool {
    std::vector<CIImage*> frames;
    frames.reserve(clips.size());
    for (CVPixelBufferRef pb : clips) {
      frames.push_back(pb ? [CIImage imageWithCVPixelBuffer:pb] : nil);
    }
    VALTZ_ASSIGN(CIImage* img, _impl->compose(frame, frames));
    const CGRect bounds = CGRectMake(0, 0, CVPixelBufferGetWidth(out),
                                     CVPixelBufferGetHeight(out));
    // Drawn at the buffer's size: the player's may be smaller than the
    // canvas (its stage's pixels). Core Image reads only what that needs.
    const double sx = bounds.size.width / _impl->size.width;
    const double sy = bounds.size.height / _impl->size.height;
    if (std::abs(sx - 1) > 1e-6 || std::abs(sy - 1) > 1e-6) {
      img = [img imageByApplyingTransform:CGAffineTransformMakeScale(sx,
                                                                     sy)];
    }
    // Clear first: what the stack does not reach shows nothing.
    CIImage* clear = [[CIImage imageWithColor:[CIColor clearColor]]
        imageByCroppingToRect:bounds];
    [_impl->context render:[img imageByCompositingOverImage:clear]
           toCVPixelBuffer:out
                    bounds:bounds
                colorSpace:_impl->space];
    return ok_status();
  }
}

Result<CGImageRef>
StackRenderer::still(std::int64_t frame) const
{
  @autoreleasepool {
    std::vector<CIImage*> frames;
    std::vector<std::unique_ptr<ClipCursor>> cursors;
    const double t = static_cast<double>(frame) * _impl->rate.den /
                     _impl->rate.num;
    for (const auto& l : _impl->stack.layers) {
      if (l.video) {
        const double s = l.timing.source_at(t);
        VALTZ_ASSIGN(auto c, ClipCursor::open(l.file, s));
        frames.push_back(c->at(s));
        cursors.push_back(std::move(c));
      }
    }
    VALTZ_ASSIGN(CIImage* img, _impl->compose(frame, frames));
    CGImageRef out = [_impl->context
        createCGImage:img
             fromRect:CGRectMake(0, 0, _impl->size.width,
                                 _impl->size.height)
               format:kCIFormatRGBA8
           colorSpace:_impl->space];
    if (!out) {
      return make_error(Code::Internal, "cannot draw the frame");
    }
    return out;
  }
}

}
