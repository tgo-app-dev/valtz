#include "testing.h"

#include "valtz/media/model-input.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace valtz;

namespace {

struct Px {
  std::uint8_t r, g, b, a;
};

// Write `w` x `h` sRGB pixels (straight alpha, row 0 on top) as `uti`,
// tagged with EXIF `orientation`.
bool
write_image(const std::filesystem::path& path, int w, int h,
            const std::vector<Px>& px, CFStringRef uti, int orientation = 1)
{
  std::vector<std::uint8_t> pre(static_cast<std::size_t>(w) * h * 4);
  for (std::size_t i = 0; i < px.size(); ++i) {
    const unsigned a = px[i].a;
    pre[i * 4 + 0] = static_cast<std::uint8_t>((px[i].r * a + 127) / 255);
    pre[i * 4 + 1] = static_cast<std::uint8_t>((px[i].g * a + 127) / 255);
    pre[i * 4 + 2] = static_cast<std::uint8_t>((px[i].b * a + 127) / 255);
    pre[i * 4 + 3] = px[i].a;
  }
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      pre.data(), w, h, 8, static_cast<size_t>(w) * 4, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast));
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  CGImageDestinationRef dst =
      CGImageDestinationCreateWithURL(url, uti, 1, nullptr);
  CFNumberRef o = CFNumberCreate(nullptr, kCFNumberIntType, &orientation);
  const void* keys[] = {kCGImagePropertyOrientation};
  const void* vals[] = {o};
  CFDictionaryRef props = CFDictionaryCreate(
      nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
  CGImageDestinationAddImage(dst, img, props);
  const bool ok = CGImageDestinationFinalize(dst);
  CFRelease(props);
  CFRelease(o);
  CFRelease(dst);
  CFRelease(url);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

bool
near(int a, int b, int tol = 2)
{
  return std::abs(a - b) <= tol;
}

const Px kRed{255, 0, 0, 255};
const Px kGreen{0, 255, 0, 255};
const Px kBlue{0, 0, 255, 255};

}

// Crop covers the target and cuts the overhang evenly: a 4x2 strip of
// red | green | green | blue cropped to 2x2 keeps the two green columns.
TEST(model_input, crop_keeps_the_centre_planar)
{
  auto dir = test::temp_dir("model-input");
  const auto p = dir / "strip.png";
  REQUIRE(write_image(p, 4, 2, {kRed, kGreen, kGreen, kBlue,
                                kRed, kGreen, kGreen, kBlue},
                      CFSTR("public.png")));
  auto sz = media::oriented_size(p);
  REQUIRE_OK(sz);
  CHECK(sz->width == 4);
  CHECK(sz->height == 2);
  std::vector<std::uint8_t> out(3 * 2 * 2, 7);
  REQUIRE_OK(media::decode_planar_srgb(p, {2, 2}, 3, media::Fit::Crop,
                                       out.data(), out.size()));
  bool green = true;
  for (int i = 0; i < 4; ++i) {
    green = green && near(out[i], 0) && near(out[4 + i], 255) &&
            near(out[8 + i], 0);
  }
  CHECK(green);
}

// EXIF orientation 6 (turn 90° clockwise to display): the stored left
// edge becomes the top, and the displayed size is the transposed one.
TEST(model_input, orientation_is_applied)
{
  auto dir = test::temp_dir("model-input");
  const auto p = dir / "rotated.tiff";
  REQUIRE(write_image(p, 4, 2, {kRed, kRed, kBlue, kBlue,
                                kRed, kRed, kBlue, kBlue},
                      CFSTR("public.tiff"), 6));
  auto sz = media::oriented_size(p);
  REQUIRE_OK(sz);
  CHECK(sz->width == 2);
  CHECK(sz->height == 4);
  std::vector<std::uint8_t> out(3 * 1 * 2);
  REQUIRE_OK(media::decode_planar_srgb(p, {1, 2}, 3, media::Fit::Stretch,
                                       out.data(), out.size()));
  // Planar [3,2,1]: R plane = top, bottom. Lanczos blends across the
  // red/blue edge of a 4-pixel picture (249 / 66 measured), so "red on
  // top, blue at the bottom" is what is checked: clearly, not purely.
  CHECK(out[0] > 200 && out[0] > out[1] + 120);  // R: top red, bottom not
  CHECK(out[5] > 200 && out[5] > out[4] + 120);  // B: bottom blue, top not
}

// Four channels come back with STRAIGHT alpha: a half-transparent red is
// still full red, with alpha ~128.
TEST(model_input, alpha_is_straight)
{
  auto dir = test::temp_dir("model-input");
  const auto p = dir / "alpha.png";
  REQUIRE(write_image(p, 1, 1, {{255, 0, 0, 128}}, CFSTR("public.png")));
  std::vector<std::uint8_t> out(4);
  REQUIRE_OK(media::decode_planar_srgb(p, {1, 1}, 4, media::Fit::Crop,
                                       out.data(), out.size()));
  CHECK(near(out[0], 255, 3));
  CHECK(near(out[1], 0));
  CHECK(near(out[3], 128));
  // And a buffer that is too small is refused, not overrun.
  CHECK(!media::decode_planar_srgb(p, {1, 1}, 4, media::Fit::Crop,
                                   out.data(), 3).ok());
}

// ---- deep pictures, RAW, adjustments -----------------------------------

namespace {

// A 64x2 sRGB ramp in 16 bits: 0.30 rising by 40/65535 a pixel -- finer
// than 8 bits can step (1/255), coarser than half floats near 0.3.
bool
write_ramp16(const std::filesystem::path& path)
{
  const int w = 64, h = 2;
  std::vector<std::uint16_t> px(static_cast<std::size_t>(w) * h * 4);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      const auto v = static_cast<std::uint16_t>(19661 + 40 * x);
      std::uint16_t* p = &px[(static_cast<std::size_t>(y) * w + x) * 4];
      p[0] = p[1] = p[2] = v;
      p[3] = 65535;
    }
  }
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      px.data(), w, h, 16, static_cast<size_t>(w) * 8, cs,
      static_cast<std::uint32_t>(kCGImageAlphaPremultipliedLast) |
          static_cast<std::uint32_t>(kCGBitmapByteOrder16Little));
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  CFURLRef url = CFURLCreateFromFileSystemRepresentation(
      nullptr, reinterpret_cast<const UInt8*>(path.c_str()),
      static_cast<CFIndex>(std::strlen(path.c_str())), false);
  CGImageDestinationRef dst =
      CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, nullptr);
  CGImageDestinationAddImage(dst, img, nullptr);
  const bool ok = CGImageDestinationFinalize(dst);
  CFRelease(dst);
  CFRelease(url);
  CGImageRelease(img);
  CGContextRelease(ctx);
  CGColorSpaceRelease(cs);
  return ok;
}

float
srgb_from_linear(float l)
{
  return l <= 0.0031308f ? 12.92f * l
                         : 1.055f * std::pow(l, 1.0f / 2.4f) - 0.055f;
}

float
linear_from_srgb(float s)
{
  return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

}

// F16 keeps what 8 bits cannot: a 16-bit ramp whose 64 steps fall
// between 8-bit codes comes back with most of them, where u8 has ~10.
TEST(model_input, f16_keeps_a_sixteen_bit_ramp)
{
  auto dir = test::temp_dir("model-input");
  const auto p = dir / "ramp16.png";
  REQUIRE(write_ramp16(p));
  std::vector<_Float16> f(3 * 64 * 2);
  REQUIRE_OK(media::decode_planar(p, {64, 2}, 3, media::Fit::Stretch, {},
                                  media::Sample::F16, f.data(),
                                  f.size() * sizeof(_Float16)));
  std::vector<std::uint8_t> u(3 * 64 * 2);
  REQUIRE_OK(media::decode_planar_srgb(p, {64, 2}, 3, media::Fit::Stretch,
                                       u.data(), u.size()));
  std::vector<float> fv;
  std::vector<int> uv;
  for (int x = 0; x < 64; ++x) {
    if (fv.empty() || (float)f[x] != fv.back()) { fv.push_back(f[x]); }
    if (uv.empty() || u[x] != uv.back()) { uv.push_back(u[x]); }
  }
  std::printf("[model_input] ramp: %zu distinct f16 steps, %zu u8; "
              "f16 %.4f..%.4f\n", fv.size(), uv.size(), (float)f[0],
              (float)f[63]);
  CHECK(fv.size() > 30);
  CHECK(uv.size() <= 12);
  CHECK(std::fabs((float)f[32] - (19661 + 40 * 32) / 65535.0f) < 0.002f);
}

// What the stage shows is what the model gets: +1 EV doubles the LIGHT
// (mid-grey 0.5 sRGB -> 0.687), and the result is clamped to the model's
// 0..1 -- a light grey pushed +2 EV (2.9 in linear light) reads 1.
TEST(model_input, adjustments_apply_in_linear_light_and_clamp)
{
  auto dir = test::temp_dir("model-input");
  const auto p = dir / "grey.png";
  REQUIRE(write_image(p, 2, 2, {{128, 128, 128, 255}, {128, 128, 128, 255},
                                {128, 128, 128, 255}, {128, 128, 128, 255}},
                      CFSTR("public.png")));
  const auto light = dir / "light.png";
  REQUIRE(write_image(light, 2, 2, {{220, 220, 220, 255},
                                    {220, 220, 220, 255},
                                    {220, 220, 220, 255},
                                    {220, 220, 220, 255}},
                      CFSTR("public.png")));
  auto value = [&](const std::filesystem::path& file,
                   const media::Adjustments& a) {
    std::vector<_Float16> f(3 * 2 * 2);
    auto st = media::decode_planar(file, {2, 2}, 3, media::Fit::Crop, a,
                                   media::Sample::F16, f.data(),
                                   f.size() * sizeof(_Float16));
    return st.ok() ? (float)f[0] : -1.0f;
  };
  auto at = [&](const media::Adjustments& a) { return value(p, a); };
  const float grey = at({});
  media::Adjustments up;
  up.exposure = 1;
  media::Adjustments far;
  far.exposure = 2;
  const float want = srgb_from_linear(2 * linear_from_srgb(128 / 255.0f));
  const float clamped = value(light, far);
  std::printf("[model_input] grey %.4f, +1 EV %.4f (want %.4f); light grey "
              "+2 EV %.4f\n", grey, at(up), want, clamped);
  CHECK(std::fabs(grey - 128 / 255.0f) < 0.003f);
  CHECK(std::fabs(at(up) - want) < 0.01f);
  CHECK(clamped > 0.99f && clamped <= 1.0f);   // 0.9995: half rounding
}

// The adjustments as data: only what moved is written, out-of-range
// values are clamped to the slider, and the chain names its filters in
// the panel's order.
TEST(model_input, adjustments_are_data)
{
  media::Adjustments a;
  CHECK(a.identity());
  CHECK(media::filter_chain(a).empty());
  a = media::adjustments_from_json(
      Json{{"exposure", 5.0}, {"vibrance", 0.3}, {"tint", -0.2},
           {"nonsense", 1}});
  CHECK(a.exposure == 2.0);   // clamped to the slider's range
  CHECK(a.vibrance == 0.3);
  const Json j = media::to_json(a);
  CHECK(j.size() == 3);
  CHECK(media::adjustments_from_json(j) == a);
  const auto chain = media::filter_chain(a);
  REQUIRE(chain.size() == 3);
  CHECK(chain[0].filter == "CITemperatureAndTint");
  CHECK(chain[1].filter == "CIExposureAdjust");
  CHECK(chain[2].filter == "CIVibrance");
  const Json cj = media::to_json(chain);
  CHECK(cj[1]["params"]["inputEV"].get<double>() == 2.0);
  CHECK(cj[0]["params"]["inputNeutral"].size() == 2);
}

// A camera RAW is developed, upright, at the size asked for, in F16.
// Gated on VALTZ_TEST_RAW (a camera RAW file).
TEST(model_input, camera_raw_is_developed_upright)
{
  const char* path = std::getenv("VALTZ_TEST_RAW");
  if (path == nullptr) {
    SKIP("set VALTZ_TEST_RAW to a camera RAW");
  }
  auto sz = media::oriented_size(path);
  REQUIRE_OK(sz);
  const media::PixelSize feed{sz->width / 10, sz->height / 10};
  std::vector<_Float16> f(3 * static_cast<std::size_t>(feed.width) *
                          feed.height);
  REQUIRE_OK(media::decode_planar(path, feed, 3, media::Fit::Crop, {},
                                  media::Sample::F16, f.data(),
                                  f.size() * sizeof(_Float16)));
  double sum = 0;
  float lo = 1, hi = 0;
  for (_Float16 v : f) {
    sum += (float)v;
    lo = std::min(lo, (float)v);
    hi = std::max(hi, (float)v);
  }
  std::printf("[model_input] raw %dx%d (upright) -> %dx%d f16 [%.3f, %.3f] "
              "mean %.3f\n", sz->width, sz->height, feed.width, feed.height,
              lo, hi, sum / f.size());
  CHECK(lo >= 0.0f && hi <= 1.0f);
  CHECK(sum / f.size() > 0.05 && sum / f.size() < 0.95);
}

// The crop is made real in the decode, after the adjustments: an all-
// green 16x16 picture at half its size on its canvas, offset a quarter
// left, over blue padding, comes out with the padding round it -- the
// right half all blue, the green square in the left half.
TEST(model_input, crop_places_the_picture_on_its_canvas)
{
  auto dir = test::temp_dir("model-input-crop");
  const auto p = dir / "green.png";
  REQUIRE(write_image(p, 16, 16, std::vector<Px>(256, kGreen),
                      CFSTR("public.png")));
  media::Crop c;
  c.content = {16, 16};
  c.scale_x = c.scale_y = 0.5;
  c.offset_x = -0.25;
  c.pad = {0, 0, 1, 1};
  std::vector<std::uint8_t> out(3 * 16 * 16, 7);
  REQUIRE_OK(media::decode_planar(p, {16, 16}, 3, media::Fit::Crop, {},
                                  media::Sample::U8, out.data(), out.size(),
                                  media::Space::Srgb, c));
  auto px = [&](int x, int y, int ch) {
    return int(out[ch * 256 + y * 16 + x]);
  };
  // The green square spans x 0..8, y 4..12.
  CHECK(near(px(4, 8, 1), 255) && near(px(4, 8, 2), 0));
  // Everything right of it, and the top and bottom rows, are padding.
  CHECK(near(px(12, 8, 2), 255) && near(px(12, 8, 1), 0));
  CHECK(near(px(4, 1, 2), 255) && near(px(4, 14, 2), 255));
}
