// Where a composition's layers are in TIME (DESIGN §6a), resolved to
// seconds: one definition for the frames a writer draws, the frames the
// player draws and the sound the mixer makes.
//
// A layer STARTS at `start` on the timeline and runs for `length` (to the
// timeline's end when it has none: a picture). Its SOURCE TIME at local
// time u (seconds after its start) is `in` plus the integral of its speed
// up to u -- speed keyed at the layer's own frames of the composition's
// rate, linear between keys, so the integral is exact (a trapezoid a
// segment). With no duration of its own, a timed layer (a clip, a sound, a
// timeline) runs until its source reaches its mark-out, or its own end.
//
// TRANSITIONS weigh two overlapping layers: a cut swaps them at the middle
// of the overlap; a dissolve cross-fades -- the upper one's opacity ramps
// (so two opaque pictures mix as (1 - w) a + w b), and their sounds' gains
// ramp against each other.

#ifndef VALTZ_MEDIA_TIMING_H
#define VALTZ_MEDIA_TIMING_H

#include "valtz/base/json.h"
#include "valtz/base/rational.h"
#include "valtz/media/keyframes.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace valtz::media {

inline constexpr double kForever = std::numeric_limits<double>::infinity();

struct LayerTiming {
  double     start = 0;          // timeline seconds
  double     length = kForever;  // timeline seconds it runs
  double     in = 0;             // source seconds at its start
  double     out = kForever;     // source seconds where its span ends
  bool       timed = false;      // a clip, a sound, a timeline
  KeyedSpeed speed;              // at its own frames of `rate`
  Rational   rate{24, 1};        // the composition's

  double end() const { return start + length; }
  // Showing at timeline second `t`?
  bool active(double t) const { return t >= start && t < end(); }
  // Source seconds at local second `u` (after its start).
  double source_at_local(double u) const;
  // Source seconds at timeline second `t`.
  double source_at(double t) const { return source_at_local(t - start); }
  // The local second at which its source reaches `s` (the inverse; past
  // what its speed can reach, kForever).
  double local_at_source(double s) const;
  // Its speed at local second `u`.
  double speed_at_local(double u) const;
  bool constant_speed() const;
};

// A layer's timing from its record's numbers: `offset`, `duration` in the
// composition's frames at `rate`; `in`, `out` in its source's frames at
// `mark_rate` (-1 unset); `source_seconds` the source's own length (0:
// untimed -- a picture). The length is `duration` when set, else what its
// span takes at its speed, else forever.
LayerTiming resolve_timing(std::int64_t offset, std::int64_t duration,
                           std::int64_t in, std::int64_t out,
                           Rational mark_rate, Rational rate,
                           double source_seconds, KeyedSpeed speed);

// {"start", "length", "in", "out", "timed", "speed", "rate_num",
//  "rate_den"}; forever is -1.
Json to_json(const LayerTiming&);
LayerTiming layer_timing_from_json(const Json&);

// The latest end among timed layers (seconds); 0 when none is timed.
double timeline_end(const std::vector<LayerTiming>& layers);

// Where two layers' spans overlap, `from` ending first: an abrupt CUT at
// the overlap's middle, a DISSOLVE across it (picture and sound), or a
// DIP through black or white -- `from` going to the colour by the middle
// (its sound fading out), `to` coming out of it after (its sound in).
struct TransitionSpec {
  enum class Kind { Cut, Dissolve, DipBlack, DipWhite };
  std::size_t from = 0;  // layer indices, bottom first
  std::size_t to = 0;
  Kind        kind = Kind::Cut;
};

// project::Transition's kinds: "cut", "dissolve", "dip-black",
// "dip-white"; nullopt for another.
std::optional<TransitionSpec::Kind> transition_kind(std::string_view);
std::string_view to_string(TransitionSpec::Kind);

// The weights of every layer at timeline second `t`: `opacity` (its
// picture) and `gain` (its sound), each 0..1, after `transitions` -- 0
// where a layer is not active -- and a dip's: how far its picture has
// gone to the dip's colour, 0..1, and that colour (0 black, 1 white).
struct LayerWeights {
  std::vector<double> opacity;
  std::vector<double> gain;
  std::vector<double> dip;
  std::vector<double> dip_level;
};
LayerWeights layer_weights(const std::vector<LayerTiming>& layers,
                           const std::vector<TransitionSpec>& transitions,
                           double t);

// Where layer `i`'s weights change over time -- its start and end, and
// each transition's overlap edges and middle -- sorted, for a mixer that
// ramps between them.
std::vector<double> weight_breaks(const std::vector<LayerTiming>& layers,
                                  const std::vector<TransitionSpec>& tr,
                                  std::size_t i);

}

#endif
