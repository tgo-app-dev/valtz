// The GPU path of decode_planar (media::GpuTarget): Core Image draws, a
// compute kernel writes the planes into a Metal buffer -- and must write
// exactly what the CPU path writes, for every case the CPU path covers.
#include "testing.h"

#include "valtz/media/layers.h"
#include "valtz/media/markup.h"
#include "valtz/media/model-input.h"
#include "valtz/media/probe.h"
#include "valtz/project/project.h"

#import <AVFoundation/AVFoundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

using namespace valtz;

namespace {

// A picture with something in every channel: gradients, a soft alpha
// ramp (straight alpha out exercises the divide), a patch of full
// transparency.
std::filesystem::path
test_picture(int w, int h)
{
  const auto path = test::temp_dir("gpu") / "pic.png";
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
      const unsigned a = x < w / 8 ? 0 : 64 + 191 * y / (h - 1);
      p[0] = static_cast<std::uint8_t>(255 * x / (w - 1) * a / 255);
      p[1] = static_cast<std::uint8_t>(255 * y / (h - 1) * a / 255);
      p[2] = static_cast<std::uint8_t>(((x * 7 + y * 3) & 255) * a / 255);
      p[3] = static_cast<std::uint8_t>(a);
    }
  }
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
  CGImageDestinationRef d = CGImageDestinationCreateWithURL(
      (__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
  CGImageDestinationAddImage(d, img, nullptr);
  CGImageDestinationFinalize(d);
  CFRelease(d);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return path;
}

}

TEST(model_input, gpu_planes_match_the_cpu)
{
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  if (!dev) {
    SKIP("no Metal device");
  }
  const auto src = test_picture(97, 61);  // odd sizes: partial groups
  struct Case {
    media::PixelSize size;
    int              channels;
    media::Space     space;
    bool             adjusted;
    bool             cropped;
  };
  const Case cases[] = {
    {{97, 61}, 3, media::Space::Srgb, false, false},
    {{97, 61}, 4, media::Space::Srgb, false, false},
    {{64, 48}, 3, media::Space::Srgb, true, false},
    {{80, 80}, 4, media::Space::Linear, true, true},
    {{50, 31}, 3, media::Space::Srgb, false, true},
  };
  media::Adjustments adj;
  adj.exposure = 0.7;
  adj.contrast = 0.3;
  adj.saturation = -0.4;
  media::Crop crop;
  crop.scale_x = crop.scale_y = 1.4;
  crop.rotate = 12;
  crop.offset_x = 0.1;
  for (const auto& c : cases) {
    const std::size_t n = static_cast<std::size_t>(c.size.width) *
                          c.size.height * c.channels;
    std::vector<_Float16> cpu(n);
    REQUIRE_OK(media::decode_planar(
        src, c.size, c.channels, media::Fit::Crop,
        c.adjusted ? adj : media::Adjustments{}, media::Sample::F16,
        cpu.data(), n * 2, c.space, c.cropped ? crop : media::Crop{}));
    // A lease sits part-way into its buffer.
    const std::size_t offset = 256;
    id<MTLBuffer> buf = [dev newBufferWithLength:offset + n * 2
                                         options:MTLResourceStorageModeShared];
    std::memset(buf.contents, 0, buf.length);
    // The CPU view stays untouched when the GPU drew the planes.
    std::vector<_Float16> untouched(n, static_cast<_Float16>(-7));
    REQUIRE_OK(media::decode_planar(
        src, c.size, c.channels, media::Fit::Crop,
        c.adjusted ? adj : media::Adjustments{}, media::Sample::F16,
        untouched.data(), n * 2, c.space, c.cropped ? crop : media::Crop{},
        {(__bridge void*)buf, offset}));
    CHECK(untouched[0] == static_cast<_Float16>(-7));
    CHECK(untouched[n - 1] == static_cast<_Float16>(-7));
    const auto* gpu = reinterpret_cast<const _Float16*>(
        static_cast<const std::uint8_t*>(buf.contents) + offset);
    std::size_t differ = 0;
    float worst = 0;
    for (std::size_t i = 0; i < n; ++i) {
      const float d = std::abs(static_cast<float>(gpu[i]) -
                               static_cast<float>(cpu[i]));
      if (d > 0) { ++differ; }
      worst = std::max(worst, d);
    }
    // The same samples: bit for bit, or within a half-float step.
    CHECK(worst <= 1.0f / 1024);
    CHECK(differ <= n / 1000);
    std::fprintf(stderr, "[gpu_planar] %dx%d c%d: %zu of %zu samples "
                 "differ, worst %g\n", c.size.width, c.size.height,
                 c.channels, differ, n, worst);
  }
}

// How long a model input takes to make, each way, at a camera's size
// (VALTZ_TEST_BENCH=1): the CPU path draws, reads back, then splits the
// planes on the CPU; the GPU path writes them where vpipe reads them.
TEST(model_input, gpu_planes_bench)
{
  if (!std::getenv("VALTZ_TEST_BENCH")) {
    SKIP("set VALTZ_TEST_BENCH=1 to time it");
  }
  id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
  if (!dev) {
    SKIP("no Metal device");
  }
  const auto src = test_picture(4000, 3000);
  const media::PixelSize size{4000, 3000};
  const std::size_t n = 4000ull * 3000 * 3;
  media::Adjustments adj;
  adj.exposure = 0.3;
  std::vector<_Float16> cpu(n);
  id<MTLBuffer> buf = [dev newBufferWithLength:n * 2
                                       options:MTLResourceStorageModeShared];
  auto ms = [](auto t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
  };
  for (int round = 0; round < 3; ++round) {
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE_OK(media::decode_planar(src, size, 3, media::Fit::Crop, adj,
                                    media::Sample::F16, cpu.data(), n * 2));
    const double tc = ms(t0);
    t0 = std::chrono::steady_clock::now();
    REQUIRE_OK(media::decode_planar(
        src, size, 3, media::Fit::Crop, adj, media::Sample::F16,
        buf.contents, n * 2, media::Space::Srgb, {},
        {(__bridge void*)buf, 0}));
    const double tg = ms(t0);
    std::fprintf(stderr, "[gpu_planar] 4000x3000 F16 RGB: cpu %.1f ms, "
                 "gpu %.1f ms\n", tc, tg);
  }
}

// The stage composing a stack (VALTZ_TEST_BENCH=1): as it was -- the core
// writes a 16-bit PNG, the app reads it back -- and as it is now, into a
// surface the layer shows.
TEST(model_input, stack_surface_bench)
{
  if (!std::getenv("VALTZ_TEST_BENCH")) {
    SKIP("set VALTZ_TEST_BENCH=1 to time it");
  }
  const media::PixelSize canvas{4000, 3000};
  media::LayerPicture bottom;
  bottom.file = test_picture(canvas.width, canvas.height);
  bottom.adjust.exposure = 0.3;
  media::LayerPicture marks;
  auto drawn = media::render_markup_picture(
      {}, canvas,
      Json::parse(R"([{"id":"a","kind":"rect","rect":[100,100,900,700],
                       "stroke":[1,0,0,1],"width":12}])"),
      {});
  REQUIRE_OK(drawn);
  marks.drawn = *drawn;
  const std::vector<media::LayerPicture> stack{bottom, marks};
  auto ms = [](auto t0) {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
  };
  const auto png = test::temp_dir("gpu") / "stack.png";
  for (int round = 0; round < 3; ++round) {
    auto t0 = std::chrono::steady_clock::now();
    REQUIRE_OK(media::flatten_layers(stack, png));
    NSURL* url = [NSURL fileURLWithPath:@(png.c_str())];
    CGImageSourceRef s =
        CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
    NSDictionary* opts = @{(id)kCGImageSourceShouldCacheImmediately : @YES};
    CGImageRef img = CGImageSourceCreateImageAtIndex(
        s, 0, (__bridge CFDictionaryRef)opts);
    const double tf = ms(t0);
    CGImageRelease(img);
    CFRelease(s);
    t0 = std::chrono::steady_clock::now();
    auto surf = media::flatten_layers_surface(stack);
    REQUIRE_OK(surf);
    const double ts = ms(t0);
    CHECK(IOSurfaceGetWidth(*surf) == 4000);
    CFRelease(*surf);
    std::fprintf(stderr, "[stack] 4000x3000, 2 layers: PNG written and "
                 "read %.1f ms, surface %.1f ms\n", tf, ts);
  }
}

namespace {

// An asymmetric picture with a clear and a half-clear part, `turn`
// shifting its pattern: a flip, a turn or alpha mishandled shows.
void
write_pattern(const std::filesystem::path& path, int w, int h, int turn)
{
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      std::uint8_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
      const unsigned a = x < w / 6 ? 0 : y < h / 3 ? 128 : 255;
      p[0] = static_cast<std::uint8_t>((x * 255 / w + turn) % 256 * a / 255);
      p[1] = static_cast<std::uint8_t>(y * 255 / h * a / 255);
      p[2] = static_cast<std::uint8_t>(((x ^ y) + turn) % 256 * a / 255);
      p[3] = static_cast<std::uint8_t>(a);
    }
  }
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
  CGImageDestinationRef d = CGImageDestinationCreateWithURL(
      (__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
  CGImageDestinationAddImage(d, img, nullptr);
  CGImageDestinationFinalize(d);
  CFRelease(d);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
}

// A file's pixels as 8-bit sRGB, premultiplied RGBA, row 0 at the top.
std::vector<std::uint8_t>
file_pixels(const std::filesystem::path& path, int& w, int& h)
{
  NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
  CGImageSourceRef s =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
  CGImageRef img = CGImageSourceCreateImageAtIndex(s, 0, nullptr);
  CFRelease(s);
  w = static_cast<int>(CGImageGetWidth(img));
  h = static_cast<int>(CGImageGetHeight(img));
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGContextSetBlendMode(ctx, kCGBlendModeCopy);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  CGImageRelease(img);
  return px;
}

// The largest difference between a surface (BGRA) and `want` (RGBA).
int
surface_off(IOSurfaceRef surf, const std::vector<std::uint8_t>& want,
            int w, int h)
{
  if (static_cast<int>(IOSurfaceGetWidth(surf)) != w ||
      static_cast<int>(IOSurfaceGetHeight(surf)) != h) {
    return 256;
  }
  IOSurfaceLock(surf, kIOSurfaceLockReadOnly, nullptr);
  const auto* base = static_cast<const std::uint8_t*>(
      IOSurfaceGetBaseAddress(surf));
  const std::size_t bpr = IOSurfaceGetBytesPerRow(surf);
  int off = 0;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const std::uint8_t* s = base + y * bpr + x * 4;
      const std::uint8_t* p = &want[(static_cast<std::size_t>(y) * w + x) * 4];
      const int d[4] = {s[2] - p[0], s[1] - p[1], s[0] - p[2], s[3] - p[3]};
      for (int k : d) { off = std::max(off, std::abs(k)); }
    }
  }
  IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, nullptr);
  return off;
}

}  // namespace

// The stage's draws keep a file's picture decoded between them (a jog
// wheel's tick decoded every layer's PNG again): drawn as the file path
// draws it, placed, turned, half clear; the same again; a file
// rewritten drawn anew.
TEST(model_input, a_kept_picture_draws_as_its_file)
{
  const auto dir = test::temp_dir("kept");
  const auto a = dir / "a.png", b = dir / "b.png";
  write_pattern(a, 240, 160, 0);
  write_pattern(b, 120, 90, 77);
  media::LayerPicture bottom;
  bottom.file = a;
  media::LayerPicture top;
  top.file = b;
  top.crop.scale_x = top.crop.scale_y = 0.8;
  top.crop.offset_x = 0.2;
  top.crop.offset_y = -0.1;
  top.crop.rotate = 12;
  const std::vector<media::LayerPicture> stack{bottom, top};
  const auto check = [&](const char* when) {
    const auto png = dir / "want.png";
    REQUIRE_OK(media::flatten_layers(stack, png));
    int w = 0, h = 0;
    const auto want = file_pixels(png, w, h);
    auto surf = media::flatten_layers_surface(stack);
    REQUIRE_OK(surf);
    const int off = surface_off(*surf, want, w, h);
    CFRelease(*surf);
    std::fprintf(stderr, "[kept] %s: %dx%d, off by %d\n", when, w, h, off);
    CHECK(off <= 2);
  };
  check("first draw");
  check("kept");
  // Rewritten (a linked file may be): read again.
  write_pattern(b, 120, 90, 160);
  check("rewritten");
}

// A stack of many layers -- past 64 drawn, composed in groups, each drawn
// before the next -- is the picture the same stack makes a few dozen at
// a time (each part fewer than 64, drawn as one chain, then put under the
// next): 150 asymmetric, half-clear layers placed, scaled and turned on
// a framed canvas, one hidden and one off the canvas; on the screen's
// surface as in the PNG.
TEST(model_input, a_stack_of_many_layers_is_drawn_in_groups)
{
  const auto dir = test::temp_dir("many-layers");
  std::vector<std::filesystem::path> pics;
  for (int k = 0; k < 5; ++k) {
    pics.push_back(dir / ("p" + std::to_string(k) + ".png"));
    write_pattern(pics.back(), 40 + 9 * k, 30 + 5 * k, 37 * k);
  }
  std::vector<media::LayerPicture> stack(150);
  for (int i = 0; i < 150; ++i) {
    auto& l = stack[static_cast<std::size_t>(i)];
    l.file = pics[static_cast<std::size_t>(i % 5)];
    l.crop.scale_x = 0.15 + 0.01 * (i % 7);
    l.crop.scale_y = 0.12 + 0.01 * (i % 5);
    l.crop.offset_x = -0.45 + 0.9 * ((i * 37) % 101) / 100.0;
    l.crop.offset_y = -0.45 + 0.9 * ((i * 53) % 97) / 96.0;
    l.crop.rotate = (i % 9) * 4 - 16;
  }
  stack[40].visible = false;
  stack[77].crop.offset_x = 3;  // off the canvas: nothing drawn
  media::StackCanvas sc;
  sc.frame_w = 400;
  sc.frame_h = 300;
  // The reference: in parts of 50, each part's picture the bottom of the
  // next -- a layer on the frame at its own size, as it was made.
  std::filesystem::path below;
  for (int part = 0; part < 3; ++part) {
    std::vector<media::LayerPicture> some;
    if (!below.empty()) {
      media::LayerPicture b;
      b.file = below;
      some.push_back(b);
    }
    some.insert(some.end(), stack.begin() + part * 50,
                stack.begin() + (part + 1) * 50);
    below = dir / ("part" + std::to_string(part) + ".png");
    REQUIRE_OK(media::flatten_layers(some, below, sc));
  }
  int w = 0, h = 0;
  const auto want = file_pixels(below, w, h);
  const auto whole = dir / "whole.png";
  REQUIRE_OK(media::flatten_layers(stack, whole, sc));
  int ww = 0, wh = 0;
  const auto got = file_pixels(whole, ww, wh);
  REQUIRE(ww == w && wh == h);
  int off = 0, past1 = 0;
  for (std::size_t i = 0; i < got.size(); ++i) {
    const int d = std::abs(got[i] - want[i]);
    off = std::max(off, d);
    past1 += d > 1;
  }
  auto surf = media::flatten_layers_surface(stack, sc);
  REQUIRE_OK(surf);
  const int soff = surface_off(*surf, want, w, h);
  CFRelease(*surf);
  std::fprintf(stderr, "[many] %dx%d: the PNG off by %d (%d values by "
               "more than 1), the surface %d\n", w, h, off, past1, soff);
  CHECK(w == 400 && h == 300);
  // Half floats between the groups, 16-bit PNGs between the parts.
  CHECK(off <= 2);
  CHECK(past1 <= 10);
  CHECK(soff <= 2);
}

// A movie whose picture track runs past its last frame (its session ended
// a frame late, the last frame lasting two): counted by the last frame's
// own time, 48 frames -- the track's length made 49, one that does not
// exist, where a playhead at the end stood and a grab drew nothing.
TEST(model_input, a_movie_is_counted_by_its_last_frame)
{
  const auto path = test::temp_dir("last-frame") / "long-last.mov";
  NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
  NSError* err = nil;
  AVAssetWriter* w = [AVAssetWriter assetWriterWithURL:url
                                              fileType:AVFileTypeQuickTimeMovie
                                                 error:&err];
  REQUIRE(w != nil);
  AVAssetWriterInput* in = [AVAssetWriterInput
      assetWriterInputWithMediaType:AVMediaTypeVideo
                     outputSettings:@{
                       AVVideoCodecKey : AVVideoCodecTypeH264,
                       AVVideoWidthKey : @64,
                       AVVideoHeightKey : @64,
                     }];
  in.expectsMediaDataInRealTime = NO;
  AVAssetWriterInputPixelBufferAdaptor* ad =
      [AVAssetWriterInputPixelBufferAdaptor
          assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
                                     sourcePixelBufferAttributes:@{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey : @64,
            (id)kCVPixelBufferHeightKey : @64,
          }];
  [w addInput:in];
  REQUIRE([w startWriting]);
  [w startSessionAtSourceTime:kCMTimeZero];
  for (int i = 0; i < 48; ++i) {
    while (!in.readyForMoreMediaData) {
      [NSThread sleepForTimeInterval:0.002];
    }
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferPoolCreatePixelBuffer(nullptr, ad.pixelBufferPool, &pb);
    REQUIRE(pb != nullptr);
    CVPixelBufferLockBaseAddress(pb, 0);
    std::memset(CVPixelBufferGetBaseAddress(pb), i * 5,
                CVPixelBufferGetDataSize(pb));
    CVPixelBufferUnlockBaseAddress(pb, 0);
    [ad appendPixelBuffer:pb withPresentationTime:CMTimeMake(i, 24)];
    CVPixelBufferRelease(pb);
  }
  [in markAsFinished];
  [w endSessionAtSourceTime:CMTimeMake(49, 24)];
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [w finishWritingWithCompletionHandler:^{
    dispatch_semaphore_signal(done);
  }];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  REQUIRE(w.status == AVAssetWriterStatusCompleted);

  // The file is as meant: its picture track over 48.5 frames long, so
  // counted by its length it rounds to 49.
  AVURLAsset* asset = [AVURLAsset URLAssetWithURL:url options:@{
    AVURLAssetPreferPreciseDurationAndTimingKey : @YES
  }];
  __block CMTime length = kCMTimeInvalid;
  dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
  [asset loadTracksWithMediaType:AVMediaTypeVideo
               completionHandler:^(NSArray<AVAssetTrack*>* tracks,
                                   NSError*) {
    AVAssetTrack* track = tracks.firstObject;
    if (!track) {
      dispatch_semaphore_signal(loaded);
      return;
    }
    [track loadValuesAsynchronouslyForKeys:@[ @"timeRange" ]
                         completionHandler:^{
      length = track.timeRange.duration;
      dispatch_semaphore_signal(loaded);
    }];
  }];
  dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
  REQUIRE(CMTIME_IS_NUMERIC(length));
  CHECK(CMTimeGetSeconds(length) * 24 > 48.5);
  auto info = media::probe_file(path);
  REQUIRE_OK(info);
  CHECK(info->frame_count == 48);
  CHECK(media::last_picture_frame(path, {24, 1}) == 47);
  CHECK(media::last_picture_frame(path) == 47);
}

namespace {

// A clip as a phone keeps one: stored `w` x `h`, its top half red and its
// bottom half blue, turned upright by the track's transform `t`.
std::filesystem::path
turned_movie(const std::string& name, int w, int h, CGAffineTransform t)
{
  const auto path = test::temp_dir("turned") / name;
  NSURL* url = [NSURL fileURLWithPath:@(path.c_str())];
  NSError* err = nil;
  AVAssetWriter* wr = [AVAssetWriter
      assetWriterWithURL:url fileType:AVFileTypeQuickTimeMovie error:&err];
  if (!wr) {
    return {};
  }
  AVAssetWriterInput* in = [AVAssetWriterInput
      assetWriterInputWithMediaType:AVMediaTypeVideo
                     outputSettings:@{
                       AVVideoCodecKey : AVVideoCodecTypeH264,
                       AVVideoWidthKey : @(w),
                       AVVideoHeightKey : @(h),
                     }];
  in.expectsMediaDataInRealTime = NO;
  in.transform = t;
  AVAssetWriterInputPixelBufferAdaptor* ad =
      [AVAssetWriterInputPixelBufferAdaptor
          assetWriterInputPixelBufferAdaptorWithAssetWriterInput:in
                                     sourcePixelBufferAttributes:@{
            (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey : @(w),
            (id)kCVPixelBufferHeightKey : @(h),
          }];
  [wr addInput:in];
  if (![wr startWriting]) {
    return {};
  }
  [wr startSessionAtSourceTime:kCMTimeZero];
  for (int i = 0; i < 24; ++i) {
    while (!in.readyForMoreMediaData) {
      [NSThread sleepForTimeInterval:0.002];
    }
    CVPixelBufferRef pb = nullptr;
    CVPixelBufferPoolCreatePixelBuffer(nullptr, ad.pixelBufferPool, &pb);
    if (!pb) {
      return {};
    }
    CVPixelBufferLockBaseAddress(pb, 0);
    auto* base = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pb));
    const std::size_t row = CVPixelBufferGetBytesPerRow(pb);
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        std::uint8_t* p = base + y * row + static_cast<std::size_t>(x) * 4;
        const bool top = y < h / 2;
        p[0] = top ? 0 : 255;   // B
        p[1] = 0;               // G
        p[2] = top ? 255 : 0;   // R
        p[3] = 255;
      }
    }
    CVPixelBufferUnlockBaseAddress(pb, 0);
    [ad appendPixelBuffer:pb withPresentationTime:CMTimeMake(i, 24)];
    CVPixelBufferRelease(pb);
  }
  [in markAsFinished];
  dispatch_semaphore_t done = dispatch_semaphore_create(0);
  [wr finishWritingWithCompletionHandler:^{
    dispatch_semaphore_signal(done);
  }];
  dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
  return wr.status == AVAssetWriterStatusCompleted
             ? path
             : std::filesystem::path();
}

}

// A clip read for a summary is UPRIGHT, its size turned with it: a
// phone's portrait clip -- stored landscape, a quarter turn in its track
// -- came out upside down (AVFoundation's transform is y-down, Core
// Image's y-up).
TEST(model_input, a_turned_clip_is_read_upright)
{
  struct Case {
    const char*       name;
    CGAffineTransform turn;
    int               w, h;          // upright, as sampled
    const char*       red;           // where the stored top half lands
  };
  // Stored 96 x 64: a quarter turn clockwise (a phone held upright), one
  // the other way, a half turn, none.
  const Case cases[] = {
    {"cw.mov", CGAffineTransformMake(0, 1, -1, 0, 64, 0), 64, 96, "right"},
    {"ccw.mov", CGAffineTransformMake(0, -1, 1, 0, 0, 96), 64, 96, "left"},
    {"half.mov", CGAffineTransformMake(-1, 0, 0, -1, 96, 64), 96, 64,
     "bottom"},
    {"none.mov", CGAffineTransformIdentity, 96, 64, "top"},
  };
  for (const Case& c : cases) {
    const auto path = turned_movie(c.name, 96, 64, c.turn);
    REQUIRE(!path.empty());
    // Within the most: its long edge the long, its short the short.
    auto size = media::sampled_size(path, {576, 320}, 32);
    REQUIRE_OK(size);
    CHECK(size->width == c.w);
    CHECK(size->height == c.h);
    std::vector<std::uint8_t> buf(
        static_cast<std::size_t>(size->width) * size->height * 3);
    std::vector<std::uint8_t> first;
    auto st = media::sample_movie(
        path, 0.5, *size,
        [&](std::int64_t, double) -> Result<media::FrameBuffer> {
          return media::FrameBuffer{buf.data(), buf.size(), {}};
        },
        [&](std::int64_t n) -> Status {
          if (n == 0) {
            first = buf;
          }
          return ok_status();
        });
    REQUIRE_OK(st);
    REQUIRE(!first.empty());
    const int w = size->width;
    const int h = size->height;
    const std::size_t plane = static_cast<std::size_t>(w) * h;
    // Red at x, y: the red plane high, the blue low.
    const auto red_at = [&](int x, int y) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      return first[i] > 180 && first[2 * plane + i] < 80;
    };
    const auto blue_at = [&](int x, int y) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      return first[2 * plane + i] > 180 && first[i] < 80;
    };
    const std::string red = c.red;
    const int l = w / 4, r = w * 3 / 4, t = h / 4, b = h * 3 / 4;
    if (red == "right") {
      CHECK(red_at(r, h / 2) && blue_at(l, h / 2));
    } else if (red == "left") {
      CHECK(red_at(l, h / 2) && blue_at(r, h / 2));
    } else if (red == "bottom") {
      CHECK(red_at(w / 2, b) && blue_at(w / 2, t));
    } else {
      CHECK(red_at(w / 2, t) && blue_at(w / 2, b));
    }
  }
}

namespace {

// Where a picture's red half lies -- right, left, bottom or top -- by
// `red` and `blue`, each a pixel's channel at x, y; "" when it is none.
std::string
red_side(int w, int h, const std::function<int(int, int)>& red,
         const std::function<int(int, int)>& blue)
{
  const auto is_red = [&](int x, int y) {
    return red(x, y) > 180 && blue(x, y) < 80;
  };
  const auto is_blue = [&](int x, int y) {
    return blue(x, y) > 180 && red(x, y) < 80;
  };
  const int l = w / 4, r = w * 3 / 4, t = h / 4, b = h * 3 / 4;
  if (is_red(r, h / 2) && is_blue(l, h / 2)) {
    return "right";
  }
  if (is_red(l, h / 2) && is_blue(r, h / 2)) {
    return "left";
  }
  if (is_red(w / 2, b) && is_blue(w / 2, t)) {
    return "bottom";
  }
  if (is_red(w / 2, t) && is_blue(w / 2, b)) {
    return "top";
  }
  return "";
}

// `img` as 8-bit sRGB RGBA, row 0 at the top.
std::vector<std::uint8_t>
image_pixels(CGImageRef img, int& w, int& h)
{
  w = static_cast<int>(CGImageGetWidth(img));
  h = static_cast<int>(CGImageGetHeight(img));
  std::vector<std::uint8_t> px(static_cast<std::size_t>(w) * h * 4);
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return px;
}

std::string
rgba_red_side(const std::vector<std::uint8_t>& px, int w, int h)
{
  const auto at = [&](int c) {
    return [&, c](int x, int y) {
      return static_cast<int>(
          px[(static_cast<std::size_t>(y) * w + x) * 4 + c]);
    };
  };
  return red_side(w, h, at(0), at(2));
}

}

// A turned clip is UPRIGHT wherever Valtz reads it -- its recorded size,
// a model's input decoded, a timeline's frames, the player's, a frame
// written, a movie drawn of it -- not only in a summary: Valtz's info,
// the frames generation models were handed and the timeline's frames all
// had a phone's portrait clip stored landscape (2026-10-24).
TEST(model_input, a_turned_clip_is_upright_everywhere)
{
  struct Case {
    const char*       name;
    CGAffineTransform turn;
    int               degrees;
    int               w, h;
    const char*       red;
  };
  const Case cases[] = {
    {"cw.mov", CGAffineTransformMake(0, 1, -1, 0, 64, 0), 90, 64, 96,
     "right"},
    {"ccw.mov", CGAffineTransformMake(0, -1, 1, 0, 0, 96), 270, 64, 96,
     "left"},
    {"half.mov", CGAffineTransformMake(-1, 0, 0, -1, 96, 64), 180, 96, 64,
     "bottom"},
    {"none.mov", CGAffineTransformIdentity, 0, 96, 64, "top"},
  };
  for (const Case& c : cases) {
    std::fprintf(stderr, "[turned] %s\n", c.name);
    const auto path = turned_movie(c.name, 96, 64, c.turn);
    REQUIRE(!path.empty());
    // Recorded as it shows.
    auto info = media::probe_file(path);
    REQUIRE_OK(info);
    CHECK(info->frame.width == c.w);
    CHECK(info->frame.height == c.h);
    CHECK(info->turn == c.degrees);
    CHECK(media::movie_turn(path) == c.degrees);

    // A model's input: decoded at its upright size.
    const std::size_t plane = static_cast<std::size_t>(c.w) * c.h;
    std::vector<std::uint8_t> buf(plane * 3), first;
    auto st = media::decode_movie(
        path, {}, {}, Rational{24, 1}, 0, 2, {c.w, c.h},
        [&](std::int64_t) -> Result<media::FrameBuffer> {
          return media::FrameBuffer{buf.data(), buf.size(), {}};
        },
        [&](std::int64_t n) -> Status {
          if (n == 0) {
            first = buf;
          }
          return ok_status();
        },
        nullptr, media::Sample::U8);
    REQUIRE_OK(st);
    REQUIRE(!first.empty());
    CHECK(red_side(
              c.w, c.h,
              [&](int x, int y) { return first[y * c.w + x]; },
              [&](int x, int y) {
                return first[2 * plane + y * c.w + x];
              }) == c.red);

    // A frame written (a still's layer showing it, a grab).
    const auto png = test::temp_dir("turned") /
                     (std::string(c.name) + ".png");
    REQUIRE_OK(media::write_movie_frame(path, 0.1, png));
    int w = 0, h = 0;
    auto px = file_pixels(png, w, h);
    CHECK(w == c.w);
    CHECK(h == c.h);
    CHECK(rgba_red_side(px, w, h) == c.red);

    // On a timeline: drawn alone, and as the player hands its frames
    // over -- as stored.
    media::MovieStack stack;
    media::MovieLayer l;
    l.file = path;
    l.video = true;
    stack.layers.push_back(l);
    stack.rate = Rational{24, 1};
    auto r = media::StackRenderer::make(stack, Rational{24, 1});
    REQUIRE_OK(r);
    CHECK((*r)->size().width == c.w);
    CHECK((*r)->size().height == c.h);
    auto still = (*r)->still(3);
    REQUIRE_OK(still);
    px = image_pixels(*still, w, h);
    CGImageRelease(*still);
    CHECK(w == c.w);
    CHECK(rgba_red_side(px, w, h) == c.red);
    @autoreleasepool {
      AVURLAsset* asset = [AVURLAsset
          URLAssetWithURL:[NSURL fileURLWithPath:@(path.c_str())]
                  options:nil];
      __block AVAssetTrack* track = nil;
      dispatch_semaphore_t got = dispatch_semaphore_create(0);
      [asset loadTracksWithMediaType:AVMediaTypeVideo
                   completionHandler:^(NSArray<AVAssetTrack*>* t, NSError*) {
                     track = t.firstObject;
                     dispatch_semaphore_signal(got);
                   }];
      dispatch_semaphore_wait(got, DISPATCH_TIME_FOREVER);
      REQUIRE(track != nil);
      AVAssetReader* reader = [AVAssetReader assetReaderWithAsset:asset
                                                            error:nil];
      AVAssetReaderTrackOutput* out = [AVAssetReaderTrackOutput
          assetReaderTrackOutputWithTrack:track
                           outputSettings:@{
        (id)kCVPixelBufferPixelFormatTypeKey :
            @(kCVPixelFormatType_32BGRA),
      }];
      [reader addOutput:out];
      REQUIRE([reader startReading]);
      CMSampleBufferRef sb = [out copyNextSampleBuffer];
      REQUIRE(sb != nullptr);
      CVPixelBufferRef raw = CMSampleBufferGetImageBuffer(sb);
      CHECK(static_cast<int>(CVPixelBufferGetWidth(raw)) == 96);
      CVPixelBufferRef drawn = nullptr;
      CVPixelBufferCreate(nullptr, c.w, c.h, kCVPixelFormatType_32BGRA,
                          (__bridge CFDictionaryRef)@{
                            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
                          },
                          &drawn);
      REQUIRE(drawn != nullptr);
      REQUIRE_OK((*r)->render(0, {raw}, drawn));
      CVPixelBufferLockBaseAddress(drawn, kCVPixelBufferLock_ReadOnly);
      const auto* base = static_cast<const std::uint8_t*>(
          CVPixelBufferGetBaseAddress(drawn));
      const std::size_t row = CVPixelBufferGetBytesPerRow(drawn);
      const auto ch = [&](int k) {
        return [&, k](int x, int y) {
          return static_cast<int>(base[y * row + x * 4 + k]);
        };
      };
      CHECK(red_side(c.w, c.h, ch(2), ch(0)) == c.red);
      CVPixelBufferUnlockBaseAddress(drawn, kCVPixelBufferLock_ReadOnly);
      CVPixelBufferRelease(drawn);
      CFRelease(sb);
      [reader cancelReading];
    }

    // A movie of it (a reference handed to vpipe, a nested timeline's
    // rendering): upright in its pixels, no turn left.
    const auto mov = test::temp_dir("turned") /
                     (std::string("upright-") + c.name);
    stack.frames = 12;
    REQUIRE_OK(media::encode_movie_stack(stack, Rational{24, 1},
                                         {c.w, c.h}, {}, mov));
    auto made = media::probe_file(mov);
    REQUIRE_OK(made);
    CHECK(made->frame.width == c.w);
    CHECK(made->turn == 0);
    REQUIRE_OK(media::write_movie_frame(mov, 0.1, png));
    px = file_pixels(png, w, h);
    CHECK(rgba_red_side(px, w, h) == c.red);
  }
}

// A clip recorded before the turn was read (2026-10-24) is read again:
// turned, its version's info is its upright size; upright, it is left as
// recorded.
TEST(model_input, an_old_record_of_a_turned_clip_is_read_again)
{
  const auto cw = turned_movie("old-cw.mov", 96, 64,
                               CGAffineTransformMake(0, 1, -1, 0, 64, 0));
  const auto none = turned_movie("old-none.mov", 96, 64,
                                 CGAffineTransformIdentity);
  REQUIRE(!cw.empty());
  REQUIRE(!none.empty());
  auto pkg = test::temp_dir("turned") / "Old.valtz";
  auto pr = project::Project::create(pkg, "Old");
  REQUIRE_OK(pr);
  project::Project& p = **pr;
  auto a = p.import_file(cw, {});
  auto b = p.import_file(none, {});
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  auto v = p.version(a->id);
  REQUIRE_OK(v);
  CHECK(v->info.frame.width == 64);
  CHECK(v->info.turn == 90);
  // As one before kept them: the stored size, no turn.
  auto doc = p.dump();
  REQUIRE_OK(doc);
  for (auto& ver : (*doc)["tables"]["versions"]) {
    ver["info"].erase("turn");
    ver["info"]["frame"]["w"] = 96;
    ver["info"]["frame"]["h"] = 64;
  }
  REQUIRE_OK(p.load(*doc));
  v = p.version(a->id);
  REQUIRE_OK(v);
  CHECK(v->info.turn == -1);
  CHECK(v->info.frame.width == 96);
  auto turned = p.read_turns(a->id);
  REQUIRE_OK(turned);
  CHECK(*turned);
  v = p.version(a->id);
  REQUIRE_OK(v);
  CHECK(v->info.frame.width == 64);
  CHECK(v->info.frame.height == 96);
  CHECK(v->info.turn == 90);
  turned = p.read_turns(a->id);
  REQUIRE_OK(turned);
  CHECK(!*turned);
  turned = p.read_turns(b->id);
  REQUIRE_OK(turned);
  CHECK(!*turned);
  v = p.version(b->id);
  REQUIRE_OK(v);
  CHECK(v->info.turn == -1);
  CHECK(v->info.frame.width == 96);
}
