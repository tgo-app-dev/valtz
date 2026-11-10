#include "valtz/media/beats.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace valtz::media {

Json
to_json(const Beats& b)
{
  Json times = Json::array();
  for (const double t : b.times) {
    times.push_back(std::round(t * 1e6) / 1e6);
  }
  return {{"tempo", std::round(b.tempo * 1000) / 1000},
          {"times", std::move(times)},
          {"seconds", std::round(b.seconds * 1000) / 1000}};
}

Beats
beats_from_json(const Json& j)
{
  Beats b;
  b.tempo = jget(j, "tempo", 0.0);
  b.seconds = jget(j, "seconds", 0.0);
  for (const auto& t : jget(j, "times", Json::array())) {
    if (t.is_number()) {
      b.times.push_back(t.get<double>());
    }
  }
  std::ranges::sort(b.times);
  return b;
}

namespace {

// "1:02.345": minutes, seconds, milliseconds.
std::string
clock(double s)
{
  const auto ms = std::llround(std::max(0.0, s) * 1000);
  return std::format("{}:{:02}.{:03}", ms / 60000, ms / 1000 % 60,
                     ms % 1000);
}

}  // namespace

std::string
beats_text(std::string_view name, const Beats& b)
{
  std::string out = std::format("# Beats of {}\n\n", name);
  if (b.times.empty()) {
    out += "No beats were found: nothing in it keeps a steady beat.\n";
    return out;
  }
  out += std::format("{:.1f} bpm, {} beats from {} to {}.\n\n", b.tempo,
                     b.times.size(), clock(b.times.front()),
                     clock(b.times.back()));
  // Four to a line, as a bar of four would be counted.
  for (std::size_t i = 0; i < b.times.size(); ++i) {
    out += clock(b.times[i]);
    out += (i % 4 == 3 || i + 1 == b.times.size()) ? "\n" : "  ";
  }
  return out;
}

std::vector<double>
beats_on_timeline(const std::vector<double>& times, const LayerTiming& t)
{
  std::vector<double> out;
  if (!t.timed || times.empty()) {
    return out;
  }
  for (const double s : times) {
    // Within what it shows: from its mark-in up to its mark-out.
    if (s < t.in || s >= t.out) {
      continue;
    }
    const double u = t.local_at_source(s);
    if (!std::isfinite(u) || u >= t.length) {
      continue;
    }
    out.push_back(t.start + u);
  }
  return out;
}

}
