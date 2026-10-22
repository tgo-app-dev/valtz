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
//   {"id", "kind": "line" | "rect" | "ellipse" | "polygon" | "text",
//    "x0", "y0", "x1", "y1"   its two corners (a line: its two ends; a
//                             text: (x0, y0) is its top-left),
//    "points": [[u, v], ...]  a POLYGON's corners, closed, in its box's
//                             units -- 0 at (x0, y0), 1 at (x1, y1) -- so
//                             they move and scale with the box,
//    "smooth": [bool, ...]    a PATH (2026-10-15): a polygon whose points
//                             marked true are SMOOTH -- the curve passes
//                             through them without a turn (a curvature
//                             pen's); the rest corners (absent: all),
//    "open": true             a path not closed: a curve, a line without
//                             an inside -- its ends and turns round,
//                             neither fill nor gradient drawn,
//    "gradient": {"kind": "linear" | "radial", "stops": [{"at" 0..1,
//                 "color"}, ...] (two or more), "from": [u, v], "to":
//                 [u, v]}     a shape's inside -- or a text box's words --
//                             in place of its flat colour: a linear one
//                             from FROM to TO, a radial one round FROM
//                             out to TO, both in the box's units (none:
//                             across the middle, left to right; round the
//                             middle out to the right side's), the end
//                             colours carried on past them (2026-10-11),
//    "stroke": [r, g, b, a]   sRGB, straight alpha, 0..1: a line's, an
//                             outline's, a text's colour,
//    "fill": [r, g, b, a]     a rectangle's or ellipse's inside,
//    "width"                  a line's or outline's, in pixels (0: none),
//    "dash": "dashed" | "dotted"  a line's or outline's DASHES (2026-10-21;
//                             absent: solid), in its own widths -- dashes
//                             three long and two apart, square-ended; round
//                             dots two apart -- so they scale with it,
//    "arrows": "start" | "end" | "both"  a LINE's ARROWHEADS: filled in its
//                             colour at that end, its line stopped inside
//                             the head (its cap and dashes do not show past
//                             it), four widths long and four wide (at
//                             least 10 and 10 pixels),
//    "text", "font": {"family", "size" (pixels), "bold", "italic",
//                     "underline"},
//    "box"                    a text's: true -- a TEXT BOX, (x0, y0) to
//                             (x1, y1),
//    a text box's PARAGRAPH style (absent: as before 2026-10-11):
//    "align"                  "left" | "center" | "right" | "justified",
//    "line_spacing"           its lines this many of the font's line
//                             apart (1 .. 3; 1 by default),
//    "paragraph_spacing"      pixels after each paragraph ("\n"),
//    "justify_last"           justified, its LAST line stretched too: a
//                             paragraph running on in another box (a
//                             page's), cut mid-way,
//    "rotate"                 degrees, + clockwise, within half a turn
//                             either way (2026-10-21; absent: 0), and
//    "flip_x", "flip_y"       true: MIRRORED across / down -- both about
//                             its box's CENTRE (a line's ends', a
//                             polygon's points', a text without a box its
//                             lines'): it is drawn in its own pixels --
//                             corners, points, gradient, words -- flipped,
//                             then turned (Information's rotation and its
//                             negative width and height). Any kind,
//                             a drawing lifted too,
//    "effects": [...]         what is drawn with it (2026-10-11), in
//                             order: {"kind": "shadow", "color", "blur",
//                             "dx", "dy"} -- a drop shadow blurred `blur`
//                             pixels, `dx` right and `dy` down -- and
//                             {"kind": "glow", "color", "radius"} -- a
//                             halo `radius` pixels out; any kind
//                             unknown kept, not drawn}
//   {"id", "kind": "drawing", "x0", "y0", "x1", "y1", "image"}
//                             a DRAWING lifted off the raster (below): its
//                             pixels a PNG blob (`image`, a
//                             project::BlobRef), lying in its box; drawn
//                             from "file", the blob's path, which whoever
//                             holds the project sets to draw it (never
//                             kept)
// A text BOX wraps its words within its width -- a line per "\n" too --
// and shows the lines that fit WHOLE in its height: the rest are cut at
// a line, never through one (Core Text's framesetter, which sets only
// whole lines in a frame). A text without one (from before) runs from
// its top-left, a line per "\n", each the font's ascent + descent +
// leading tall, unwrapped. The extra space of a line spacing lies
// BETWEEN lines (Core Text's line-spacing adjustment), so a box's first
// line is at its top whatever the spacing -- what a page laid out by
// boxes needs.
//
// EFFECTS are Core Graphics shadows, each the shadow of a transparency
// layer the object is drawn in -- the object drawn ONCE (a translucent
// fill is not darkened), each effect around what the ones before it
// made (the first innermost): a shadow one pass, a glow two at no
// offset (one is faint). They are drawn in a BITMAP, whose shadows are
// measured in its pixels, y up: the offset and blur, canvas pixels, are
// turned into those by the context's own transform. A view's context
// measures a shadow otherwise, so the app draws an object with effects
// into a bitmap of its own and lays that in.
//
// A PATH's curve is cubic Beziers from point to point, worked out on the
// canvas (not in the box's units): a smooth point's tangent runs from the
// point before it to the one after, each handle a share of the segment it
// reaches into that puts a circle's arc through points evenly round one
// -- four smooth points round a square are a circle -- by the turn there
// (held to 120 degrees); a corner, and an open path's ends, have none.
// What a path covers is its box with its curve's, which can bulge past
// its points.
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

#include <CoreGraphics/CGContext.h>

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

// `raster` (empty: a clear one) with `drawing` -- another markup's
// raster, a PNG placed as draw_raster places one (centred at its own
// size) -- laid over it `dx`, `dy` canvas pixels off (y down), the
// canvas's size, to `out`: a drawing COPIED onto a markup.
Status paste_drawing(const std::filesystem::path& raster, PixelSize canvas,
                     const std::filesystem::path& drawing, double dx,
                     double dy, const std::filesystem::path& out);

// DRAWINGS (2026-10-13). A raster's painted pixels fall into DRAWINGS:
// each the pixels that touch -- 8-connected, alpha above kDrawingAlpha of
// 255 -- with the faint edge round them (anti-aliasing, a soft brush's
// falloff: fainter pixels up to kDrawingFringe away, reached from it).
// Strokes laid over one another are one drawing; strokes apart, two. A
// drawing LIFTED off its raster is an object of kind "drawing" (above),
// moved as objects are, until it is made pixels again -- laid over the
// raster, where, touching another drawing, the two are one.
inline constexpr int kDrawingAlpha = 8;
inline constexpr int kDrawingFringe = 16;

// Where a drawing lies, canvas pixels; empty: none.
struct DrawingBox {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  bool empty() const { return w <= 0 || h <= 0; }
};

// The drawing of `raster` (placed as draw_raster places one) painted
// within `reach` pixels of (x, y) on `canvas`: where it lies -- empty with
// none there.
Result<DrawingBox> drawing_at(const std::filesystem::path& raster,
                              PixelSize canvas, double x, double y,
                              int reach);

// That drawing LIFTED: its pixels, as they are, into `piece` (a PNG its
// box's size; an empty path writes none) and the raster without them
// into `rest` (the canvas's size; none, none). Its box; empty -- nothing
// written -- with no drawing there.
Result<DrawingBox> lift_drawing(const std::filesystem::path& raster,
                                PixelSize canvas, double x, double y,
                                int reach, const std::filesystem::path& rest,
                                const std::filesystem::path& piece);

// A drawing as copied (a PNG placed as draw_raster places one) cut to its
// painted pixels: `piece` written, and where it lies on `canvas` --
// empty, nothing written, with nothing painted.
Result<DrawingBox> crop_drawing(const std::filesystem::path& png,
                                PixelSize canvas,
                                const std::filesystem::path& piece);

// How a TEXT BOX's words lie in it, as it is drawn: the lines set, whether
// every word is, the height all of them need at its width, and its widest
// line -- what the agent is told, so it can size a box to its words.
// `shown`: the bytes of its text the box sets. With `all_lines`, each
// line its words would take at the box's width, however tall the box:
// where it ends in the text (bytes) and the height a box needs to show
// the lines to it -- where a long text is CUT to flow on (text-flow.h).
struct TextFit {
  bool   fits = true;
  int    lines = 0;
  double needed_height = 0;
  double widest = 0;
  std::size_t shown = 0;
  std::vector<std::size_t> line_ends;
  std::vector<double>      line_heights;
};
TextFit text_fit(const Json& object, bool all_lines = false);

// What an object covers as the stage outlines it, canvas pixels {x, y,
// width, height}: a line's or a shape's box with half its outline round
// it, a text box's box, a text without one its lines as set -- turned
// and flipped as it is, the box round that.
std::array<double, 4> object_bounds(const Json& object);

// An object's own pixels to the canvas: its turn and flips about its
// box's centre ("rotate", "flip_x", "flip_y"); the identity without.
CGAffineTransform object_transform(const Json& object);

// Objects ARRANGED (the markup toolbar's Arrange, 2026-10-11): `ids`
// among `objects` -- a layer's, in drawing order, the last on top -- the
// rest left as they are, by `how`:
//   "to-top" / "to-bottom"  drawn last / first, in their order;
//   "up" / "down"           a place up / down the order, past the next
//                           object not among them (a run moves together);
//   "left" | "center" | "right" | "top" | "middle" | "bottom"
//                           their bounds' edges or middles lined up --
//                           on the edge (or the middle) of their bounds
//                           together; ONE object, on the canvas's;
//   "distribute-h" / "distribute-v"
//                           three or more spaced evenly across (down):
//                           the first and last by their middles stay,
//                           the gaps between the bounds made equal.
// Moved objects move whole (x0, y0, x1, y1; points and a gradient are in
// their box's units). An unknown `how` is refused; ids not found ignored.
Result<Json> arrange_objects(const Json& objects,
                             const std::vector<std::string>& ids,
                             const std::string& how, PixelSize canvas);

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

// The same, drawn on `ctx`, its CTM putting the canvas at (0, 0) to
// (width, height), y up, a unit a pixel: a bitmap's, as above -- or, with
// `vectors`, a PDF page's (pdf.h), which keeps what is drawn as it is
// drawn: the raster and drawings as images, the other objects as paths,
// shadings and text. Two things are drawn otherwise there, as a PDF
// cannot take them as a bitmap does: a text box's words in a GRADIENT are
// their glyphs' outlines, the gradient through them; an object's EFFECTS
// are pixels beneath it (twice the page's, a shadow's soft edge and
// all), the object over them still vectors.
Status draw_markup(CGContextRef ctx, const std::filesystem::path& raster,
                   PixelSize canvas, const Json& objects,
                   const std::set<std::string>& hidden, bool vectors);

}

#endif
