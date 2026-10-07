// How long a generation takes: its phases timed as they pass (kept with
// the result, and shown in the inspector), and how long it has left.
//
// THE TIME LEFT is the phase's own and the phases after it. The phase
// running is measured: once its count is past 5% (vpipe's own counts and
// the engine's estimate between them), its recent pace says when it
// ends. The phases after it are not known yet -- a clip's decode and
// soundtrack come after its denoise -- so they are taken from the last
// job of the same model and operation (its PRIOR): each phase as long as
// it took then, scaled by how much more there is to make -- a decode by
// pixels x frames, a soundtrack by frames; loading and finishing as they
// were. The prior is read as the SEQUENCE its phases came in, not by a
// fixed order: a phase can come twice (a clip "finishes" its denoise
// before its decode opens, and again after it), and what follows the
// first is not what follows the second. A job that has gone its prior's
// way so far is given the rest of it; one that has not, the prior's
// phases that come later in the usual order. With no prior the estimate
// covers the phase running only. A phase that is not counted (yet) is
// estimated from the prior alone, and only once an earlier phase has
// been: before any count passes 5%, there is no estimate.

#ifndef VALTZ_CONTROLLER_JOB_TIMING_H
#define VALTZ_CONTROLLER_JOB_TIMING_H

#include "valtz/base/json.h"

#include <optional>
#include <string>

namespace valtz {

class JobTiming {
public:
  // `start`: seconds on a steady clock. `prior`: a record() of an
  // earlier job of the same model and operation (with its "volume" and
  // "frames"), or null. `volume`: this job's pixels x frames; `frames`
  // its frames (1 for a picture).
  JobTiming(double start, Json prior, double volume, double frames);

  // The clock restarted: the job left the queue.
  void start(double now);

  // A job.progress event's data (engine.h: "phase", "estimate", "rate",
  // "total"), at `now`.
  void note(const Json& progress, double now);

  // Seconds left at `now`; none when there is no estimate yet.
  std::optional<double> left(double now) const;

  // What it took, at `now`: {"seconds", "phases": {phase: seconds},
  // "sequence": [[phase, seconds], ...]} -- every phase it went through
  // (in total, and in the order it came), the one running closed at
  // `now`.
  Json record(double now) const;

  // The prior a later job reads: record() with this job's "volume" and
  // "frames".
  Json prior(double now) const;

private:
  // How long phase `p` took in the prior (in all), scaled to this job;
  // none when the prior has no such phase.
  std::optional<double> expected_(const std::string& p) const;
  // `seconds` of phase `p` in the prior, scaled to this job.
  double scaled_(const std::string& p, double seconds) const;
  // The prior's sequence from where this job is, when it has gone the
  // prior's way so far: [the phase running's seconds, the rest's].
  std::optional<std::pair<double, double>> along_() const;

  Json        _prior;
  double      _volume_ratio = 1;
  double      _frames_ratio = 1;
  double      _volume = 0;
  double      _frames = 1;
  double      _start = 0;
  std::string _phase = "prepare";  // before any report
  double      _since = 0;          // when it began
  Json        _phases = Json::object();
  Json        _sequence = Json::array();  // the phases before it
  double      _fraction = -1;      // of the phase running; -1 uncounted
  double      _rate = 0;           // its pace, of the phase per second
  double      _counted_at = -1;    // when its count began
  double      _counted_from = 0;   //   and from what fraction
  bool        _paced = false;      // a phase's count has passed 5%
};

}

#endif
