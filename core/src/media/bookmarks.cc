#include "valtz/media/bookmarks.h"

#include <algorithm>
#include <cmath>

namespace valtz::media {

std::string_view
kind_name(Bookmark::Kind k)
{
  switch (k) {
  case Bookmark::Kind::Speech: return "speech";
  case Bookmark::Kind::Sound: return "sound";
  case Bookmark::Kind::Scene: return "scene";
  }
  return "speech";
}

namespace {

// Each entry of `list` with a start, its words under `words`.
void
read_marks(const Json& list, const char* words, Bookmark::Kind kind,
           std::vector<Bookmark>& out)
{
  if (!list.is_array()) {
    return;
  }
  for (const auto& e : list) {
    const Json s = jget(e, "start", Json());
    if (!s.is_number()) {
      continue;
    }
    Bookmark b;
    b.kind = kind;
    b.start = std::max(0.0, s.get<double>());
    b.end = std::max(b.start, jget(e, "end", b.start));
    b.text = jget<std::string>(e, words, "");
    out.push_back(std::move(b));
  }
}

void
in_order(std::vector<Bookmark>& v)
{
  std::ranges::stable_sort(v, {}, &Bookmark::start);
}

}  // namespace

std::vector<Bookmark>
bookmarks_from_transcript(const Json& j)
{
  std::vector<Bookmark> out;
  read_marks(jget(j, "lines", Json()), "text", Bookmark::Kind::Speech, out);
  read_marks(jget(j, "events", Json()), "label", Bookmark::Kind::Sound,
             out);
  in_order(out);
  return out;
}

std::vector<Bookmark>
bookmarks_from_summary(const Json& j)
{
  std::vector<Bookmark> out;
  read_marks(jget(j, "scenes", Json()), "text", Bookmark::Kind::Scene, out);
  in_order(out);
  return out;
}

std::vector<Bookmark>
bookmarks_on_timeline(const std::vector<Bookmark>& marks,
                      const LayerTiming& t)
{
  std::vector<Bookmark> out;
  if (!t.timed || marks.empty()) {
    return out;
  }
  for (const auto& m : marks) {
    // Starting within what it shows: from its mark-in up to its mark-out.
    if (m.start < t.in || m.start >= t.out) {
      continue;
    }
    const double u = t.local_at_source(m.start);
    if (!std::isfinite(u) || u >= t.length) {
      continue;
    }
    // Its end where the layer's speed brings it, no later than the layer
    // ends: past its mark-out, where the span does.
    double v = t.local_at_source(std::min(m.end, t.out));
    if (!std::isfinite(v)) {
      v = t.length;
    }
    v = std::clamp(v, u, t.length);
    Bookmark b = m;
    b.start = t.start + u;
    b.end = t.start + v;
    out.push_back(std::move(b));
  }
  return out;
}

}
