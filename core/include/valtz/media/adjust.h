// Image adjustments -- the Adjust panel's sliders -- as DATA.
//
// The same adjustments are rendered in two places: live in the app (the
// result on the stage, or the picture about to be edited) and in the
// engine (that picture as the model receives it, on top of its RAW
// development, before it goes into vpipe as F16). Two hand-written Core
// Image chains would drift, and the model would be handed something
// other than what the stage showed. So the chain is defined HERE, once,
// as a list of Core Image filter names and parameters; the app builds
// its filters from that list (through the bridge) and the engine from
// the same list (model-input.mm).
//
// Every value is 0 at rest but the ALPHA (1: as it is, 0 clear -- the
// layer's master opacity, the panel's fader at its left); exposure is in
// stops (-2...2), the rest run -1...1. The chain runs in extended linear
// sRGB, in this order: white
// balance, exposure, tone (contrast, highlights, shadows), colour
// (vibrance, saturation), then the COLOR MIXER (ColorMix below) and the
// COLOR GRADE (ColorGrade) -- steps no stock filter makes: Core Image
// kernels of Valtz's own, their source here (color_mix_kernel_source,
// color_grade_kernel_source), compiled at run time by the engine and by
// the app alike, so both draw them with the same maths -- and last the
// alpha, every pixel's coverage scaled (a CIColorMatrix's alpha vector,
// as a transition fades a layer), so a layer under it shows through.

#ifndef VALTZ_MEDIA_ADJUST_H
#define VALTZ_MEDIA_ADJUST_H

#include "valtz/base/json.h"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace valtz::media {

// THE COLOR MIXER (DESIGN §10a Color): three CURVES over the colour
// wheel -- a hue SHIFT (kMixHueDegrees at -1 / +1, + toward the hues
// after it), a SATURATION (-1 the grey of its own luminance, +1 twice as
// far from it) and a LUMINANCE (kMixLuminanceStops darker or lighter, in
// linear light) -- each a value -1...1 at every hue, 0 at rest. A curve
// is its control POINTS, a hue and a value each, joined round the wheel
// (the last to the first) by a cubic through them: smooth, and never
// past the points either side (Fritsch and Butland's slopes, as a spline
// key's in keyframes.h). No point: 0 everywhere -- the panel's line down
// the middle of its plane; one point: its value everywhere. A colour is
// moved by each curve's value at its own hue, read in sRGB's encoding
// -- where hue and saturation read as a person sees them -- and the move
// of its hue and luminance fades out as it goes grey: a grey has no hue
// to move, and stays grey.
inline constexpr double kMixHueDegrees = 30;
inline constexpr double kMixLuminanceStops = 1.5;

// The seven hues a person names, their centres on the wheel: the marks
// along the panel's hue axis.
inline constexpr int kMixHues = 7;
struct MixHue {
  const char* key;     // "red"
  double      degrees;
};
inline constexpr std::array<MixHue, kMixHues> kMixHueBands = {{
    {"red", 0},    {"orange", 30}, {"yellow", 60}, {"green", 120},
    {"aqua", 180}, {"blue", 240},  {"magenta", 300},
}};

struct MixPoint {
  MixPoint() = default;
  MixPoint(double h, double v, std::optional<double> s = std::nullopt)
      : hue(h), value(v), slope(s) {}

  double hue = 0;     // degrees, 0 <= hue < 360
  double value = 0;   // -1...1
  // Its slope (value a degree) when it is given: a curve laid on another
  // curve's points between two keys (zip_mix) keeps the shape it had.
  // Otherwise -- every point a person places -- the curve's own, from the
  // points either side.
  std::optional<double> slope;

  bool operator==(const MixPoint&) const = default;
};

struct MixCurve {
  std::vector<MixPoint> points;   // by hue, ascending, none alike

  double at(double hue) const;         // its value there
  double slope_at(double hue) const;   // and its slope, value a degree
  bool identity() const;               // 0 at every hue
  bool operator==(const MixCurve&) const = default;
};

struct ColorMix {
  MixCurve hue;
  MixCurve saturation;
  MixCurve luminance;

  // Nothing moves (its points may be there, every one at 0).
  bool identity() const;
  bool operator==(const ColorMix&) const = default;
};

// Four mixers made one, number by number -- a key's lerp or spline:
// each curve laid on the points of all four, where each has its own value
// and slope (exactly: a cubic cut where it is laid is the same cubic), and
// `f` making each value and each slope of the result from the four's. So
// between two keys of other points the look is the two curves' own,
// eased hue by hue.
ColorMix zip_mix(const ColorMix& p, const ColorMix& a, const ColorMix& b,
                 const ColorMix& n,
                 const std::function<double(double, double, double,
                                            double)>& f);

// A colour's hue (sRGB as encoded, straight, 0...1), degrees 0...360, and
// its saturation (its chroma of its largest channel), as the kernel reads
// them: the target tool's colour pressed on. Nothing for a grey.
struct MixReading {
  double hue = 0;
  double saturation = 0;
};
std::optional<MixReading> mix_hue_of(double r, double g, double b);

// What the kernel reads of a curve: its value at each whole degree, 0 to
// 360 (kMixSamples + 1: the last the first again), each clamped to -1...1.
inline constexpr int kMixSamples = 360;
std::vector<double> mix_samples(const MixCurve&);

// The mixer's step in the chain (filter_chain): kColorMixFilter, its
// params "inputHue", "inputSaturation", "inputLuminance" -- each a curve's
// samples. Drawn with the kernel below: a Core Image kernel in Metal,
// `valtz_color_mix(sampler image, sampler table)`, the table a
// (kMixSamples + 1) x 1 picture, not colour managed, of the three curves'
// samples as (value + 1) / 2 in its red, green and blue -- compiled with
// CIKernel's kernelsWithMetalString: (no offline Metal compiler is
// needed), applied with the table's whole extent its region of interest.
inline constexpr const char* kColorMixFilter = "ValtzColorMix";
inline constexpr const char* kColorMixKernel = "valtz_color_mix";
const char* color_mix_kernel_source();

// THE COLOR GRADE (DESIGN §10a Grade): a TINT for each of the picture's
// three tone ranges -- shadows, midtones, highlights -- a hue on the
// colour wheel and a saturation, 0 (none) to 1, how far the range is
// pushed toward that hue; and where the ranges lie. A pixel's TONE is
// its luminance in sRGB's encoding (0...1, as a person sees it). The
// midtones are centred where BALANCE puts them -- 0.5, moved up to
// kGradeBalanceReach toward black (-1) or white (+1); their WIDTH is the
// share of the way from that centre to black, and to white, where they
// are at half strength at a blend of 0 -- where the shadows and the
// highlights take over; BLEND is how gradual each change is: 0 a clean
// cut there, 1 the change spread as far as the ranges reach -- where it
// spreads past the centre the shadows and highlights overlap, and share
// what the midtones leave. A tint is a GAIN of each channel in linear
// light of luminance 1, so a grey keeps its brightness and black stays
// black: 1 + saturation x kGradeStrength x (the hue's colour, at full
// saturation, less its luminance). A pixel takes the three ranges'
// gains by its weights in them.
inline constexpr double kGradeBalanceReach = 0.4;
inline constexpr double kGradeStrength = 1.0;

struct GradeTint {
  double hue = 0;          // degrees, 0 <= hue < 360
  double saturation = 0;   // 0...1

  bool operator==(const GradeTint&) const = default;
};

struct ColorGrade {
  GradeTint shadows;
  GradeTint midtones;
  GradeTint highlights;
  double balance = 0;   // -1...1
  double width = 0.5;   // 0...1
  double blend = 0.5;   // 0...1

  // Nothing tinted (the ranges may be set).
  bool identity() const;
  bool operator==(const ColorGrade&) const = default;
};

// A tone's (0...1) weights in the three ranges, which sum to 1 -- as the
// kernel weighs a pixel.
struct GradeWeights {
  double shadows = 0;
  double midtones = 0;
  double highlights = 0;
};
GradeWeights grade_weights(const ColorGrade&, double tone);

// What the kernel is handed: a tint's gain, red, green, blue; and the
// ranges' TONES -- where the shadows meet the midtones and half the
// width of that change, where the midtones meet the highlights and half
// its width (a range none of the tones reach set beyond them).
std::array<double, 3> grade_gain(const GradeTint&);
std::array<double, 4> grade_tones(const ColorGrade&);

// Four grades made one, number by number -- a key's lerp or spline: each
// tint as its point on the wheel's disk (so a hue eases the short way
// round, through grey where two are opposite), the ranges as numbers.
ColorGrade zip_grade(const ColorGrade& p, const ColorGrade& a,
                     const ColorGrade& b, const ColorGrade& n,
                     const std::function<double(double, double, double,
                                                double)>& f);

// The grade's step in the chain (filter_chain), after the mixer's:
// kColorGradeFilter, its params "inputShadows", "inputMidtones",
// "inputHighlights" (gains) and "inputTones". Drawn with the kernel
// below: a Core Image colour kernel in Metal,
// `valtz_color_grade(sample_t s, float3 shadows, float3 midtones,
// float3 highlights, float4 tones)`, compiled as the mixer's is.
inline constexpr const char* kColorGradeFilter = "ValtzColorGrade";
inline constexpr const char* kColorGradeKernel = "valtz_color_grade";
const char* color_grade_kernel_source();

struct Adjustments {
  double exposure = 0;     // stops
  double contrast = 0;
  double highlights = 0;
  double shadows = 0;
  double vibrance = 0;
  double saturation = 0;
  double temperature = 0;  // + warmer
  double tint = 0;         // + magenta, - green
  double alpha = 1;        // 0 clear ... 1 as it is
  // The Color panel's and the Grade panel's: keyed with the rest, one
  // look (the layer's "adjust" modifier).
  ColorMix mix;
  ColorGrade grade;

  bool identity() const;
  bool operator==(const Adjustments&) const = default;
};

// {"exposure": 0.4, "mix_hue_0_at": 30, "mix_hue_0": -0.2, ...}: only
// the sliders that are not at rest ("alpha" while not 1, held to 0...1),
// and every point of the mixer's curves --
// flat numbers beside the rest, so a key holds them as it holds a slider:
// "mix_<hue|sat|lum>_<n>_at" its hue, "mix_..._<n>" its value,
// "mix_..._<n>_slope" a slope given, n from 0 in the order of their hues.
// Reading is tolerant -- unknown keys are ignored, missing ones are 0 --
// clamps each value to its slider's range and takes "<hue|sat|lum>@<deg>"
// too, a point at that hue (`valtzctl adjust ... hue@30=0.5`). The
// grade's: "grade_<shadows|midtones|highlights>_hue" and "..._sat" (a
// tint, written while its saturation is not 0), "grade_balance",
// "grade_width", "grade_blend" (written when not at rest: 0, 0.5, 0.5).
Json to_json(const Adjustments&);
Adjustments adjustments_from_json(const Json&);

// One Core Image filter of the chain. A parameter is a number (one
// value) or a CIVector (two to four) -- the mixer's, its curves' samples.
struct FilterStep {
  std::string filter;  // "CIExposureAdjust"
  std::vector<std::pair<std::string, std::vector<double>>> params;
};

// The chain for `a`, in order; empty for the identity.
std::vector<FilterStep> filter_chain(const Adjustments& a);

// [{"filter": "CIToneCurve", "params": {"inputPoint1": [0.25, 0.22]}}]
Json to_json(const std::vector<FilterStep>&);

}

#endif
