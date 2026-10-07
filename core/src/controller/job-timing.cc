#include "valtz/controller/job-timing.h"

#include <algorithm>
#include <array>
#include <string_view>

namespace valtz {

namespace {

// The order a generation's phases come in (engine.h, kPhase*).
constexpr std::array<std::string_view, 9> kOrder = {
  "prepare", "references", "score", "song", "denoise", "decode", "sound",
  "restore", "finish"};

int
order_of(std::string_view p)
{
  const auto it = std::find(kOrder.begin(), kOrder.end(), p);
  return it == kOrder.end() ? -1 : static_cast<int>(it - kOrder.begin());
}

// A count this far along says when its phase ends.
constexpr double kPaced = 0.05;

}

JobTiming::JobTiming(double start, Json prior, double volume, double frames)
    : _prior(std::move(prior)), _volume(volume), _frames(frames),
      _start(start), _since(start)
{
  if (_prior.is_object()) {
    const double v = jget(_prior, "volume", 0.0);
    const double f = jget(_prior, "frames", 0.0);
    _volume_ratio = v > 0 && volume > 0 ? volume / v : 1;
    _frames_ratio = f > 0 && frames > 0 ? frames / f : 1;
  }
}

void
JobTiming::start(double now)
{
  _start = _since = now;
  _phases = Json::object();
  _sequence = Json::array();
}

void
JobTiming::note(const Json& progress, double now)
{
  const auto phase = jget<std::string>(progress, "phase", "");
  if (phase.empty()) {
    return;
  }
  if (phase != _phase) {
    _phases[_phase] = jget(_phases, _phase.c_str(), 0.0) + (now - _since);
    _sequence.push_back(Json::array({_phase, now - _since}));
    _phase = phase;
    _since = now;
    _fraction = -1;
    _rate = 0;
    _counted_at = -1;
  }
  double f = jget(progress, "estimate", -1.0);
  const auto total = jget<std::uint64_t>(progress, "total", 0);
  if (f < 0 && total > 0) {
    f = static_cast<double>(jget<std::uint64_t>(progress, "done", 0)) /
        static_cast<double>(total);
  }
  if (f >= 0 && _counted_at < 0) {
    _counted_at = now;
    _counted_from = f;
  }
  _fraction = std::min(1.0, f);
  _rate = jget(progress, "rate", 0.0);
  if (_fraction > kPaced) {
    _paced = true;
  }
}

double
JobTiming::scaled_(const std::string& p, double seconds) const
{
  if (p == "decode" || p == "references" || p == "restore") {
    return seconds * _volume_ratio;
  }
  if (p == "sound") {
    return seconds * _frames_ratio;
  }
  return seconds;
}

std::optional<double>
JobTiming::expected_(const std::string& p) const
{
  const Json phases = jget(_prior, "phases", Json::object());
  if (!phases.contains(p) || !phases[p].is_number()) {
    return std::nullopt;
  }
  return scaled_(p, phases[p].get<double>());
}

std::optional<std::pair<double, double>>
JobTiming::along_() const
{
  const Json seq = jget(_prior, "sequence", Json::array());
  const std::size_t at = _sequence.size();
  auto phase_of = [](const Json& e) {
    return e.is_array() && e.size() == 2 && e[0].is_string() &&
                   e[1].is_number()
               ? e[0].get<std::string>()
               : std::string();
  };
  if (!seq.is_array() || seq.size() <= at || phase_of(seq[at]) != _phase) {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < at; ++i) {
    if (phase_of(seq[i]) != _sequence[i][0].get<std::string>()) {
      return std::nullopt;
    }
  }
  const double here = scaled_(_phase, seq[at][1].get<double>());
  double rest = 0;
  for (std::size_t i = at + 1; i < seq.size(); ++i) {
    rest += scaled_(phase_of(seq[i]), seq[i][1].get<double>());
  }
  return std::pair{here, rest};
}

std::optional<double>
JobTiming::left(double now) const
{
  const auto along = along_();
  double here = 0;
  if (_fraction > kPaced) {
    // The phase at its pace: the recent one, else its own average since
    // its count began (its loading left out).
    double rate = _rate;
    if (rate <= 0 && _counted_at >= 0 && now > _counted_at) {
      rate = (_fraction - _counted_from) / (now - _counted_at);
    }
    if (rate <= 0) {
      return std::nullopt;
    }
    here = (1 - _fraction) / rate;
  } else if (_paced) {
    // Not counted, or not far yet: as long as it took last time.
    const auto e = along ? std::optional<double>(along->first)
                         : expected_(_phase);
    if (!e) {
      return std::nullopt;
    }
    here = std::max(0.0, *e - (now - _since));
  } else {
    return std::nullopt;
  }
  // The phases still to come, as they went last time: the rest of its
  // sequence, else those later in the usual order.
  if (along) {
    return here + along->second;
  }
  const int at = order_of(_phase);
  double after = 0;
  if (at >= 0) {
    for (int i = at + 1; i < static_cast<int>(kOrder.size()); ++i) {
      after += expected_(std::string(kOrder[i])).value_or(0);
    }
  }
  return here + after;
}

Json
JobTiming::record(double now) const
{
  Json phases = _phases;
  phases[_phase] = jget(phases, _phase.c_str(), 0.0) + (now - _since);
  Json sequence = _sequence;
  sequence.push_back(Json::array({_phase, now - _since}));
  return {{"seconds", now - _start}, {"phases", std::move(phases)},
          {"sequence", std::move(sequence)}};
}

Json
JobTiming::prior(double now) const
{
  Json j = record(now);
  j["volume"] = _volume;
  j["frames"] = _frames;
  return j;
}

}
