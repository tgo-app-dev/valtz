#include "testing.h"

#include "valtz/media/keyframes.h"
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

// THE COLOR MIXER (media/adjust.h): three curves over the wheel, their
// points keyed flat beside the rest ("mix_hue_0_at", "hue@30" read too),
// clamped and wrapped; a cubic through them, round the wheel, never past
// the points either side, one point its value everywhere; its step last
// in the chain, each curve sampled a degree at a time; keys of other
// points eased exactly, hue by hue; and, drawn by its kernel, red turned
// toward orange, blue made the grey of its luminance (not white), red
// darkened in its light, a grey left as it is.
TEST(model_input, the_color_mixer_moves_each_hue)
{
  media::Adjustments a = media::adjustments_from_json(
      Json{{"hue@0", 0.5}, {"hue@60", 0.0}, {"hue@300", 0.0},
           {"mix_sat_0_at", 240.0}, {"mix_sat_0", -2.0}, {"lum@390", 0.3},
           {"saturation", 0.1}});
  REQUIRE(a.mix.hue.points.size() == 3);
  CHECK(a.mix.hue.points[0].hue == 0 && a.mix.hue.points[0].value == 0.5);
  CHECK(a.mix.hue.points[2].hue == 300);
  REQUIRE(a.mix.saturation.points.size() == 1);
  CHECK(a.mix.saturation.points[0].value == -1.0);   // clamped
  REQUIRE(a.mix.luminance.points.size() == 1);
  CHECK(std::fabs(a.mix.luminance.points[0].hue - 30) < 1e-9);  // wrapped
  const Json j = media::to_json(a);
  CHECK(j.size() == 11);
  CHECK(j["mix_hue_2_at"].get<double>() == 300);
  CHECK(media::adjustments_from_json(j) == a);

  // The curves: through their points, between them never past either
  // side, round the wheel from magenta's to red's; one point everywhere.
  const media::MixCurve& hue = a.mix.hue;
  CHECK(hue.at(0) == 0.5 && hue.at(60) == 0 && hue.at(300) == 0);
  CHECK(hue.at(180) == 0);
  CHECK(hue.at(30) > 0.05 && hue.at(30) < 0.45);
  CHECK(hue.at(330) > 0.05 && hue.at(330) < 0.45);
  CHECK(hue.at(359.9) > 0.49);
  bool inside = true;
  for (int d = 0; d < 3600; ++d) {
    const double v = hue.at(d / 10.0);
    if (v < -1e-12 || v > 0.5 + 1e-12) { inside = false; }
  }
  CHECK(inside);
  CHECK(std::fabs(a.mix.luminance.at(200) - 0.3) < 1e-12);
  CHECK(media::MixCurve{}.at(100) == 0);
  // Points at 0 move nothing, and are kept.
  media::Adjustments anchors;
  anchors.mix.hue.points = {{30, 0}, {90, 0}};
  CHECK(anchors.mix.identity());
  CHECK(!anchors.identity());

  const auto chain = media::filter_chain(a);
  REQUIRE(chain.size() == 2);
  CHECK(chain[0].filter == "CIColorControls");
  CHECK(chain[1].filter == media::kColorMixFilter);
  REQUIRE(chain[1].params.size() == 3);
  CHECK(chain[1].params[0].first == "inputHue");
  const auto& samples = chain[1].params[0].second;
  REQUIRE(samples.size() == media::kMixSamples + 1);
  CHECK(samples[0] == 0.5 && samples[180] == 0 && samples[360] == 0.5);

  // Keyed: two keys of other points eased hue by hue -- each key's own
  // curve at its end, between them the two curves' mix, exactly.
  media::Adjustments b;
  b.mix.hue.points = {{120, -0.4}, {200, 0.2}};
  double worst = 0;
  for (const double t : {0.0, 0.25, 1.0}) {
    const media::Adjustments m = media::lerp(a, b, t);
    for (int d = 0; d < 360; d += 7) {
      const double want = (1 - t) * a.mix.hue.at(d) + t * b.mix.hue.at(d);
      worst = std::max(worst, std::fabs(m.mix.hue.at(d) - want));
    }
  }
  std::printf("[model_input] keys eased: worst %.2e\n", worst);
  CHECK(worst < 1e-9);
  // On a key, its own points (not laid on the next key's).
  media::KeyedAdjustments keyed;
  keyed.keys = {{0, a}, {20, b}, {40, a}};
  CHECK(keyed.at(20).mix == b.mix);
  CHECK(keyed.at(10).mix.hue.points.size() == 5);

  // The hue read: red 0, between yellow and green 90, magenta 300, a
  // grey none.
  CHECK(media::mix_hue_of(1, 0, 0)->hue == 0);
  CHECK(std::fabs(media::mix_hue_of(0.5, 1, 0)->hue - 90) < 1e-9);
  CHECK(media::mix_hue_of(1, 0, 1)->hue == 300);
  CHECK(!media::mix_hue_of(0.4, 0.4, 0.4));

  // Drawn: red, blue, a grey -- straight RGBA, sRGB-encoded.
  auto dir = test::temp_dir("model-input");
  const auto p = dir / "hues.png";
  REQUIRE(write_image(p, 3, 1, {{255, 0, 0, 255}, {0, 0, 255, 255},
                                {128, 128, 128, 255}},
                      CFSTR("public.png")));
  auto draw = [&](const media::Adjustments& adj) {
    std::vector<_Float16> f(4 * 3);
    auto st = media::decode_planar(p, {3, 1}, 4, media::Fit::Stretch, adj,
                                   media::Sample::F16, f.data(),
                                   f.size() * sizeof(_Float16));
    std::vector<float> out(f.begin(), f.end());
    if (!st.ok()) { out.assign(12, -1); }
    return out;   // planar: r of each pixel, then g, then b, then a
  };
  media::Adjustments turn;
  turn.mix.hue.points = {{0, 1}, {120, 0}, {240, 0}};   // red 30 degrees on
  turn.mix.saturation.points = {{0, 0}, {120, 0}, {240, -1}};  // blue grey
  turn.mix.luminance.points = {{0, 1}};   // every hue; a grey has none
  const auto t = draw(turn);
  std::printf("[model_input] mixed: red %.3f %.3f %.3f, blue %.3f %.3f "
              "%.3f, grey %.3f %.3f %.3f\n", t[0], t[3], t[6], t[1], t[4],
              t[7], t[2], t[5], t[8]);
  // Red at hue 30, then lit a stop and a half (and clamped): its green
  // up to about half of its red.
  CHECK(t[0] > 0.99f && t[3] > 0.6f && t[6] < 0.05f);
  // Blue grey: its channels together, at its own luminance -- 0.0722
  // in linear light (not white) -- lit a stop and a half with every
  // hue's: 0.204, 0.489 encoded.
  CHECK(std::fabs(t[1] - t[4]) < 0.01f && std::fabs(t[4] - t[7]) < 0.01f);
  CHECK(std::fabs(t[7] - 0.489f) < 0.01f);
  // The grey as it was.
  CHECK(std::fabs(t[2] - 128 / 255.0f) < 0.004f);
  CHECK(std::fabs(t[5] - 128 / 255.0f) < 0.004f);
  media::Adjustments dark;
  dark.mix.luminance.points = {{0, -1}, {120, 0}, {240, 0}};
  const auto d = draw(dark);
  // Red a stop and a half darker in its light: 0.354 linear, 0.629
  // encoded; blue untouched.
  std::printf("[model_input] red darkened %.3f (want 0.629), blue %.3f\n",
              d[0], d[7]);
  CHECK(std::fabs(d[0] - 0.629f) < 0.01f);
  CHECK(d[3] < 0.01f && d[6] < 0.01f);
  CHECK(d[7] > 0.99f);
}

// The look's master ALPHA: read and written while not 1, eased between
// keys, the chain's last step -- drawn, the picture's coverage scaled,
// its colours kept.
TEST(model_input, the_looks_alpha_fades_the_picture)
{
  media::Adjustments a = media::adjustments_from_json(
      Json{{"alpha", 0.25}, {"exposure", 0.5}});
  CHECK(a.alpha == 0.25);
  CHECK(media::to_json(a)["alpha"].get<double>() == 0.25);
  CHECK(!media::to_json(media::Adjustments{}).contains("alpha"));
  CHECK(media::adjustments_from_json(Json{{"alpha", 3.0}}).alpha == 1);
  CHECK(media::adjustments_from_json(Json{{"alpha", -1.0}}).alpha == 0);
  CHECK(media::adjustments_from_json(media::to_json(a)) == a);
  media::Adjustments clear;
  clear.alpha = 0;
  CHECK(std::fabs(media::lerp(media::Adjustments{}, clear, 0.25).alpha -
                  0.75) < 1e-12);
  const auto chain = media::filter_chain(a);
  REQUIRE(!chain.empty());
  CHECK(chain.back().filter == "CIColorMatrix");

  auto dir = test::temp_dir("model-input");
  const auto p = dir / "alpha.png";
  REQUIRE(write_image(p, 2, 1, {{200, 60, 20, 255}, {128, 128, 128, 255}},
                      CFSTR("public.png")));
  auto draw = [&](const media::Adjustments& adj) {
    std::vector<_Float16> f(4 * 2);
    auto st = media::decode_planar(p, {2, 1}, 4, media::Fit::Stretch, adj,
                                   media::Sample::F16, f.data(),
                                   f.size() * sizeof(_Float16));
    std::vector<float> out(f.begin(), f.end());
    if (!st.ok()) { out.assign(8, -1); }
    return out;   // planar: r, g, b, a of each pixel
  };
  media::Adjustments half;
  half.alpha = 0.5;
  const auto plain = draw({});
  const auto faded = draw(half);
  std::printf("[model_input] alpha 0.5: %.3f %.3f (was %.3f %.3f), red "
              "%.3f (was %.3f)\n", faded[6], faded[7], plain[6], plain[7],
              faded[0], plain[0]);
  CHECK(std::fabs(faded[6] - 0.5f) < 0.004f);
  CHECK(std::fabs(faded[7] - 0.5f) < 0.004f);
  // Straight colour: as it was.
  for (int i = 0; i < 6; ++i) {
    CHECK(std::fabs(faded[i] - plain[i]) < 0.004f);
  }
}

// The color grade: a tint for each tone range, the ranges where balance,
// width and blend put them -- read, weighed, eased between keys, drawn.
TEST(model_input, the_color_grade_tints_each_tone)
{
  media::Adjustments a = media::adjustments_from_json(
      Json{{"grade_shadows_hue", 210.0}, {"grade_shadows_sat", 0.4},
           {"grade_midtones_hue", 120.0},   // no saturation: no tint
           {"grade_highlights_hue", 390.0}, {"grade_highlights_sat", 1.5},
           {"grade_balance", 0.2}, {"grade_width", 2.0},
           {"grade_blend", 0.5}, {"exposure", 0.1}});
  CHECK(a.grade.shadows.hue == 210 && a.grade.shadows.saturation == 0.4);
  CHECK(a.grade.midtones == media::GradeTint{});
  CHECK(std::fabs(a.grade.highlights.hue - 30) < 1e-9);   // wrapped
  CHECK(a.grade.highlights.saturation == 1);               // clamped
  CHECK(a.grade.balance == 0.2 && a.grade.width == 1);
  const Json j = media::to_json(a);
  // exposure, two tints of two, balance and width (blend at rest).
  CHECK(j.size() == 7);
  CHECK(!j.contains("grade_midtones_hue") && !j.contains("grade_blend"));
  CHECK(media::adjustments_from_json(j) == a);
  CHECK(media::ColorGrade{}.identity());
  media::Adjustments ranges;
  ranges.grade.width = 0.3;   // set, but nothing tinted: no step
  CHECK(media::filter_chain(ranges).empty());

  // The ranges at rest: shadows below a quarter, highlights above three
  // quarters, half and half at each; every tone's weights sum to 1.
  media::ColorGrade g;
  auto w = [&](double t) { return media::grade_weights(g, t); };
  bool sums = true;
  for (int i = 0; i <= 100; ++i) {
    const auto x = w(i / 100.0);
    if (std::fabs(x.shadows + x.midtones + x.highlights - 1) > 1e-12 ||
        x.shadows < 0 || x.midtones < 0 || x.highlights < 0) {
      sums = false;
    }
  }
  CHECK(sums);
  CHECK(w(0).shadows == 1 && w(1).highlights == 1 && w(0.5).midtones == 1);
  CHECK(std::fabs(w(0.25).shadows - 0.5) < 1e-12);
  CHECK(std::fabs(w(0.25).midtones - 0.5) < 1e-12);
  CHECK(std::fabs(w(0.75).highlights - 0.5) < 1e-12);
  // Blend 0: a clean cut where the width puts it.
  g.blend = 0;
  g.width = 0.3;   // 0.35 and 0.65
  CHECK(w(0.3499).shadows == 1 && w(0.3501).midtones == 1);
  CHECK(w(0.6499).midtones == 1 && w(0.6501).highlights == 1);
  // Balance: the midtones' centre moved toward white.
  g = {};
  g.balance = 1;
  CHECK(w(0.9).midtones == 1);
  CHECK(std::fabs(w(0.45).shadows - 0.5) < 1e-12);   // 0.9 x (1 - 0.5)
  // Width 1: all midtones; width 0 and a full blend: the shadows and the
  // highlights cross over at the centre, no midtones.
  g = {};
  g.width = 1;
  CHECK(w(0).midtones == 1 && w(1).midtones == 1);
  g.width = 0;
  g.blend = 1;
  CHECK(std::fabs(w(0.5).shadows - 0.5) < 1e-12);
  CHECK(std::fabs(w(0.5).highlights - 0.5) < 1e-12);
  CHECK(w(0.3).midtones == 0 && w(0.3).shadows > w(0.3).highlights);

  // A tint's gain: luminance 1 (a grey keeps its brightness), toward its
  // hue; none at no saturation.
  const auto none = media::grade_gain({200, 0});
  CHECK(none[0] == 1 && none[1] == 1 && none[2] == 1);
  double off = 0;
  for (int h = 0; h < 360; h += 15) {
    for (const double s : {0.3, 1.0}) {
      const auto k = media::grade_gain({double(h), s});
      off = std::max(off, std::fabs(0.2126 * k[0] + 0.7152 * k[1] +
                                    0.0722 * k[2] - 1));
    }
  }
  CHECK(off < 1e-12);
  const auto blue = media::grade_gain({240, 0.5});
  CHECK(blue[2] > 1.4 && blue[0] < 1 && blue[1] < 1);

  // Keyed: a tint eases across the disk -- red and its opposite through
  // grey, red and yellow-green by 45 degrees -- the ranges as numbers;
  // on a key its own grade.
  media::Adjustments ka, kb;
  ka.grade.shadows = {0, 0.5};
  kb.grade.shadows = {180, 0.5};
  ka.grade.midtones = {0, 0.4};
  kb.grade.midtones = {90, 0.4};
  kb.grade.balance = 0.5;
  const auto half = media::lerp(ka, kb, 0.5);
  CHECK(half.grade.shadows == media::GradeTint{});
  CHECK(std::fabs(half.grade.midtones.hue - 45) < 1e-9);
  CHECK(std::fabs(half.grade.midtones.saturation - 0.4 * std::sqrt(0.5)) <
        1e-9);
  CHECK(std::fabs(half.grade.balance - 0.25) < 1e-12);
  media::KeyedAdjustments keyed;
  keyed.keys = {{0, ka}, {20, kb}};
  CHECK(keyed.at(20).grade == kb.grade);
  CHECK(keyed.at(0).grade == ka.grade);

  // Drawn: a dark grey, a mid grey, a light grey (one no channel of
  // which the tint takes past 1) -- shadows to blue, highlights to
  // orange, the midtones left.
  auto dir = test::temp_dir("model-input");
  const auto p = dir / "tones.png";
  REQUIRE(write_image(p, 3, 1, {{40, 40, 40, 255}, {128, 128, 128, 255},
                                {200, 200, 200, 255}},
                      CFSTR("public.png")));
  auto draw = [&](const media::Adjustments& adj) {
    std::vector<_Float16> f(4 * 3);
    auto st = media::decode_planar(p, {3, 1}, 4, media::Fit::Stretch, adj,
                                   media::Sample::F16, f.data(),
                                   f.size() * sizeof(_Float16));
    std::vector<float> out(f.begin(), f.end());
    if (!st.ok()) { out.assign(12, -1); }
    return out;   // planar: r of each pixel, then g, then b, then a
  };
  auto linear = [](float c) {
    return c <= 0.04045f ? c / 12.92f
                         : std::pow((c + 0.055f) / 1.055f, 2.4f);
  };
  auto luma = [&](const std::vector<float>& v, int i) {
    return 0.2126f * linear(v[i]) + 0.7152f * linear(v[3 + i]) +
           0.0722f * linear(v[6 + i]);
  };
  media::Adjustments graded;
  graded.grade.shadows = {240, 0.6};
  graded.grade.highlights = {30, 0.6};
  const auto plain = draw({});
  const auto t = draw(graded);
  std::printf("[model_input] graded: dark %.3f %.3f %.3f, mid %.3f %.3f "
              "%.3f, light %.3f %.3f %.3f\n", t[0], t[3], t[6], t[1], t[4],
              t[7], t[2], t[5], t[8]);
  CHECK(t[6] > t[0] + 0.03f);   // the dark grey bluer
  CHECK(t[2] > t[8] + 0.03f);   // the light grey warmer
  // The mid grey as it was; each grey's luminance kept.
  CHECK(std::fabs(t[1] - plain[1]) < 0.004f);
  CHECK(std::fabs(t[7] - plain[7]) < 0.004f);
  for (int i = 0; i < 3; ++i) {
    CHECK(std::fabs(luma(t, i) - luma(plain, i)) <
          0.01f * luma(plain, i) + 1e-4f);
  }
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
