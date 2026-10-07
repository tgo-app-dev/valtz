#include "valtz/media/adjust.h"

#include <algorithm>

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
