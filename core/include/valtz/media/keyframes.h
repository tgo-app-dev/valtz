// Values that change over a clip -- or a composition layer's time --:
// KEYFRAMES.
//
// A clip's adjustments and crop (media/adjust.h, media/crop.h) are not
// one value but a track: keyframes at frames of the clip, and between
// two of them the values INTERPOLATED -- before the first key, the
// first; after the last, the last. A track with one key is a value for
// the whole clip, which is how a clip starts (a key at its first frame).
// Frames count from 0 in the clip as made (a trim does not move them).
//
// HOW a value goes from a key to the next is that key's (2026-10-11):
//   linear  in a straight line (as every key went before);
//   hold    not at all: the key's value until the next key's frame, then
//           the next's -- an abrupt change;
//   spline  along a cubic Bezier (Hermite) curve through the keys, its
//           handles set by the keys either side as Fritsch and Butland
//           set a monotone spline's: smooth through a key the values run
//           on past, level at one where they turn (no overshoot past
//           either key -- a scale or a level never leaves the range its
//           keys span), level at a track's ends and next to a held key.
//           Two keys alone ease in and out.
// Only the LOOK's tracks take it (adjustments, a crop's placement and
// turn); a layer's speed and sound stay linear -- the source time is a
// speed ramp's integral, and the player ramps a volume linearly.
//
// As modifiers (project::Modifier "adjust" / "crop") a track is
//   {"keys": [{"frame": 0, <the value's own keys>,
//              "interp"?: "hold" | "spline"}, ...],
//    "rate_num": 24, "rate_den": 1}
// and a still's flat value reads as a track of one key at frame 0, so
// one reader takes both. A clip's CROP is two tracks keyed apart -- where
// its frames lie (offsets and scales: "keys") and how they are turned
// ("rotate_keys": [{"frame", "rotate"}]) -- and one background colour
// for the whole clip ("pad_r" ... "pad_a").

#ifndef VALTZ_MEDIA_KEYFRAMES_H
#define VALTZ_MEDIA_KEYFRAMES_H

#include "valtz/base/json.h"
#include "valtz/base/rational.h"
#include "valtz/media/adjust.h"
#include "valtz/media/crop.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace valtz::media {

// A clip's turn, in degrees (+ clockwise): the crop's rotation, keyed on
// its own.
struct Rotation {
  double degrees = 0;

  bool identity() const { return degrees == 0; }
  bool operator==(const Rotation&) const = default;
};

// A composition layer's playback RATE (DESIGN §6a): its source advances
// `rate` seconds a second, > 0 -- a ramp between two keys eases.
inline constexpr double kMinSpeed = 0.05;
inline constexpr double kMaxSpeed = 20.0;
struct Speed {
  double rate = 1;

  bool identity() const { return rate == 1; }
  bool operator==(const Speed&) const = default;
};

// A composition layer's SOUND: its gain (`volume`, 0 mute to 1 as it is,
// up to kMaxVolume) and its pitch in semitones -- kept apart from its
// speed: a faster layer keeps its pitch.
inline constexpr double kMaxVolume = 4.0;
inline constexpr double kMaxPitch = 24.0;
struct Sound {
  double volume = 1;
  double pitch = 0;

  bool identity() const { return volume == 1 && pitch == 0; }
  bool operator==(const Sound&) const = default;
};

// `a` to `b` by `t` (0..1), each number on its own.
Speed lerp(const Speed& a, const Speed& b, double t);
Sound lerp(const Sound& a, const Sound& b, double t);
Adjustments lerp(const Adjustments& a, const Adjustments& b, double t);
Crop lerp(const Crop& a, const Crop& b, double t);
Rotation lerp(const Rotation& a, const Rotation& b, double t);

// How a key's value goes on to the next key's (above).
enum class Interp { Hold, Linear, Spline };
const char* to_str(Interp);
Interp interp_from_str(const std::string&);  // unknown: Linear

// A SPLINE's span: from key `a` to `b` (`h` frames apart, `t` of the way,
// 0..1), and the keys either side when the curve runs on through them --
// `before` `h_before` frames before `a`, `after` `h_after` after `b`.
struct SplineSpan {
  double t = 0;
  double h = 1;
  bool   before = false;
  double h_before = 1;
  bool   after = false;
  double h_after = 1;
};

// One number on that span: `a` to `b` (`p` the key before, `n` the one
// after, when there are).
double spline(double p, double a, double b, double n, const SplineSpan&);

// The same for each number of a value (`a`'s other parts kept, as lerp's).
Speed spline(const Speed& p, const Speed& a, const Speed& b, const Speed& n,
             const SplineSpan&);
Sound spline(const Sound& p, const Sound& a, const Sound& b, const Sound& n,
             const SplineSpan&);
Adjustments spline(const Adjustments& p, const Adjustments& a,
                   const Adjustments& b, const Adjustments& n,
                   const SplineSpan&);
Crop spline(const Crop& p, const Crop& a, const Crop& b, const Crop& n,
            const SplineSpan&);
Rotation spline(const Rotation& p, const Rotation& a, const Rotation& b,
                const Rotation& n, const SplineSpan&);

template <class T>
struct Keyframe {
  std::int64_t frame = 0;
  T            value;
  Interp       interp = Interp::Linear;  // on to the next key

  bool operator==(const Keyframe&) const = default;
};

template <class T>
struct Keyed {
  std::vector<Keyframe<T>> keys;  // ascending frames, no two alike
  Rational                 rate{0, 1};  // the clip's, when it was keyed

  bool empty() const { return keys.empty(); }

  // Every key is the identity (or there are none): nothing to apply.
  bool
  identity() const
  {
    return std::all_of(keys.begin(), keys.end(),
                       [](const Keyframe<T>& k) {
                         return k.value.identity();
                       });
  }

  // How the span `frame` is in goes on: its first key's (none before
  // the first key: linear). A key made inside it takes this, so a held
  // span stays held.
  Interp
  interp_at(double frame) const
  {
    Interp i = Interp::Linear;
    for (const auto& k : keys) {
      if (static_cast<double>(k.frame) > frame) {
        break;
      }
      i = k.interp;
    }
    return i;
  }

  // The value at `frame` (fractions allowed).
  T
  at(double frame) const
  {
    if (keys.empty()) {
      return T{};
    }
    if (frame <= static_cast<double>(keys.front().frame)) {
      return keys.front().value;
    }
    if (frame >= static_cast<double>(keys.back().frame)) {
      return keys.back().value;
    }
    auto b = std::upper_bound(
        keys.begin(), keys.end(), frame,
        [](double f, const Keyframe<T>& k) {
          return f < static_cast<double>(k.frame);
        });
    auto a = b - 1;
    const double span = static_cast<double>(b->frame - a->frame);
    const double t = (frame - static_cast<double>(a->frame)) / span;
    switch (a->interp) {
      case Interp::Hold:
        return a->value;
      case Interp::Linear:
        return lerp(a->value, b->value, t);
      case Interp::Spline:
        break;
    }
    // On through a key either side unless that side is held.
    SplineSpan s{.t = t, .h = span};
    const T* p = &a->value;
    const T* n = &b->value;
    if (a != keys.begin() && (a - 1)->interp != Interp::Hold) {
      s.before = true;
      s.h_before = static_cast<double>(a->frame - (a - 1)->frame);
      p = &(a - 1)->value;
    }
    if (b + 1 != keys.end() && b->interp != Interp::Hold) {
      s.after = true;
      s.h_after = static_cast<double>((b + 1)->frame - b->frame);
      n = &(b + 1)->value;
    }
    return spline(*p, a->value, b->value, *n, s);
  }

  bool operator==(const Keyed&) const = default;
};

using KeyedAdjustments = Keyed<Adjustments>;
using KeyedSpeed = Keyed<Speed>;
using KeyedSound = Keyed<Sound>;

// A clip's crop: where its frames lie on the canvas (offsets and scales;
// the keys' own rotation unused) and how they are turned, two tracks
// keyed apart, over one background colour.
struct KeyedCrop {
  Keyed<Crop>           place;
  Keyed<Rotation>       turn;
  std::array<double, 4> pad{0, 0, 0, 1};
  Rational              rate{0, 1};

  bool empty() const { return place.empty() && turn.empty(); }
  bool identity() const { return place.identity() && turn.identity(); }

  // The crop at `frame`: the placement there, turned as the clip is there,
  // over the background.
  Crop
  at(double frame) const
  {
    Crop c = place.at(frame);
    c.rotate = turn.at(frame).degrees;
    c.pad = pad;
    return c;
  }

  bool operator==(const KeyedCrop&) const = default;
};

// {"keys": [...], "rate_num", "rate_den"} (a crop's also "rotate_keys"
// and its background); {} when every key is the identity. Reading takes
// a track or a still's flat value (one key at frame 0), sorts the keys
// and drops repeated frames. A key's "interp" is written unless linear,
// and read on the look's tracks only (a speed's and a sound's: linear).
Json to_json(const KeyedAdjustments&);
Json to_json(const KeyedCrop&);
KeyedAdjustments keyed_adjustments_from_json(const Json&);
KeyedCrop keyed_crop_from_json(const Json&);
// {"keys": [{"frame", "rate"}]} and {"keys": [{"frame", "volume",
// "pitch"}]}: the modifiers "speed" and "audio" (project::Modifier).
Json to_json(const KeyedSpeed&);
Json to_json(const KeyedSound&);
KeyedSpeed keyed_speed_from_json(const Json&);
KeyedSound keyed_sound_from_json(const Json&);

}

#endif
