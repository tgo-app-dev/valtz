#include "engine/vpipe/job-progress.h"

#include "valtz/engine/engine.h"

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

namespace valtz::engine::vp {

namespace {

// vpipe's report names (its stages' open_progress descs) -> Valtz's.
std::string_view
phase_of(std::string_view desc)
{
  struct Name {
    std::string_view desc;
    std::string_view phase;
  };
  static constexpr Name kNames[] = {
    {"denoise", kPhaseDenoise},
    {"vae decode", kPhaseDecode},
    {"audio vae decode", kPhaseSound},
    {"encoding references", kPhaseReferences},
    {"restore", kPhaseRestore},
  };
  for (const auto& n : kNames) {
    if (n.desc == desc) {
      return n.phase;
    }
  }
  return kPhaseWork;
}

// A song's report (generate-audio's "song") is three passes under one
// name: the flow matching is the one with a count; of the two before it,
// the detail names the score while it is planned.
std::string_view
song_phase(std::uint64_t total, std::string_view detail)
{
  if (total > 0) {
    return kPhaseDenoise;
  }
  return detail.find("score") != std::string_view::npos ? kPhaseScore
                                                        : kPhaseSong;
}

// The first number in vpipe's detail ("writing the song: 42.3 s"); -1
// for none.
double
first_number(std::string_view s)
{
  const auto at = s.find_first_of("0123456789");
  if (at == std::string_view::npos) {
    return -1;
  }
  double v = 0;
  double scale = 0;
  for (std::size_t i = at; i < s.size(); ++i) {
    const char c = s[i];
    if (c >= '0' && c <= '9') {
      if (scale > 0) {
        v += (c - '0') * scale;
        scale /= 10;
      } else {
        v = v * 10 + (c - '0');
      }
    } else if (c == '.' && scale == 0) {
      scale = 0.1;
    } else {
      break;
    }
  }
  return v;
}

// The pace is the last minute's moves (and never fewer than two: a unit
// may well take longer than that).
constexpr double kPaceWindow = 60.0;

struct Item {
  std::uint64_t id = 0;
  std::uint64_t seq = 0;
  JobPhase      phase;

  // Still at work: short of its total, or uncounted.
  bool
  going() const
  {
    return phase.total == 0 || phase.done < phase.total;
  }
};

}

float
JobPhase::fraction() const
{
  if (total == 0) {
    return -1.0f;
  }
  return static_cast<float>(std::min(done, total)) /
         static_cast<float>(total);
}

Json
JobPhase::data() const
{
  Json d = {{"phase", phase},       {"done", done},   {"total", total},
            {"detail", detail},     {"label", label}, {"elapsed", elapsed},
            {"estimate", estimate}, {"rate", rate}};
  if (made >= 0) {
    d["made"] = made;
  }
  return d;
}

std::uint64_t
JobProgress::last_id(const Json& doc)
{
  std::uint64_t id = 0;
  const Json items = jget(doc, "items", Json::array());
  if (!items.is_array()) {
    return 0;
  }
  for (const auto& it : items) {
    id = std::max(id, jget<std::uint64_t>(it, "id", 0));
  }
  return id;
}

void
JobProgress::track_(std::uint64_t id, const JobPhase& p, double now)
{
  if (p.total == 0) {
    _followed = 0;
    _moves.clear();
    return;
  }
  // Another report (or the same one counting something else): its pace
  // starts over.
  if (id != _followed || p.total != _total || _moves.empty() ||
      p.done < _moves.back().second) {
    _followed = id;
    _total = p.total;
    _moves.clear();
    _moves.emplace_back(now, p.done);
    _crossed.clear();
    _based = false;
    track_groups_(p, now);
    return;
  }
  track_groups_(p, now);
  if (p.done > _moves.back().second) {
    _moves.emplace_back(now, p.done);
  }
  while (_moves.size() > 2 && now - _moves.front().first > kPaceWindow) {
    _moves.pop_front();
  }
}

// How far into a span the time says, `x` spans of it gone: in step with
// it to 80%, then only approaching its end -- continuous, still rising
// long after, and never 1 (an exponential's approach rounded to it in
// minutes).
static double
into(double x)
{
  return x < 0.8 ? x : 1.0 - 0.2 / (1.0 + (x - 0.8) / 0.2);
}

void
JobProgress::track_groups_(const JobPhase& p, double now)
{
  if (!grouped_(p)) {
    return;
  }
  const std::uint64_t base = p.done >= p.total
      ? p.total : p.done / _group * _group;
  if (!_based) {
    // Where it was first seen: not a crossing (no time to measure from).
    _base = base;
    _based = true;
  } else if (base > _base) {
    _base = base;
    _crossed.emplace_back(now, base);
    while (_crossed.size() > 2) {
      _crossed.pop_front();
    }
  }
}

bool
JobProgress::grouped_(const JobPhase& p) const
{
  return _group > 1 && p.phase == kPhaseRestore && p.total > 0;
}

void
JobProgress::estimate_(JobPhase& p, double now) const
{
  if (p.total == 0) {
    return;
  }
  const double total = static_cast<double>(p.total);
  double units = static_cast<double>(std::min(p.done, p.total));
  if (grouped_(p)) {
    // From the last boundary crossed to the next, at the last group's
    // pace; a piece of a count past the boundary is the floor.
    if (p.done < p.total && _crossed.size() >= 2) {
      const auto& [t0, b0] = _crossed.front();
      const auto& [t1, b1] = _crossed.back();
      const double per_s = static_cast<double>(b1 - b0) / (t1 - t0);
      const double next = std::min(static_cast<double>(_group),
                                   total - static_cast<double>(b1));
      if (t1 > t0 && per_s > 0 && next > 0) {
        p.rate = per_s / total;
        // A group takes a group's time however few new frames it gives:
        // the last is padded whole.
        const double x = std::max(0.0, now - t1) * per_s /
                         static_cast<double>(_group);
        units = std::max(units, static_cast<double>(b1) + next * into(x));
      }
    }
    p.estimate = std::min(1.0, units / total);
    return;
  }
  if (p.done < p.total && _moves.size() >= 2) {
    const auto& [t0, d0] = _moves.front();
    const auto& [t1, d1] = _moves.back();
    if (t1 > t0 && d1 > d0) {
      const double per_s = static_cast<double>(d1 - d0) / (t1 - t0);
      p.rate = per_s / total;
      // How far into the next unit at that pace.
      units += into(std::max(0.0, now - t1) * per_s);
    }
  }
  p.estimate = std::min(1.0, units / total);
}

JobPhase
JobProgress::read(const Json& doc, double now)
{
  std::vector<Item> live;
  const Json items = jget(doc, "items", Json::array());
  if (items.is_array()) {
    for (const auto& it : items) {
      if (!it.is_object()) {
        continue;
      }
      Item i;
      i.id = jget<std::uint64_t>(it, "id", 0);
      if (i.id <= _after) {
        continue;  // an earlier job's
      }
      i.seq = jget<std::uint64_t>(it, "seq", 0);
      const auto desc = jget<std::string>(it, "desc", "");
      JobPhase& p = i.phase;
      p.done = jget<std::uint64_t>(it, "done", 0);
      p.total = jget<std::uint64_t>(it, "total", 0);
      p.detail = jget<std::string>(it, "detail", "");
      if (_download) {
        p.phase = kPhaseDownload;
        p.label = desc;
      } else if (desc == "save video") {
        p.phase = _export    ? kPhaseExport
                  : _restore ? kPhaseRestore
                             : kPhaseFinish;
      } else if (desc == "song") {
        p.phase = song_phase(p.total, p.detail);
        if (p.total == 0) {
          p.made = first_number(p.detail);
        }
      } else if (desc == "speech") {
        // Speech (text-to-speech's "speech"): spoken a frame per 80 ms --
        // counted against the length asked for, else what it has so far,
        // in seconds -- then decoded.
        p.phase = p.detail == "decoding" ? kPhaseSound : kPhaseSpeech;
        if (p.phase == kPhaseSpeech) {
          p.made = first_number(p.detail);
        }
      } else {
        p.phase = phase_of(desc);
        if (p.phase == kPhaseWork) {
          p.label = desc;
        }
      }
      p.elapsed = jget<std::uint64_t>(it, "elapsed_ms", 0) / 1000.0;
      live.push_back(std::move(i));
    }
  }
  // An upscale's frames written are the whole of it, once they begin.
  if (_restore && std::ranges::any_of(live, [](const Item& i) {
        return i.phase.phase == kPhaseRestore;
      })) {
    std::erase_if(live, [](const Item& i) {
      return i.phase.phase != kPhaseRestore;
    });
  }
  for (const auto& i : live) {
    // Past the main work -- a denoise, an export's frames -- what is
    // left is finishing (a clip's soundtrack joined).
    if (i.phase.phase == kPhaseDenoise || i.phase.phase == kPhaseExport) {
      _past = true;
    }
  }
  // The document lists the oldest-opened first.
  const Item* pick = nullptr;
  for (const auto& i : live) {
    if (i.going()) {
      pick = &i;
      break;
    }
  }
  if (pick == nullptr && !live.empty()) {
    pick = &*std::max_element(live.begin(), live.end(),
                              [](const Item& a, const Item& b) {
                                return a.seq < b.seq;
                              });
  }
  if (pick != nullptr) {
    JobPhase p = pick->phase;
    track_(pick->id, p, now);
    estimate_(p, now);
    return p;
  }
  _followed = 0;
  _moves.clear();
  // Nothing live. A download counts only its big files: between them
  // it is still downloading, uncounted.
  JobPhase idle;
  idle.phase = _download ? kPhaseDownload
               : _past   ? kPhaseFinish
                         : kPhasePrepare;
  return idle;
}

}
