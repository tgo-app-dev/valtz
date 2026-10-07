// Values that change over a clip -- or a composition layer's time --:
// KEYFRAMES.
//
// A clip's adjustments and crop (media/adjust.h, media/crop.h) are not
// one value but a track: keyframes at frames of the clip, and between
// two of them the values interpolated LINEARLY -- before the first key,
// the first; after the last, the last. A track with one key is a value
// for the whole clip, which is how a clip starts (a key at its first
// frame). Frames count from 0 in the clip as made (a trim does not move
// them).
//
// As modifiers (project::Modifier "adjust" / "crop") a track is
//   {"keys": [{"frame": 0, <the value's own keys>}, ...],
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

template <class T>
struct Keyframe {
  std::int64_t frame = 0;
  T            value;

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
    return lerp(a->value, b->value,
                (frame - static_cast<double>(a->frame)) / span);
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
// and drops repeated frames.
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
