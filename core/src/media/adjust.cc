#include "valtz/media/adjust.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <string>

namespace valtz::media {

namespace {

struct Slider {
  const char* key;
  double Adjustments::*field;
  double lo;
  double hi;
};

constexpr Slider kSliders[] = {
  {"exposure", &Adjustments::exposure, -2, 2},
  {"contrast", &Adjustments::contrast, -1, 1},
  {"highlights", &Adjustments::highlights, -1, 1},
  {"shadows", &Adjustments::shadows, -1, 1},
  {"vibrance", &Adjustments::vibrance, -1, 1},
  {"saturation", &Adjustments::saturation, -1, 1},
  {"temperature", &Adjustments::temperature, -1, 1},
  {"tint", &Adjustments::tint, -1, 1},
};

// The mixer's three curves, as they are keyed ("mix_hue_0").
struct MixChannelKey {
  const char* name;
  MixCurve ColorMix::*curve;
};

constexpr MixChannelKey kMixChannelKeys[] = {
  {"hue", &ColorMix::hue},
  {"sat", &ColorMix::saturation},
  {"lum", &ColorMix::luminance},
};

// Steeper than this a degree no curve of values -1...1 runs between
// points a degree apart (three times its secant, Fritsch and Butland's
// bound): a slope read is held to it.
constexpr double kMaxMixSlope = 8;

// The grade's three ranges and its numbers, as they are keyed
// ("grade_shadows_hue", "grade_balance").
struct GradeRangeKey {
  const char* name;
  GradeTint ColorGrade::*tint;
};

constexpr GradeRangeKey kGradeRangeKeys[] = {
  {"shadows", &ColorGrade::shadows},
  {"midtones", &ColorGrade::midtones},
  {"highlights", &ColorGrade::highlights},
};

struct GradeSlider {
  const char* key;
  double ColorGrade::*field;
  double lo;
  double hi;
  double rest;
};

constexpr GradeSlider kGradeSliders[] = {
  {"grade_balance", &ColorGrade::balance, -1, 1, 0},
  {"grade_width", &ColorGrade::width, 0, 1, 0.5},
  {"grade_blend", &ColorGrade::blend, 0, 1, 0.5},
};

// A change of range no wider than this either side is a clean cut: the
// kernel's own threshold.
constexpr double kGradeCut = 1e-5;

constexpr double kPi = 3.14159265358979323846;

double
wrap_hue(double h)
{
  h = std::fmod(h, 360.0);
  if (h < 0) { h += 360; }
  return h >= 360 ? 0 : h;
}

// Degrees on round the wheel from one point to the next: all the way
// round from a point to itself.
double
gap(double from, double to)
{
  const double g = to - from;
  return g > 0 ? g : g + 360;
}

// A curve's points on the wheel, by hue, none alike (the later of two
// kept), each value and slope in range.
void
tidy(MixCurve& c)
{
  std::vector<MixPoint> in;
  for (MixPoint p : c.points) {
    if (!std::isfinite(p.hue) || !std::isfinite(p.value)) { continue; }
    p.hue = wrap_hue(p.hue);
    p.value = std::clamp(p.value, -1.0, 1.0);
    if (p.slope) {
      if (!std::isfinite(*p.slope)) {
        p.slope.reset();
      } else {
        *p.slope = std::clamp(*p.slope, -kMaxMixSlope, kMaxMixSlope);
      }
    }
    in.push_back(p);
  }
  std::stable_sort(in.begin(), in.end(),
                   [](const MixPoint& a, const MixPoint& b) {
                     return a.hue < b.hue;
                   });
  std::vector<MixPoint> out;
  for (const MixPoint& p : in) {
    if (!out.empty() && p.hue - out.back().hue < 1e-9) {
      out.back() = p;
    } else {
      out.push_back(p);
    }
  }
  c.points = std::move(out);
}

// A point as the curve runs through it: its hue, value and slope -- the
// one given, or the weighted harmonic mean of the slopes to the points
// either side, never steeper than three times the shallower, level where
// they turn (a spline key's, keyframes.cc).
struct Knot {
  double hue;
  double value;
  double slope;
};

std::vector<Knot>
knots(const MixCurve& c)
{
  // By hue on the wheel, as a curve read is kept (one made in code may
  // not be).
  std::vector<MixPoint> p = c.points;
  for (MixPoint& q : p) { q.hue = wrap_hue(q.hue); }
  std::ranges::stable_sort(p, {}, &MixPoint::hue);
  const std::size_t n = p.size();
  std::vector<Knot> k(n);
  for (std::size_t i = 0; i < n; ++i) {
    const MixPoint& prev = p[(i + n - 1) % n];
    const MixPoint& next = p[(i + 1) % n];
    double m = 0;
    if (p[i].slope) {
      m = *p[i].slope;
    } else {
      const double h0 = gap(prev.hue, p[i].hue);
      const double h1 = gap(p[i].hue, next.hue);
      const double d0 = (p[i].value - prev.value) / h0;
      const double d1 = (next.value - p[i].value) / h1;
      if (d0 * d1 > 0) {
        m = 3 * (h0 + h1) / ((2 * h1 + h0) / d0 + (h1 + 2 * h0) / d1);
      }
    }
    k[i] = {p[i].hue, p[i].value, m};
  }
  return k;
}

// The curve at hue `x` (on the wheel): the cubic Hermite of the span it
// lies in -- from the last point at or before it, round from the last
// to the first -- and its slope there.
double
eval(const std::vector<Knot>& k, double x, double* slope)
{
  if (slope) { *slope = 0; }
  if (k.empty()) { return 0; }
  x = wrap_hue(x);
  const std::size_t n = k.size();
  std::size_t i = n - 1;
  for (std::size_t j = 0; j < n; ++j) {
    if (k[j].hue > x) { break; }
    i = j;
  }
  const Knot& a = k[i];
  const Knot& b = k[(i + 1) % n];
  double from = a.hue;
  if (x < from) { from -= 360; }
  const double h = gap(a.hue, b.hue);
  const double t = std::clamp((x - from) / h, 0.0, 1.0);
  const double t2 = t * t, t3 = t2 * t;
  if (slope) {
    *slope = (6 * t2 - 6 * t) * (a.value - b.value) / h +
             (3 * t2 - 4 * t + 1) * a.slope + (3 * t2 - 2 * t) * b.slope;
  }
  return (2 * t3 - 3 * t2 + 1) * a.value + (t3 - 2 * t2 + t) * h * a.slope +
         (-2 * t3 + 3 * t2) * b.value + (t3 - t2) * h * b.slope;
}

double
decode_srgb(double c)
{
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

// Metal's smoothstep.
double
smooth(double from, double to, double x)
{
  const double t = std::clamp((x - from) / (to - from), 0.0, 1.0);
  return t * t * (3 - 2 * t);
}

// A change of range at `at`, `half` either side of it: 0 below, 1 above
// -- a clean cut where it has no width, as the kernel's.
double
ramp(double at, double half, double x)
{
  if (half <= kGradeCut) { return x < at ? 0 : 1; }
  return smooth(at - half, at + half, x);
}

// The grade's keys, read: a tint's hue and saturation, the ranges.
void
read_grade(const Json& j, ColorGrade& g)
{
  auto number = [&](const std::string& key) -> std::optional<double> {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) { return std::nullopt; }
    const double v = it->get<double>();
    if (!std::isfinite(v)) { return std::nullopt; }
    return v;
  };
  for (const GradeRangeKey& r : kGradeRangeKeys) {
    GradeTint& t = g.*r.tint;
    const std::string k = std::string("grade_") + r.name;
    if (auto h = number(k + "_hue")) { t.hue = wrap_hue(*h); }
    if (auto s = number(k + "_sat")) {
      t.saturation = std::clamp(*s, 0.0, 1.0);
    }
    // Untinted, it has no hue: one written is not kept.
    if (t.saturation == 0) { t.hue = 0; }
  }
  for (const GradeSlider& s : kGradeSliders) {
    if (auto v = number(s.key)) { g.*s.field = std::clamp(*v, s.lo, s.hi); }
  }
}

// The flat keys of the curves' points, read: "mix_hue_2_at" (its hue),
// "mix_hue_2" (its value), "mix_hue_2_slope", and "hue@30" (a point there
// of that value).
void
read_mix(const Json& j, ColorMix& mix)
{
  struct Part {
    std::optional<double> at, value, slope;
  };
  std::map<std::pair<int, long>, Part> parts;
  std::array<std::vector<MixPoint>, 3> loose;
  for (auto it = j.begin(); it != j.end(); ++it) {
    if (!it->is_number()) { continue; }
    const std::string& key = it.key();
    const double v = it->get<double>();
    for (int c = 0; c < 3; ++c) {
      const std::string name = kMixChannelKeys[c].name;
      if (key.size() > name.size() + 1 && key.starts_with(name + "@")) {
        const char* s = key.c_str() + name.size() + 1;
        char* end = nullptr;
        const double h = std::strtod(s, &end);
        if (end != s && *end == '\0') {
          loose[static_cast<std::size_t>(c)].push_back({h, v, {}});
        }
        continue;
      }
      const std::string prefix = "mix_" + name + "_";
      if (!key.starts_with(prefix)) { continue; }
      const char* s = key.c_str() + prefix.size();
      char* end = nullptr;
      const long n = std::strtol(s, &end, 10);
      if (end == s || n < 0) { continue; }
      Part& p = parts[{c, n}];
      const std::string rest = end;
      if (rest.empty()) {
        p.value = v;
      } else if (rest == "_at") {
        p.at = v;
      } else if (rest == "_slope") {
        p.slope = v;
      }
    }
  }
  for (int c = 0; c < 3; ++c) {
    MixCurve& curve = mix.*kMixChannelKeys[c].curve;
    curve.points.clear();
    for (const auto& [key, p] : parts) {
      if (key.first != c || !p.at) { continue; }
      curve.points.push_back({*p.at, p.value.value_or(0), p.slope});
    }
    for (const MixPoint& p : loose[static_cast<std::size_t>(c)]) {
      curve.points.push_back(p);
    }
    tidy(curve);
  }
}

}

double
MixCurve::at(double hue) const
{
  return eval(knots(*this), hue, nullptr);
}

double
MixCurve::slope_at(double hue) const
{
  double s = 0;
  eval(knots(*this), hue, &s);
  return s;
}

bool
MixCurve::identity() const
{
  return std::ranges::all_of(points, [](const MixPoint& p) {
    return p.value == 0 && p.slope.value_or(0) == 0;
  });
}

bool
ColorMix::identity() const
{
  return hue.identity() && saturation.identity() && luminance.identity();
}

ColorMix
zip_mix(const ColorMix& p, const ColorMix& a, const ColorMix& b,
        const ColorMix& n,
        const std::function<double(double, double, double, double)>& f)
{
  ColorMix o;
  for (const MixChannelKey& ch : kMixChannelKeys) {
    const MixCurve* c[4] = {&(p.*ch.curve), &(a.*ch.curve), &(b.*ch.curve),
                            &(n.*ch.curve)};
    MixCurve& out = o.*ch.curve;
    // The same curve four times: as it is, its slopes its own.
    if (*c[0] == *c[1] && *c[1] == *c[2] && *c[2] == *c[3]) {
      out = *c[1];
      continue;
    }
    std::vector<double> hues;
    std::array<std::vector<Knot>, 4> k;
    for (int i = 0; i < 4; ++i) {
      for (const MixPoint& pt : c[i]->points) { hues.push_back(pt.hue); }
      k[static_cast<std::size_t>(i)] = knots(*c[i]);
    }
    std::ranges::sort(hues);
    hues.erase(std::unique(hues.begin(), hues.end(),
                           [](double x, double y) { return y - x < 1e-9; }),
               hues.end());
    for (const double h : hues) {
      std::array<double, 4> v{}, s{};
      for (std::size_t i = 0; i < 4; ++i) { v[i] = eval(k[i], h, &s[i]); }
      out.points.push_back({h, f(v[0], v[1], v[2], v[3]),
                            f(s[0], s[1], s[2], s[3])});
    }
    tidy(out);
  }
  return o;
}

std::optional<MixReading>
mix_hue_of(double r, double g, double b)
{
  r = std::max(r, 0.0);
  g = std::max(g, 0.0);
  b = std::max(b, 0.0);
  const double mx = std::max({r, g, b});
  const double chroma = mx - std::min({r, g, b});
  if (mx <= 1e-6 || chroma <= 1e-5) { return std::nullopt; }
  double h = 0;
  if (mx == r) {
    h = 60 * std::fmod((g - b) / chroma + 6, 6);
  } else if (mx == g) {
    h = 60 * ((b - r) / chroma + 2);
  } else {
    h = 60 * ((r - g) / chroma + 4);
  }
  return MixReading{h, chroma / mx};
}

std::vector<double>
mix_samples(const MixCurve& c)
{
  const auto k = knots(c);
  std::vector<double> s(kMixSamples + 1);
  for (int i = 0; i < kMixSamples; ++i) {
    s[static_cast<std::size_t>(i)] =
        std::clamp(eval(k, i * 360.0 / kMixSamples, nullptr), -1.0, 1.0);
  }
  s[kMixSamples] = s[0];
  return s;
}

// The kernel. Core Image hands it premultiplied extended linear sRGB; it
// reads the colour in sRGB's encoding (as mix_hue_of does), finds the
// curves at its hue in the table (between the two whole degrees either
// side) and turns its hue there -- its saturation and value kept --
// adding the change to what it had, so a channel out of sRGB's gamut (a
// P3 picture's, below 0) keeps what the mixer did not change; then, in
// linear light, its saturation about its own luminance (a blue made grey
// is the dark grey of its luminance, not white) and that luminance
// scaled.
const char*
color_mix_kernel_source()
{
  // The kernel's 30 degrees, 1.5 stops and 360 samples are these.
  static_assert(kMixHueDegrees == 30 && kMixLuminanceStops == 1.5 &&
                kMixSamples == 360);
  return R"(
#include <CoreImage/CoreImage.h>
using namespace metal;

static float encode_srgb(float c) {
  float a = abs(c);
  float v = a <= 0.0031308 ? a * 12.92 : 1.055 * pow(a, 1.0 / 2.4) - 0.055;
  return copysign(v, c);
}

static float decode_srgb(float c) {
  float a = abs(c);
  float v = a <= 0.04045 ? a / 12.92 : pow((a + 0.055) / 1.055, 2.4);
  return copysign(v, c);
}

[[stitchable]] float4 valtz_color_mix(coreimage::sampler image,
                                      coreimage::sampler table) {
  float4 s = image.sample(image.coord());
  float alpha = s.a;
  if (alpha <= 0.0) { return s; }
  float3 lin = s.rgb / alpha;
  float3 g = float3(encode_srgb(lin.r), encode_srgb(lin.g),
                    encode_srgb(lin.b));
  float3 c = max(g, float3(0.0));
  float mx = max(c.r, max(c.g, c.b));
  float chroma = mx - min(c.r, min(c.g, c.b));
  if (mx <= 1e-6 || chroma <= 1e-5) { return s; }
  float h;
  if (mx == c.r) {
    h = 60.0 * fmod((c.g - c.b) / chroma + 6.0, 6.0);
  } else if (mx == c.g) {
    h = 60.0 * ((c.b - c.r) / chroma + 2.0);
  } else {
    h = 60.0 * ((c.r - c.g) / chroma + 4.0);
  }
  float x = clamp(h, 0.0, 360.0);
  float i0 = min(floor(x), 359.0);
  float3 lo = table.sample(table.transform(float2(i0 + 0.5, 0.5))).rgb;
  float3 hi = table.sample(table.transform(float2(i0 + 1.5, 0.5))).rgb;
  float3 d = mix(lo, hi, x - i0) * 2.0 - 1.0;
  float saturation = chroma / mx;
  // Near grey a hue is barely there: its move fades out.
  float fade = smoothstep(0.0, 0.15, saturation);
  float h2 = fmod(h + d.r * 30.0 * fade + 360.0, 360.0);
  float3 k = fmod(float3(5.0, 3.0, 1.0) + h2 / 60.0, 6.0);
  float3 turned = mx - mx * saturation * clamp(min(k, 4.0 - k), 0.0, 1.0);
  float3 g2 = g + (turned - c);
  float3 out = float3(decode_srgb(g2.r), decode_srgb(g2.g),
                      decode_srgb(g2.b));
  float y = dot(out, float3(0.2126, 0.7152, 0.0722));
  out = y + (out - y) * max(0.0, 1.0 + d.g);
  out *= exp2(1.5 * d.b * fade);
  return float4(out * alpha, alpha);
}
)";
}

bool
ColorGrade::identity() const
{
  return shadows.saturation == 0 && midtones.saturation == 0 &&
         highlights.saturation == 0;
}

std::array<double, 4>
grade_tones(const ColorGrade& g)
{
  const double c =
      0.5 + kGradeBalanceReach * std::clamp(g.balance, -1.0, 1.0);
  const double w = std::clamp(g.width, 0.0, 1.0);
  const double b = std::clamp(g.blend, 0.0, 1.0);
  double s = c * (1 - w);
  double u = c + (1 - c) * w;
  // Each change kept inside the tones: black wholly a shadow, white
  // wholly a highlight.
  double hs = b * std::min(s, 1 - s);
  double hu = b * std::min(u, 1 - u);
  // As wide as the tones, the midtones leave no shadows (black is one of
  // them) and no highlights.
  if (s <= 1e-9) {
    s = -1;
    hs = 0;
  }
  if (u >= 1 - 1e-9) {
    u = 2;
    hu = 0;
  }
  return {s, hs, u, hu};
}

GradeWeights
grade_weights(const ColorGrade& g, double tone)
{
  const auto k = grade_tones(g);
  const double t = std::clamp(tone, 0.0, 1.0);
  double s = 1 - ramp(k[0], k[1], t);
  double h = ramp(k[2], k[3], t);
  // Overlapping, the shadows and highlights share the tone.
  const double sum = s + h;
  if (sum > 1) {
    s /= sum;
    h /= sum;
  }
  return {s, std::max(0.0, 1 - s - h), h};
}

std::array<double, 3>
grade_gain(const GradeTint& t)
{
  // The hue at full saturation (HSV's, in sRGB's encoding, as the
  // mixer's kernel turns one), in linear light.
  const double h = wrap_hue(t.hue) / 60;
  std::array<double, 3> c{};
  const double n[3] = {5, 3, 1};
  for (std::size_t i = 0; i < 3; ++i) {
    const double k = std::fmod(n[i] + h, 6.0);
    c[i] = decode_srgb(1 - std::clamp(std::min(k, 4 - k), 0.0, 1.0));
  }
  const double y = 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
  const double s = std::clamp(t.saturation, 0.0, 1.0) * kGradeStrength;
  std::array<double, 3> g{};
  for (std::size_t i = 0; i < 3; ++i) {
    g[i] = std::max(0.0, 1 + s * (c[i] - y));
  }
  return g;
}

ColorGrade
zip_grade(const ColorGrade& p, const ColorGrade& a, const ColorGrade& b,
          const ColorGrade& n,
          const std::function<double(double, double, double, double)>& f)
{
  ColorGrade o;
  const ColorGrade* g[4] = {&p, &a, &b, &n};
  for (const GradeRangeKey& r : kGradeRangeKeys) {
    GradeTint& out = o.*r.tint;
    const GradeTint& at = a.*r.tint;
    // The same tint four times: as it is, its hue to the degree.
    if (p.*r.tint == at && b.*r.tint == at && n.*r.tint == at) {
      out = at;
      continue;
    }
    std::array<double, 4> x{}, y{};
    for (std::size_t i = 0; i < 4; ++i) {
      const GradeTint& t = g[i]->*r.tint;
      const double rad = t.hue * kPi / 180;
      x[i] = t.saturation * std::cos(rad);
      y[i] = t.saturation * std::sin(rad);
    }
    const double ox = f(x[0], x[1], x[2], x[3]);
    const double oy = f(y[0], y[1], y[2], y[3]);
    // At the centre, no hue (as one read).
    const double sat = std::clamp(std::hypot(ox, oy), 0.0, 1.0);
    if (sat > 1e-12) {
      out = {wrap_hue(std::atan2(oy, ox) * 180 / kPi), sat};
    }
  }
  for (const GradeSlider& s : kGradeSliders) {
    o.*s.field =
        std::clamp(f(p.*s.field, a.*s.field, b.*s.field, n.*s.field), s.lo,
                   s.hi);
  }
  return o;
}

// The grade's kernel. Core Image hands it premultiplied extended linear
// sRGB; it reads the pixel's tone -- its luminance in sRGB's encoding --
// weighs it in the three ranges as grade_weights does, and multiplies it
// by their gains so weighed.
const char*
color_grade_kernel_source()
{
  static_assert(kGradeCut == 1e-5);
  return R"(
#include <CoreImage/CoreImage.h>
using namespace metal;

static float valtz_grade_ramp(float at, float reach, float x) {
  if (reach <= 1e-5) { return x < at ? 0.0 : 1.0; }
  return smoothstep(at - reach, at + reach, x);
}

[[stitchable]] float4 valtz_color_grade(coreimage::sample_t s,
                                        float3 shadows, float3 midtones,
                                        float3 highlights, float4 tones) {
  float alpha = s.a;
  if (alpha <= 0.0) { return s; }
  float3 lin = s.rgb / alpha;
  float y = clamp(dot(lin, float3(0.2126, 0.7152, 0.0722)), 0.0, 1.0);
  float t = y <= 0.0031308 ? y * 12.92 : 1.055 * pow(y, 1.0 / 2.4) - 0.055;
  float sh = 1.0 - valtz_grade_ramp(tones.x, tones.y, t);
  float hi = valtz_grade_ramp(tones.z, tones.w, t);
  float sum = sh + hi;
  if (sum > 1.0) {
    sh /= sum;
    hi /= sum;
  }
  float mid = max(0.0, 1.0 - sh - hi);
  float3 gain = sh * shadows + mid * midtones + hi * highlights;
  return float4(s.rgb * gain, alpha);
}
)";
}

bool
Adjustments::identity() const
{
  return *this == Adjustments{};
}

Json
to_json(const Adjustments& a)
{
  Json j = Json::object();
  for (const Slider& s : kSliders) {
    if (a.*s.field != 0) { j[s.key] = a.*s.field; }
  }
  for (const MixChannelKey& ch : kMixChannelKeys) {
    const auto& points = (a.mix.*ch.curve).points;
    for (std::size_t i = 0; i < points.size(); ++i) {
      const std::string k =
          std::string("mix_") + ch.name + "_" + std::to_string(i);
      j[k + "_at"] = points[i].hue;
      j[k] = points[i].value;
      if (points[i].slope) { j[k + "_slope"] = *points[i].slope; }
    }
  }
  for (const GradeRangeKey& r : kGradeRangeKeys) {
    const GradeTint& t = a.grade.*r.tint;
    if (t.saturation == 0) { continue; }
    const std::string k = std::string("grade_") + r.name;
    j[k + "_hue"] = t.hue;
    j[k + "_sat"] = t.saturation;
  }
  for (const GradeSlider& s : kGradeSliders) {
    if (a.grade.*s.field != s.rest) { j[s.key] = a.grade.*s.field; }
  }
  if (a.alpha != 1) { j["alpha"] = a.alpha; }
  return j;
}

Adjustments
adjustments_from_json(const Json& j)
{
  Adjustments a;
  if (!j.is_object()) { return a; }
  for (const Slider& s : kSliders) {
    auto it = j.find(s.key);
    if (it != j.end() && it->is_number()) {
      a.*s.field = std::clamp(it->get<double>(), s.lo, s.hi);
    }
  }
  read_mix(j, a.mix);
  read_grade(j, a.grade);
  if (auto it = j.find("alpha");
      it != j.end() && it->is_number() && std::isfinite(it->get<double>())) {
    a.alpha = std::clamp(it->get<double>(), 0.0, 1.0);
  }
  return a;
}

std::vector<FilterStep>
filter_chain(const Adjustments& a)
{
  std::vector<FilterStep> chain;
  if (a.temperature != 0 || a.tint != 0) {
    // Declaring the source white warmer than D65 (a higher temperature)
    // makes the picture warmer; tint moves along green-magenta.
    // Directions checked by measurement.
    chain.push_back({"CITemperatureAndTint",
                     {{"inputNeutral", {6500 + a.temperature * 2500,
                                        a.tint * 60}},
                      {"inputTargetNeutral", {6500, 0}}}});
  }
  if (a.exposure != 0) {
    chain.push_back({"CIExposureAdjust", {{"inputEV", {a.exposure}}}});
  }
  if (a.contrast != 0 || a.highlights != 0 || a.shadows != 0) {
    // One tone curve: contrast steepens (or flattens) it around mid-grey;
    // highlights and shadows lift or lower its ends.
    const double c = a.contrast * 0.12;
    chain.push_back({"CIToneCurve",
                     {{"inputPoint0", {0, 0}},
                      {"inputPoint1", {0.25, 0.25 - c + a.shadows * 0.12}},
                      {"inputPoint2", {0.5, 0.5}},
                      {"inputPoint3", {0.75, 0.75 + c + a.highlights * 0.12}},
                      {"inputPoint4", {1, 1}}}});
  }
  if (a.vibrance != 0) {
    chain.push_back({"CIVibrance", {{"inputAmount", {a.vibrance}}}});
  }
  if (a.saturation != 0) {
    chain.push_back({"CIColorControls",
                     {{"inputSaturation", {1 + a.saturation}},
                      {"inputBrightness", {0}},
                      {"inputContrast", {1}}}});
  }
  if (!a.mix.identity()) {
    chain.push_back({kColorMixFilter,
                     {{"inputHue", mix_samples(a.mix.hue)},
                      {"inputSaturation", mix_samples(a.mix.saturation)},
                      {"inputLuminance", mix_samples(a.mix.luminance)}}});
  }
  if (!a.grade.identity()) {
    auto gain = [](const GradeTint& t) {
      const auto g = grade_gain(t);
      return std::vector<double>(g.begin(), g.end());
    };
    const auto tones = grade_tones(a.grade);
    chain.push_back({kColorGradeFilter,
                     {{"inputShadows", gain(a.grade.shadows)},
                      {"inputMidtones", gain(a.grade.midtones)},
                      {"inputHighlights", gain(a.grade.highlights)},
                      {"inputTones", {tones.begin(), tones.end()}}}});
  }
  if (a.alpha != 1) {
    // Last: every pixel's coverage scaled, its colour kept.
    chain.push_back({"CIColorMatrix",
                     {{"inputAVector",
                       {0, 0, 0, std::clamp(a.alpha, 0.0, 1.0)}}}});
  }
  return chain;
}

Json
to_json(const std::vector<FilterStep>& chain)
{
  Json out = Json::array();
  for (const FilterStep& f : chain) {
    Json params = Json::object();
    for (const auto& [key, v] : f.params) {
      if (v.size() == 1) {
        params[key] = v[0];
      } else {
        params[key] = v;
      }
    }
    out.push_back({{"filter", f.filter}, {"params", std::move(params)}});
  }
  return out;
}

}
