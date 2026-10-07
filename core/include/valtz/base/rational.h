// Exact rational numbers for frame rates and media time.
//
// Frame rates like 24000/1001 are not representable in floating point,
// and a timeline that accumulates 1/23.976 in a double drifts a frame
// within an hour. Media time is therefore always value/timescale in
// integers (the CMTime model), and conversion to seconds happens only
// for display.

#ifndef VALTZ_BASE_RATIONAL_H
#define VALTZ_BASE_RATIONAL_H

#include <cstdint>
#include <numeric>

namespace valtz {

struct Rational {
  std::int64_t num = 0;
  std::int64_t den = 1;

  constexpr Rational() = default;
  constexpr Rational(std::int64_t n, std::int64_t d = 1) : num(n), den(d)
  {
    normalize();
  }

  constexpr bool valid() const noexcept { return den != 0; }
  constexpr double to_double() const noexcept
  {
    return den ? static_cast<double>(num) / static_cast<double>(den) : 0.0;
  }

  friend constexpr bool
  operator==(const Rational& a, const Rational& b) noexcept
  {
    return a.num == b.num && a.den == b.den;
  }

private:
  constexpr void
  normalize() noexcept
  {
    if (den == 0) {
      return;
    }
    if (den < 0) {
      num = -num;
      den = -den;
    }
    auto g = std::gcd(num < 0 ? -num : num, den);
    if (g > 1) {
      num /= g;
      den /= g;
    }
  }
};

// A point on a media timeline: `value / timescale` seconds.
struct MediaTime {
  std::int64_t value = 0;
  std::int32_t timescale = 0;  // 0 = invalid / unknown

  constexpr bool valid() const noexcept { return timescale > 0; }
  constexpr double seconds() const noexcept
  {
    return valid() ? static_cast<double>(value) / timescale : 0.0;
  }
};

}

#endif
