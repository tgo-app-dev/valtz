// MARKUP: what the markup toolbar puts on a picture, as pixels.
//
// A markup layer (project::Markup) is a RASTER -- brush strokes, erasing,
// objects made pixels -- with VECTOR objects over it, which stay
// editable. Both are in CANVAS pixels: the layer stack's canvas (the
// bottom layer through its crop), top-left origin, y down, as the app's
// viewer measures them.
//
// THE OBJECTS, as JSON (one definition; the app draws the ones it is
// editing the same way, with the same Core Graphics / Core Text calls):
//   {"id", "kind": "line" | "rect" | "ellipse" | "text",
//    "x0", "y0", "x1", "y1"   its two corners (a line: its two ends; a
//                             text: (x0, y0) is its top-left),
//    "stroke": [r, g, b, a]   sRGB, straight alpha, 0..1: a line's, an
//                             outline's, a text's colour,
//    "fill": [r, g, b, a]     a rectangle's or ellipse's inside,
//    "width"                  a line's or outline's, in pixels (0: none),
//    "text", "font": {"family", "size" (pixels), "bold", "italic",
//                     "underline"},
//    "box"                    a text's: true -- a TEXT BOX, (x0, y0) to
//                             (x1, y1)}
// A text BOX wraps its words within its width -- a line per "\n" too --
// and shows the lines that fit WHOLE in its height: the rest are cut at
// a line, never through one (Core Text's framesetter, which sets only
// whole lines in a frame). A text without one (from before) runs from
// its top-left, a line per "\n", each the font's ascent + descent +
// leading tall, unwrapped.
//
// A BRUSH STROKE is round dabs along its points, each solid out to
// (1 - softness) of its radius and fading to nothing at the radius. The
// dabs of one stroke join as their maximum, so a stroke is as opaque as
// one dab wherever it crosses itself; then its colour is laid over the
// raster -- or, erasing, the raster is taken away by as much.

#ifndef VALTZ_MEDIA_MARKUP_H
#define VALTZ_MEDIA_MARKUP_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/media/format.h"
#include "valtz/media/layers.h"

#include <array>
#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace valtz::media {

struct Stroke {
  std::vector<std::array<double, 2>> points;  // canvas pixels
  double                radius = 8;            // pixels
  double                softness = 0;          // 0 hard .. 1 all fade
  std::array<double, 4> color{0, 0, 0, 1};     // sRGB, straight alpha
  bool                  erase = false;
};

// {"points": [[x, y], ...], "radius", "softness", "color": [r, g, b, a],
//  "erase"}; tolerant -- values held to their ranges.
Stroke stroke_from_json(const Json&);

// The objects as they are kept: known kinds only, every field there and
// in range, an id each (a missing or repeated one is made).
Json normalize_objects(const Json&);

// `raster` (a PNG; empty: a clear one) with `s` painted on it -- or
// erased from it -- written to `out` as an 8-bit sRGB PNG with alpha, the
// canvas's size (a raster of another size is centred on it, as the stack
// would place it).
Status paint_stroke(const std::filesystem::path& raster, PixelSize canvas,
                    const Stroke& s, const std::filesystem::path& out);

// A markup layer's picture: its raster (empty: none) with its objects
// drawn over it, those in `hidden` left out (the app draws the ones it is
// editing), the canvas's size, to `out` as an 8-bit sRGB PNG.
Status render_markup(const std::filesystem::path& raster, PixelSize canvas,
                     const Json& objects,
                     const std::set<std::string>& hidden,
                     const std::filesystem::path& out);

// The same, drawn in memory for a stack to take (LayerPicture::drawn):
// no file. Objects and raster empty: a clear canvas.
Result<DrawnPicture> render_markup_picture(
    const std::filesystem::path& raster, PixelSize canvas,
    const Json& objects, const std::set<std::string>& hidden);

}

#endif
