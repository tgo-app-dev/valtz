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
// Every value is 0 at rest; exposure is in stops (-2...2), the rest run
// -1...1. The chain runs in extended linear sRGB, in this order: white
// balance, exposure, tone (contrast, highlights, shadows), colour
// (vibrance, saturation).

#ifndef VALTZ_MEDIA_ADJUST_H
#define VALTZ_MEDIA_ADJUST_H

#include "valtz/base/json.h"

#include <string>
#include <vector>

namespace valtz::media {

struct Adjustments {
  double exposure = 0;     // stops
  double contrast = 0;
  double highlights = 0;
  double shadows = 0;
  double vibrance = 0;
  double saturation = 0;
  double temperature = 0;  // + warmer
  double tint = 0;         // + magenta, - green

  bool identity() const;
  bool operator==(const Adjustments&) const = default;
};

// {"exposure": 0.4, ...}: only the keys that are not 0. Reading is
// tolerant -- unknown keys are ignored, missing ones are 0 -- and clamps
// each value to its slider's range.
Json to_json(const Adjustments&);
Adjustments adjustments_from_json(const Json&);

// One Core Image filter of the chain. A parameter is a number (one
// value) or a CIVector (two to four).
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
