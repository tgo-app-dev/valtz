#include "valtz/media/space.h"

#include "valtz/media/keyframes.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace valtz::media {

namespace {

constexpr double kPi = std::numbers::pi;

// A one-pole low-pass's step toward its input, for a corner `hz`.
double
one_pole(double hz, double rate)
{
  return 1 - std::exp(-2 * kPi * std::clamp(hz, 1.0, 0.45 * rate) / rate);
}

// A second-order Butterworth low- or high-pass (the cookbook's biquad),
// run on one signal.
struct Biquad {
  double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
  double x1 = 0, x2 = 0, y1 = 0, y2 = 0;

  Biquad(double hz, double rate, bool high)
  {
    const double w = 2 * kPi * hz / rate;
    const double c = std::cos(w);
    // Q = 1/sqrt2.
    const double alpha = std::sin(w) / std::numbers::sqrt2;
    const double a0 = 1 + alpha;
    const double edge = high ? (1 + c) / 2 : (1 - c) / 2;
    b0 = edge / a0;
    b1 = (high ? -2 * edge : 2 * edge) / a0;
    b2 = edge / a0;
    a1 = -2 * c / a0;
    a2 = (1 - alpha) / a0;
  }

  double
  step(double x)
  {
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return y;
  }
};

}

void
pan_sound(std::span<float> left, std::span<float> right,
          std::span<const float> pan)
{
  const std::size_t n = std::min({left.size(), right.size(), pan.size()});
  for (std::size_t i = 0; i < n; ++i) {
    // Centred: as it is, exactly.
    if (pan[i] == 0) {
      continue;
    }
    const double turn = (std::clamp(double(pan[i]), -1.0, 1.0) + 1) * kPi / 4;
    left[i] = static_cast<float>(left[i] * std::numbers::sqrt2 *
                                 std::cos(turn));
    right[i] = static_cast<float>(right[i] * std::numbers::sqrt2 *
                                  std::sin(turn));
  }
}

void
widen_sound(std::span<float> left, std::span<float> right,
            std::span<const float> width)
{
  const std::size_t n = std::min({left.size(), right.size(), width.size()});
  for (std::size_t i = 0; i < n; ++i) {
    const double w = std::clamp(double(width[i]), 0.0, kMaxWidth);
    const double mid = 0.5 * (left[i] + right[i]);
    const double side = 0.5 * (left[i] - right[i]) * w;
    left[i] = static_cast<float>(mid + side);
    right[i] = static_cast<float>(mid - side);
  }
}

void
place_sound_far(std::span<float> left, std::span<float> right,
                std::span<const float> metres, int rate)
{
  const std::size_t n = std::min({left.size(), right.size(), metres.size()});
  const double fs = std::max(1, rate);
  double yl = 0, yr = 0;
  bool started = false;
  for (std::size_t i = 0; i < n; ++i) {
    const double d = std::clamp(double(metres[i]), 1.0, kMaxDistance);
    // As recorded at a metre: nothing done, exactly (the air's filter
    // following).
    if (d <= 1 || !started) {
      yl = left[i];
      yr = right[i];
      started = true;
      if (d <= 1) {
        continue;
      }
    }
    const double a = one_pole(20000.0 * std::pow(d, -0.6), fs);
    yl += a * (left[i] - yl);
    yr += a * (right[i] - yr);
    left[i] = static_cast<float>(yl / d);
    right[i] = static_cast<float>(yr / d);
  }
}

void
mono_bass(std::span<float> left, std::span<float> right, double hz,
          int rate)
{
  if (hz < kMinBassMono) {
    return;
  }
  const std::size_t n = std::min(left.size(), right.size());
  const double fs = std::max(1, rate);
  const double f = std::min(hz, 0.45 * fs);
  // A Linkwitz-Riley crossover (two Butterworths in a row, 24 dB an
  // octave): each side's highs, the middle's lows on both -- the two
  // bands in one phase, so what is above the corner keeps its side.
  Biquad hl1(f, fs, true), hl2(f, fs, true);
  Biquad hr1(f, fs, true), hr2(f, fs, true);
  Biquad lm1(f, fs, false), lm2(f, fs, false);
  for (std::size_t i = 0; i < n; ++i) {
    const double low = lm2.step(lm1.step(0.5 * (left[i] + right[i])));
    left[i] = static_cast<float>(hl2.step(hl1.step(left[i])) + low);
    right[i] = static_cast<float>(hr2.step(hr1.step(right[i])) + low);
  }
}

void
crossfeed_sound(std::span<float> left, std::span<float> right,
                double amount, int rate)
{
  if (!(amount > 0)) {
    return;
  }
  const std::size_t n = std::min(left.size(), right.size());
  const double fs = std::max(1, rate);
  const double k = 0.5 * std::clamp(amount, 0.0, 1.0);
  const double a = one_pole(700.0, fs);
  const auto delay = static_cast<std::size_t>(std::llround(0.0003 * fs));
  // The low-passed sides, a delay's worth kept.
  std::vector<double> ring_l(delay + 1, 0.0), ring_r(delay + 1, 0.0);
  double ll = 0, lr = 0;
  for (std::size_t i = 0; i < n; ++i) {
    ll += a * (left[i] - ll);
    lr += a * (right[i] - lr);
    const std::size_t at = i % (delay + 1);
    // The oldest in the ring: `delay` frames ago (none yet: 0).
    const double old_l = i >= delay ? ring_l[(i + 1) % (delay + 1)] : 0;
    const double old_r = i >= delay ? ring_r[(i + 1) % (delay + 1)] : 0;
    ring_l[at] = ll;
    ring_r[at] = lr;
    const double from_r = delay == 0 ? lr : old_r;
    const double from_l = delay == 0 ? ll : old_l;
    // The other side's lows in, as late its own taken out: what both
    // sides share passes as it was.
    left[i] = static_cast<float>(left[i] + k * (from_r - from_l));
    right[i] = static_cast<float>(right[i] + k * (from_l - from_r));
  }
}

}
