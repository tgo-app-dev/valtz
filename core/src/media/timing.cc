#include "valtz/media/timing.h"

#include <algorithm>
#include <cmath>

namespace valtz::media {

namespace {

double
fps_of(Rational r)
{
  const double f = r.to_double();
  return f > 0 ? f : 24.0;
}

// The integral of a linear speed from v0 to v1 over `d` seconds, taken
// for the first `u` of them.
double
ramp_integral(double v0, double v1, double d, double u)
{
  if (d <= 0) {
    return v0 * u;
  }
  return v0 * u + 0.5 * (v1 - v0) / d * u * u;
}

}

double
LayerTiming::speed_at_local(double u) const
{
  if (speed.keys.empty()) {
    return 1;
  }
  return std::clamp(speed.at(u * fps_of(rate)).rate, kMinSpeed, kMaxSpeed);
}

bool
LayerTiming::constant_speed() const
{
  return speed.keys.size() <= 1;
}

double
LayerTiming::source_at_local(double u) const
{
  if (u <= 0) {
    return in;
  }
  if (speed.keys.empty()) {
    return in + u;
  }
  const double fps = fps_of(rate);
  if (speed.keys.size() == 1) {
    return in + std::clamp(speed.keys.front().value.rate, kMinSpeed,
                           kMaxSpeed) * u;
  }
  // Segments between key times (seconds); before the first and after
  // the last, the speed holds.
  double s = in;
  double t = 0;
  auto v = [&](double sec) { return speed_at_local(sec); };
  for (const auto& k : speed.keys) {
    const double kt = static_cast<double>(k.frame) / fps;
    if (kt <= t) {
      continue;
    }
    const double seg = std::min(kt, u) - t;
    s += ramp_integral(v(t), v(kt), kt - t, seg);
    t += seg;
    if (t >= u) {
      return s;
    }
  }
  return s + v(t) * (u - t);
}

double
LayerTiming::local_at_source(double target) const
{
  if (target <= in) {
    return 0;
  }
  if (speed.keys.size() <= 1) {
    const double r = speed.keys.empty()
        ? 1.0 : std::clamp(speed.keys.front().value.rate, kMinSpeed,
                           kMaxSpeed);
    return (target - in) / r;
  }
  // Monotonic (speed > 0): bisect between bounds the extreme speeds give.
  double lo = 0;
  double hi = (target - in) / kMinSpeed;
  for (int i = 0; i < 64 && hi - lo > 1e-7; ++i) {
    const double mid = 0.5 * (lo + hi);
    if (source_at_local(mid) < target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  return hi;
}

LayerTiming
resolve_timing(std::int64_t offset, std::int64_t duration, std::int64_t in,
               std::int64_t out, Rational mark_rate, Rational rate,
               double source_seconds, KeyedSpeed speed)
{
  LayerTiming t;
  t.rate = rate.num > 0 ? rate : Rational(24, 1);
  const double fps = fps_of(t.rate);
  const double mfps = mark_rate.num > 0 ? mark_rate.to_double() : fps;
  t.start = static_cast<double>(std::max<std::int64_t>(0, offset)) / fps;
  t.speed = std::move(speed);
  t.timed = source_seconds > 0;
  t.in = in > 0 ? static_cast<double>(in) / mfps : 0.0;
  if (t.timed) {
    // A mark-out includes its frame: the span ends after it.
    t.out = out >= 0 ? static_cast<double>(out + 1) / mfps : source_seconds;
    t.out = std::min(t.out, source_seconds);
    t.in = std::min(t.in, std::max(0.0, t.out));
  }
  if (duration > 0) {
    t.length = static_cast<double>(duration) / fps;
  } else if (t.timed) {
    t.length = std::max(0.0, t.local_at_source(t.out));
  } else {
    t.length = kForever;
  }
  return t;
}

Json
to_json(const LayerTiming& t)
{
  return {{"start", t.start},
          {"length", std::isfinite(t.length) ? t.length : -1.0},
          {"in", t.in},
          {"out", std::isfinite(t.out) ? t.out : -1.0},
          {"timed", t.timed},
          {"speed", to_json(t.speed)},
          {"rate_num", t.rate.num},
          {"rate_den", t.rate.den}};
}

LayerTiming
layer_timing_from_json(const Json& j)
{
  LayerTiming t;
  if (!j.is_object()) {
    return t;
  }
  auto finite_or = [&](const char* key) {
    const double v = jget(j, key, -1.0);
    return v < 0 ? kForever : v;
  };
  t.start = std::max(0.0, jget(j, "start", 0.0));
  t.length = finite_or("length");
  t.in = std::max(0.0, jget(j, "in", 0.0));
  t.out = finite_or("out");
  t.timed = jget(j, "timed", false);
  t.speed = keyed_speed_from_json(jget(j, "speed", Json::object()));
  const auto num = jget<std::int64_t>(j, "rate_num", 24);
  const auto den = jget<std::int64_t>(j, "rate_den", 1);
  t.rate = num > 0 && den > 0 ? Rational(num, den) : Rational(24, 1);
  return t;
}

double
timeline_end(const std::vector<LayerTiming>& layers)
{
  double end = 0;
  for (const auto& l : layers) {
    if (std::isfinite(l.length)) {
      end = std::max(end, l.end());
    }
  }
  return end;
}

namespace {

struct Overlap {
  double a = 0;
  double b = 0;
  bool   any() const { return b > a; }
  double mid() const { return 0.5 * (a + b); }
};

Overlap
overlap_of(const LayerTiming& x, const LayerTiming& y)
{
  return {std::max(x.start, y.start), std::min(x.end(), y.end())};
}

}

LayerWeights
layer_weights(const std::vector<LayerTiming>& layers,
              const std::vector<TransitionSpec>& transitions, double t)
{
  LayerWeights w;
  w.opacity.resize(layers.size());
  w.gain.resize(layers.size());
  for (std::size_t i = 0; i < layers.size(); ++i) {
    const double on = layers[i].active(t) ? 1.0 : 0.0;
    w.opacity[i] = on;
    w.gain[i] = on;
  }
  for (const auto& tr : transitions) {
    if (tr.from >= layers.size() || tr.to >= layers.size() ||
        tr.from == tr.to) {
      continue;
    }
    const Overlap o = overlap_of(layers[tr.from], layers[tr.to]);
    if (!o.any() || t < o.a || t >= o.b) {
      continue;
    }
    if (!tr.dissolve) {
      // An abrupt change at the middle.
      if (t < o.mid()) {
        w.opacity[tr.to] = 0;
        w.gain[tr.to] = 0;
      } else {
        w.opacity[tr.from] = 0;
        w.gain[tr.from] = 0;
      }
      continue;
    }
    const double k = std::clamp((t - o.a) / (o.b - o.a), 0.0, 1.0);
    // The upper one fades over the lower, which stays whole: two opaque
    // pictures mix as (1 - k) from + k to.
    if (tr.to > tr.from) {
      w.opacity[tr.to] *= k;
    } else {
      w.opacity[tr.from] *= 1.0 - k;
    }
    w.gain[tr.from] *= 1.0 - k;
    w.gain[tr.to] *= k;
  }
  return w;
}

std::vector<double>
weight_breaks(const std::vector<LayerTiming>& layers,
              const std::vector<TransitionSpec>& tr, std::size_t i)
{
  std::vector<double> out;
  if (i >= layers.size()) {
    return out;
  }
  out.push_back(layers[i].start);
  if (std::isfinite(layers[i].length)) {
    out.push_back(layers[i].end());
  }
  for (const auto& t : tr) {
    if (t.from != i && t.to != i) {
      continue;
    }
    if (t.from >= layers.size() || t.to >= layers.size()) {
      continue;
    }
    const Overlap o = overlap_of(layers[t.from], layers[t.to]);
    if (!o.any()) {
      continue;
    }
    out.push_back(o.a);
    out.push_back(o.mid());
    out.push_back(o.b);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

}
