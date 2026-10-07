// The GPU path of decode_planar (media::GpuTarget): Core Image draws, a
// compute kernel writes the planes into a Metal buffer -- and must write
// exactly what the CPU path writes, for every case the CPU path covers.
#include "testing.h"

#include "valtz/media/layers.h"
#include "valtz/media/markup.h"
#include "valtz/media/model-input.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
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
