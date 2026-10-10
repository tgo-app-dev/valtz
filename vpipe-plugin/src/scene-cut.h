// Where a clip CUTS from one scene to the next, told from its frames as
// the summary stage gets them: sparse (one a second, or one every two)
// and small (576 x 320 at most).
//
// A frame is reduced to a SIGNATURE -- the share of its pixels in each of
// 64 colour bins (4 levels of R, G and B), and its brightness on a 16 x 9
// grid -- and two frames' DISTANCE is the mean of the two differences,
// each 0..1: half the histograms' L1 distance, and the grids' mean
// absolute difference. Colour alone misses a cut between two shots of one
// place; the grid alone calls a pan a cut.
//
// Frames a second apart differ within a shot too -- people move, the
// camera pans -- so a fixed threshold either misses quiet cuts or cuts
// busy shots. A cut is a distance over the threshold AND well over what
// the scene has been: three times the median of its own distances once
// it has three. A scene is what lies between two cuts.

#ifndef VALTZ_VPIPE_PLUGIN_SCENE_CUT_H
#define VALTZ_VPIPE_PLUGIN_SCENE_CUT_H

#include <array>
#include <cstdint>
#include <vector>

namespace valtz::plugin {

struct FrameSignature {
  static constexpr int kGridW = 16;
  static constexpr int kGridH = 9;
  std::array<float, 64>              hist{};   // shares, summing to 1
  std::array<float, kGridW * kGridH> luma{};   // 0..1
};

// A planar 8-bit RGB frame [3, h, w].
FrameSignature signature(const std::uint8_t* rgb, int h, int w);

// 0 (the same) .. 1.
double distance(const FrameSignature& a, const FrameSignature& b);

class SceneCutter {
public:
  // Distances at or over `threshold` may cut. 0.1: two shots of one
  // woman in one room measured 0.12 apart, frames of one shot 0.003-0.02
  // (generated clips, a second apart).
  explicit SceneCutter(double threshold = 0.1) : _threshold(threshold) {}

  // `sig` is the next frame's: true when it begins a new scene (the
  // frame before it ended one). The first frame begins the first scene
  // and is not a cut.
  bool next(const FrameSignature& sig);

  // The scene is over without a cut (it ran too long): the next frame
  // continues from this one, measured afresh.
  void restart();

  // The distance `next` measured last (0 for a first frame).
  double last_distance() const noexcept { return _last; }

private:
  double              _threshold;
  bool                _have = false;
  FrameSignature      _prev;
  std::vector<double> _seen;   // the scene's distances so far
  double              _last = 0;
};

}

#endif
