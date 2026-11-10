// A sound's BEATS (DESIGN §4v): where the beats of its music fall, found
// by vpipe's beat-track (librosa 0.11's beat tracker: the median mel
// onset envelope, the tempo from its autocorrelation, Ellis's dynamic
// program) over the whole sound -- a sound, a clip's sound, a
// composition's mix -- and kept as DATA with the text asset that tells
// them (its version's outputs "beats").
//
// ATTACHED to a sound or a clip (project::Asset::beats), they go with it
// onto every timeline: each layer showing it has them where they play --
// through its marks, its speed, its start (beats_on_timeline) -- for the
// timeline to draw as events and snap to.

#ifndef VALTZ_MEDIA_BEATS_H
#define VALTZ_MEDIA_BEATS_H

#include "valtz/base/json.h"
#include "valtz/media/timing.h"

#include <string>
#include <string_view>
#include <vector>

namespace valtz::media {

struct Beats {
  // The tempo the beats were placed under, in bpm (the whole sound's).
  double              tempo = 0;
  // Each beat, in seconds from the sound's first sample, in order.
  std::vector<double> times;
  // How long the sound heard was, in seconds.
  double              seconds = 0;
};

// {"tempo", "times", "seconds"}; times to the microsecond.
Json to_json(const Beats&);
Beats beats_from_json(const Json&);

// What a person reads of them (the text asset): the tempo, how many and
// over how long, then each beat's time, a bar of four to a line --
// "1:02.345". `name` the sound's.
std::string beats_text(std::string_view name, const Beats&);

// Where `times` -- seconds of a layer's SOURCE -- fall on its timeline:
// those inside the span it shows (its marks), each at the timeline
// second the layer's speed brings it to, in order. A layer of no time (a
// picture) has none.
std::vector<double> beats_on_timeline(const std::vector<double>& times,
                                      const LayerTiming& timing);

}

#endif
