#ifndef VALTZ_ENGINE_VPIPE_FLOPS_HOLD_H
#define VALTZ_ENGINE_VPIPE_FLOPS_HOLD_H

// HOW LONG THE FLOP/s READING STANDS after the newest credit. vpipe
// credits a DiT block's arithmetic when it retires, spread over the time
// it ran, so the reading is an average over the window ending where the
// credits reach -- and a unit whose credits stop must fall to zero.
//
// A fixed hold of three seconds was too short for a big clip: a 2K
// refine of 362 frames (~385,000 rows a block) retires a block every 8 s
// or so, and the meter read its rate for five seconds and 0 for three,
// the GPUs busy throughout (reported, 2026-11-15). So while a job runs
// the hold follows the gaps between credits as they come -- the longest
// of the last few, with a margin -- never below the three seconds, never
// past a minute; with nothing running it is the three seconds again, so
// a stopped job falls to zero as before.
//
// Fed the newest credit's end at each reading (`note`): gaps are between
// the distinct ends it has seen, so credits landing between two readings
// count as one.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace valtz::engine {

class FlopsHold {
public:
  static constexpr std::uint64_t kMinNs = 3'000'000'000;
  static constexpr std::uint64_t kMaxNs = 60'000'000'000;

  // The newest credit's end, as read now.
  void
  note(std::uint64_t latest_ns)
  {
    if (latest_ns <= _last) {
      return;
    }
    if (_last > 0) {
      _gaps[_next] = latest_ns - _last;
      _next = (_next + 1) % _gaps.size();
      _seen = std::min(_seen + 1, _gaps.size());
    }
    _last = latest_ns;
  }

  // How long the reading stands past the newest credit: `running`, a job
  // is under way.
  std::uint64_t
  hold(bool running) const
  {
    if (!running) {
      return kMinNs;
    }
    std::uint64_t g = 0;
    for (std::size_t i = 0; i < _seen; ++i) {
      g = std::max(g, _gaps[i]);
    }
    return std::clamp(g + g / 4, kMinNs, kMaxNs);
  }

private:
  std::array<std::uint64_t, 4> _gaps{};
  std::size_t                  _next = 0;
  std::size_t                  _seen = 0;
  std::uint64_t                _last = 0;
};

}  // namespace valtz::engine

#endif
