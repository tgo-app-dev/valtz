#include "valtz/media/crop.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace valtz::media {

bool
Crop::identity() const
{
  const bool own_canvas = (canvas.width == 0 && canvas.height == 0) ||
                          canvas == content;
  return scale_x == 1 && scale_y == 1 && offset_x == 0 && offset_y == 0 &&
         rotate == 0 && own_canvas;
}

Json
to_json(const Crop& c)
{
  if (c.identity()) {
    return Json::object();
  }
  return {
    {"content_w", c.content.width}, {"content_h", c.content.height},
    {"canvas_w", c.canvas.width},   {"canvas_h", c.canvas.height},
    {"scale_x", c.scale_x},         {"scale_y", c.scale_y},
    {"offset_x", c.offset_x},       {"offset_y", c.offset_y},
    {"rotate", c.rotate},
    {"pad_r", c.pad[0]}, {"pad_g", c.pad[1]}, {"pad_b", c.pad[2]},
    {"pad_a", c.pad[3]},
  };
}

Crop
crop_from_json(const Json& j)
{
  Crop c;
  if (!j.is_object()) {
    return c;
  }
  auto size = [&](const char* w, const char* h) {
    return PixelSize{std::max(0, jget(j, w, 0)), std::max(0, jget(j, h, 0))};
  };
  c.content = size("content_w", "content_h");
  c.canvas = size("canvas_w", "canvas_h");
  const double both = jget(j, "scale", 1.0);
  c.scale_x = std::clamp(jget(j, "scale_x", both), 0.01, 100.0);
  c.scale_y = std::clamp(jget(j, "scale_y", both), 0.01, 100.0);
  c.offset_x = std::clamp(jget(j, "offset_x", 0.0), -10.0, 10.0);
  c.offset_y = std::clamp(jget(j, "offset_y", 0.0), -10.0, 10.0);
  // Held to a whole turn either way -- not folded into one: a clip's
  // keys at 0 and 360 are a full turn, not none.
  c.rotate = std::clamp(jget(j, "rotate", 0.0), -kMaxTurn, kMaxTurn);
  const char* keys[] = {"pad_r", "pad_g", "pad_b", "pad_a"};
  for (int i = 0; i < 4; ++i) {
    c.pad[i] = std::clamp(jget(j, keys[i], c.pad[i]), 0.0, 1.0);
  }
  return c;
}

CropPlacement
crop_placement(const Crop& c, double w, double h)
{
  // The crop was made on the content at its own size; a decode at
  // another one scales the canvas with it.
  const double k = c.content.width > 0 ? w / c.content.width : 1;
  CropPlacement p;
  p.canvas_w = c.canvas.width > 0 ? c.canvas.width * k
               : c.content.width > 0 ? c.content.width * k
                                     : w;
  p.canvas_h = c.canvas.height > 0 ? c.canvas.height * k
               : c.content.height > 0 ? c.content.height * k
                                      : h;
  // In a y-down frame the content would be scaled along its own axes,
  // turned by +rotate (clockwise on screen) about its centre, and its
  // centre put at the canvas's centre plus the offset. Core Image's frame
  // is y-up, which turns the same rotation into -rotate and the offset's
  // y around: R(-rotate) * S(scale_x, scale_y).
  const double t = c.rotate * std::numbers::pi / 180;
  const double co = std::cos(t);
  const double si = std::sin(t);
  Affine& m = p.transform;
  m.a = co * c.scale_x;
  m.b = -si * c.scale_x;
  m.c = si * c.scale_y;
  m.d = co * c.scale_y;
  const double cx = p.canvas_w / 2 + c.offset_x * p.canvas_w;
  const double cy = p.canvas_h / 2 - c.offset_y * p.canvas_h;
  m.tx = cx - (m.a * w / 2 + m.c * h / 2);
  m.ty = cy - (m.b * w / 2 + m.d * h / 2);
  return p;
}

Json
to_json(const Trim& t)
{
  if (t.identity()) {
    return Json::object();
  }
  return {{"in", t.in}, {"out", t.out}, {"rate_num", t.rate.num},
          {"rate_den", t.rate.den}};
}

Trim
trim_from_json(const Json& j)
{
  Trim t;
  if (!j.is_object()) {
    return t;
  }
  t.in = std::max<std::int64_t>(-1, jget<std::int64_t>(j, "in", -1));
  t.out = std::max<std::int64_t>(-1, jget<std::int64_t>(j, "out", -1));
  if (t.in >= 0 && t.out >= 0 && t.out < t.in) {
    std::swap(t.in, t.out);
  }
  t.rate = Rational{jget<std::int64_t>(j, "rate_num", 0),
                    std::max<std::int64_t>(1, jget<std::int64_t>(
                                                  j, "rate_den", 1))};
  return t;
}

StackCanvas
resize_canvas(const StackCanvas& now, PixelSize own, PixelSize size,
              double anchor_x, double anchor_y)
{
  const StackCanvas cur = now.set()
      ? now
      : StackCanvas{own.width, own.height, 0, 0};
  StackCanvas next{size.width, size.height,
                   cur.x + std::clamp(anchor_x, 0.0, 1.0) *
                               (size.width - cur.width),
                   cur.y + std::clamp(anchor_y, 0.0, 1.0) *
                               (size.height - cur.height)};
  next.frame_w = now.frame_w;
  next.frame_h = now.frame_h;
  if (next.width <= 0 || next.height <= 0 ||
      (next.width == own.width && next.height == own.height &&
       std::abs(next.x) < 1e-9 && std::abs(next.y) < 1e-9)) {
    StackCanvas unset;
    unset.frame_w = now.frame_w;
    unset.frame_h = now.frame_h;
    return unset;
  }
  return next;
}

Json
to_json(const StackCanvas& c)
{
  if (!c.set() && !c.framed()) {
    return Json::object();
  }
  Json j = {{"w", c.set() ? c.width : 0}, {"h", c.set() ? c.height : 0},
            {"x", c.set() ? c.x : 0.0}, {"y", c.set() ? c.y : 0.0}};
  if (c.framed()) {
    j["fw"] = c.frame_w;
    j["fh"] = c.frame_h;
  }
  return j;
}

StackCanvas
stack_canvas_from_json(const Json& j)
{
  StackCanvas c;
  c.width = std::clamp(jget(j, "w", 0), 0, 1 << 16);
  c.height = std::clamp(jget(j, "h", 0), 0, 1 << 16);
  c.x = jget(j, "x", 0.0);
  c.y = jget(j, "y", 0.0);
  if (!std::isfinite(c.x) || !std::isfinite(c.y)) {
    c.x = c.y = 0;
  }
  if (!c.set()) {
    c.width = c.height = 0;
    c.x = c.y = 0;
  }
  c.frame_w = std::clamp(jget(j, "fw", 0), 0, 1 << 16);
  c.frame_h = std::clamp(jget(j, "fh", 0), 0, 1 << 16);
  if (!c.framed()) {
    c.frame_w = c.frame_h = 0;
  }
  return c;
}

}
