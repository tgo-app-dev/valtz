#include "valtz/media/keyframes.h"

#include <cmath>

namespace valtz::media {

namespace {

double
mix(double a, double b, double t)
{
  return a + (b - a) * t;
}

int
mix(int a, int b, double t)
{
  return static_cast<int>(std::lround(mix(static_cast<double>(a),
                                          static_cast<double>(b), t)));
}

// A track's JSON from its keys' own: each key's value, with its frame.
template <class T, class ValueJson>
Json
track_json(const Keyed<T>& k, ValueJson value_json)
{
  if (k.identity()) {
    return Json::object();
  }
  Json keys = Json::array();
  for (const auto& key : k.keys) {
    Json v = value_json(key.value);
    if (!v.is_object()) {
      v = Json::object();
    }
    v["frame"] = key.frame;
    keys.push_back(std::move(v));
  }
  return {{"keys", std::move(keys)},
          {"rate_num", k.rate.num},
          {"rate_den", k.rate.den}};
}

template <class T, class ValueFromJson>
Keyed<T>
track_from_json(const Json& j, ValueFromJson value_from_json)
{
  Keyed<T> k;
  if (!j.is_object()) {
    return k;
  }
  const Json keys = jget(j, "keys", Json());
  if (!keys.is_array()) {
    // A still's flat value: the whole clip.
    if (!j.empty()) {
      k.keys.push_back({0, value_from_json(j)});
    }
    return k;
  }
  for (const auto& v : keys) {
    if (!v.is_object()) {
      continue;
    }
    const auto f = std::max<std::int64_t>(0, jget<std::int64_t>(v, "frame",
                                                                 0));
    k.keys.push_back({f, value_from_json(v)});
  }
  std::stable_sort(k.keys.begin(), k.keys.end(),
                   [](const Keyframe<T>& a, const Keyframe<T>& b) {
                     return a.frame < b.frame;
                   });
  k.keys.erase(std::unique(k.keys.begin(), k.keys.end(),
                           [](const Keyframe<T>& a, const Keyframe<T>& b) {
                             return a.frame == b.frame;
                           }),
               k.keys.end());
  k.rate = Rational{jget<std::int64_t>(j, "rate_num", 0),
                    std::max<std::int64_t>(1, jget<std::int64_t>(
                                                  j, "rate_den", 1))};
  return k;
}

}

Speed
lerp(const Speed& a, const Speed& b, double t)
{
  return {mix(a.rate, b.rate, t)};
}

Sound
lerp(const Sound& a, const Sound& b, double t)
{
  return {mix(a.volume, b.volume, t), mix(a.pitch, b.pitch, t)};
}

Json
to_json(const KeyedSpeed& k)
{
  return track_json(k, [](const Speed& v) { return Json{{"rate", v.rate}}; });
}

Json
to_json(const KeyedSound& k)
{
  return track_json(k, [](const Sound& v) {
    return Json{{"volume", v.volume}, {"pitch", v.pitch}};
  });
}

KeyedSpeed
keyed_speed_from_json(const Json& j)
{
  return track_from_json<Speed>(j, [](const Json& v) {
    return Speed{std::clamp(jget(v, "rate", 1.0), kMinSpeed, kMaxSpeed)};
  });
}

KeyedSound
keyed_sound_from_json(const Json& j)
{
  return track_from_json<Sound>(j, [](const Json& v) {
    return Sound{std::clamp(jget(v, "volume", 1.0), 0.0, kMaxVolume),
                 std::clamp(jget(v, "pitch", 0.0), -kMaxPitch, kMaxPitch)};
  });
}

Adjustments
lerp(const Adjustments& a, const Adjustments& b, double t)
{
  Adjustments o;
  o.exposure = mix(a.exposure, b.exposure, t);
  o.contrast = mix(a.contrast, b.contrast, t);
  o.highlights = mix(a.highlights, b.highlights, t);
  o.shadows = mix(a.shadows, b.shadows, t);
  o.vibrance = mix(a.vibrance, b.vibrance, t);
  o.saturation = mix(a.saturation, b.saturation, t);
  o.temperature = mix(a.temperature, b.temperature, t);
  o.tint = mix(a.tint, b.tint, t);
  return o;
}

Crop
lerp(const Crop& a, const Crop& b, double t)
{
  // The content is the clip's frame either way; a key that never
  // changed anything does not know it yet.
  Crop o = a.content.width > 0 ? a : b;
  o.canvas = {mix(a.canvas.width, b.canvas.width, t),
              mix(a.canvas.height, b.canvas.height, t)};
  o.scale_x = mix(a.scale_x, b.scale_x, t);
  o.scale_y = mix(a.scale_y, b.scale_y, t);
  o.offset_x = mix(a.offset_x, b.offset_x, t);
  o.offset_y = mix(a.offset_y, b.offset_y, t);
  o.rotate = mix(a.rotate, b.rotate, t);
  for (int i = 0; i < 4; ++i) {
    o.pad[i] = mix(a.pad[i], b.pad[i], t);
  }
  return o;
}

Rotation
lerp(const Rotation& a, const Rotation& b, double t)
{
  return {mix(a.degrees, b.degrees, t)};
}

Json
to_json(const KeyedAdjustments& k)
{
  return track_json(k, [](const Adjustments& a) { return to_json(a); });
}

Json
to_json(const KeyedCrop& k)
{
  if (k.identity()) {
    return Json::object();
  }
  // Every key spelled out, identity or not: it still says where it is.
  // The placement keys hold where the frame lies; the turn and the
  // background are the crop's own.
  Json keys = Json::array();
  for (const auto& key : k.place.keys) {
    const Crop& c = key.value;
    keys.push_back({{"frame", key.frame},
                    {"content_w", c.content.width},
                    {"content_h", c.content.height},
                    {"canvas_w", c.canvas.width},
                    {"canvas_h", c.canvas.height},
                    {"scale_x", c.scale_x},
                    {"scale_y", c.scale_y},
                    {"offset_x", c.offset_x},
                    {"offset_y", c.offset_y}});
  }
  Json turns = Json::array();
  for (const auto& key : k.turn.keys) {
    turns.push_back({{"frame", key.frame}, {"rotate", key.value.degrees}});
  }
  return {{"keys", std::move(keys)},
          {"rotate_keys", std::move(turns)},
          {"pad_r", k.pad[0]}, {"pad_g", k.pad[1]},
          {"pad_b", k.pad[2]}, {"pad_a", k.pad[3]},
          {"rate_num", k.rate.num},
          {"rate_den", k.rate.den}};
}

KeyedAdjustments
keyed_adjustments_from_json(const Json& j)
{
  return track_from_json<Adjustments>(
      j, [](const Json& v) { return adjustments_from_json(v); });
}

KeyedCrop
keyed_crop_from_json(const Json& j)
{
  KeyedCrop k;
  if (!j.is_object() || j.empty()) {
    return k;
  }
  k.place = track_from_json<Crop>(
      j, [](const Json& v) { return crop_from_json(v); });
  k.rate = k.place.rate;
  // The turn: its own track; else (a still, or a crop keyed whole) the
  // placement keys' own rotations.
  const Json turns = jget(j, "rotate_keys", Json());
  if (turns.is_array()) {
    Json t = {{"keys", turns}, {"rate_num", k.rate.num},
              {"rate_den", k.rate.den}};
    k.turn = track_from_json<Rotation>(t, [](const Json& v) {
      return Rotation{std::clamp(jget(v, "rotate", 0.0), -kMaxTurn,
                                 kMaxTurn)};
    });
  } else {
    for (const auto& key : k.place.keys) {
      k.turn.keys.push_back({key.frame, {key.value.rotate}});
    }
  }
  k.turn.rate = k.rate;
  for (auto& key : k.place.keys) {
    key.value.rotate = 0;
  }
  // One background for the whole clip: the track's, else its first key's.
  const Crop first = k.place.empty() ? crop_from_json(j)
                                     : k.place.keys.front().value;
  const char* names[] = {"pad_r", "pad_g", "pad_b", "pad_a"};
  for (int i = 0; i < 4; ++i) {
    k.pad[i] = std::clamp(jget(j, names[i], first.pad[i]), 0.0, 1.0);
  }
  return k;
}

}
