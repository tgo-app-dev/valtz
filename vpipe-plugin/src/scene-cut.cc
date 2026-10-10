#include "scene-cut.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace valtz::plugin {

FrameSignature
signature(const std::uint8_t* rgb, int h, int w)
{
  FrameSignature s;
  if (rgb == nullptr || h <= 0 || w <= 0) {
    return s;
  }
  const std::size_t plane = static_cast<std::size_t>(h) * w;
  const std::uint8_t* r = rgb;
  const std::uint8_t* g = rgb + plane;
  const std::uint8_t* b = rgb + 2 * plane;
  constexpr int GW = FrameSignature::kGridW;
  constexpr int GH = FrameSignature::kGridH;
  std::array<double, GW * GH> sum{};
  std::array<std::size_t, GW * GH> count{};
  std::array<std::size_t, 64> bins{};
  // Every other pixel each way: a quarter of them describe a frame as
  // well, at a quarter of the cost.
  std::size_t n = 0;
  for (int y = 0; y < h; y += 2) {
    const int gy = std::min(GH - 1, y * GH / h);
    for (int x = 0; x < w; x += 2) {
      const std::size_t i = static_cast<std::size_t>(y) * w + x;
      const int rr = r[i], gg = g[i], bb = b[i];
      ++bins[static_cast<std::size_t>((rr >> 6) * 16 + (gg >> 6) * 4 +
                                      (bb >> 6))];
      const int cell = gy * GW + std::min(GW - 1, x * GW / w);
      // Rec. 709's weights: brightness as the eye ranks it.
      sum[cell] += 0.2126 * rr + 0.7152 * gg + 0.0722 * bb;
      ++count[cell];
      ++n;
    }
  }
  for (std::size_t k = 0; k < bins.size(); ++k) {
    s.hist[k] = n > 0 ? static_cast<float>(bins[k]) / n : 0.0f;
  }
  for (std::size_t c = 0; c < sum.size(); ++c) {
    s.luma[c] = count[c] > 0
        ? static_cast<float>(sum[c] / count[c] / 255.0) : 0.0f;
  }
  return s;
}

double
distance(const FrameSignature& a, const FrameSignature& b)
{
  double hist = 0;
  for (std::size_t k = 0; k < a.hist.size(); ++k) {
    hist += std::abs(a.hist[k] - b.hist[k]);
  }
  double grid = 0;
  for (std::size_t c = 0; c < a.luma.size(); ++c) {
    grid += std::abs(a.luma[c] - b.luma[c]);
  }
  grid /= static_cast<double>(a.luma.size());
  return 0.5 * (0.5 * hist + grid);
}

bool
SceneCutter::next(const FrameSignature& sig)
{
  if (!_have) {
    _have = true;
    _prev = sig;
    _last = 0;
    return false;
  }
  const double d = distance(_prev, sig);
  _last = d;
  _prev = sig;
  bool cut = d >= _threshold;
  if (cut && _seen.size() >= 3) {
    std::vector<double> v = _seen;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    cut = d > 3.0 * v[v.size() / 2];
  }
  if (cut) {
    _seen.clear();
  } else {
    _seen.push_back(d);
  }
  return cut;
}

void
SceneCutter::restart()
{
  _seen.clear();
}

}
