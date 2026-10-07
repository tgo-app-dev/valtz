// Crop and rotate, and a clip's trim -- the Crop panel -- as DATA.
//
// Both are IMAGE MODIFIERS (project::Modifier, kinds "crop" and "trim"):
// recorded on the asset, never baked into its file, and made real only
// where pixels leave Valtz -- when a picture is decoded for vpipe (an
// edit's base, a clip's first frame: media/model-input.h) or an asset is
// exported. The stage shows them live; the definition is here, once, so
// what the stage shows is what the model gets:
//
//   CROP. The picture -- its CONTENT -- lies on a CANVAS (by default its
//   own size): scaled (each axis on its own, as a share of the canvas),
//   then turned about its centre, its centre offset from the canvas's.
//   Where it does not reach, the canvas is filled with the background
//   (padding) colour. Zooming in crops; zooming out pads.
//
//   TRIM. A clip's mark-in and mark-out, as frame numbers (both kept).
//
// Params are flat numbers ("scale_x": 1.2, "pad_a": 0.5), so any reader --
// the app's included -- can take a modifier without knowing its kind.

#ifndef VALTZ_MEDIA_CROP_H
#define VALTZ_MEDIA_CROP_H

#include "valtz/base/json.h"
#include "valtz/base/rational.h"
#include "valtz/media/format.h"

#include <array>
#include <cstdint>

namespace valtz::media {

// A rotation's range: a whole turn either way. Not folded into one turn,
// so a clip keyed 0 at its start and 360 at its end turns all the way
// round.
inline constexpr double kMaxTurn = 360.0;

struct Crop {
  // The picture's size (as it displays) when it was cropped. A decode at
  // another resolution (a RAW developed smaller, a feed capped) scales
  // the canvas with it.
  PixelSize content;
  // The frame the result fills, in the same pixels; the content's size
  // when 0.
  PixelSize canvas;
  // Canvas pixels per content pixel, across and down: with the canvas
  // the picture's own size, the share of the frame it covers.
  double    scale_x = 1;
  double    scale_y = 1;
  double    offset_x = 0;  // the content's centre from the canvas's, in
  double    offset_y = 0;  //   canvas widths / heights (+ right, + down)
  double    rotate = 0;    // degrees, + clockwise, within ±kMaxTurn
  // What fills the canvas where the content does not: sRGB, straight
  // alpha, 0..1.
  std::array<double, 4> pad{0, 0, 0, 1};

  bool identity() const;
  bool operator==(const Crop&) const = default;
};

// {"content_w", "content_h", "canvas_w", "canvas_h", "scale_x",
//  "scale_y", "offset_x", "offset_y", "rotate", "pad_r", "pad_g", "pad_b",
//  "pad_a"}; {} for the identity. Reading is tolerant: missing keys keep
// their defaults, values are held to sane ranges, and a "scale" (one for
// both axes) is read as both.
Json to_json(const Crop&);
Crop crop_from_json(const Json&);

// A 2-D affine transform as Core Graphics writes it:
// x' = a x + c y + tx, y' = b x + d y + ty.
struct Affine {
  double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0;
};

// The canvas, for the content decoded at `w` x `h` pixels.
struct CropPlacement {
  double  canvas_w = 0;
  double  canvas_h = 0;
  // Content pixels -> canvas pixels, in Core Image's frame: origin at
  // the bottom left, y up, the content's extent starting at (0, 0).
  Affine  transform;
};
CropPlacement crop_placement(const Crop&, double w, double h);

// A picture's or a clip's CANVAS when it was resized (Canvas Size): the
// frame the whole stack is shown in, `width` x `height` pixels, with the
// stack's OWN frame at (x, y) (its top-left; it may lie partly off).
// Where the stack does not reach, the canvas is clear. Unset (0 x 0): the
// stack's own frame is the canvas.
//
// The OWN frame is what the layers lie on. A picture's own stack has its
// own image at the bottom, and that layer through its crop is the frame
// (unframed). The PROJECT's composition keeps a frame of its own,
// `frame_w` x `frame_h` (framed): set by the first asset placed in it,
// it no longer depends on any layer -- layer 0 lies on it as every layer
// does, through its crop or centred at its own size, and a new take at
// another size leaves the canvas, and the other layers, where they are.
struct StackCanvas {
  std::int32_t width = 0;
  std::int32_t height = 0;
  double       x = 0;
  double       y = 0;
  std::int32_t frame_w = 0;
  std::int32_t frame_h = 0;

  bool set() const { return width > 0 && height > 0; }
  bool framed() const { return frame_w > 0 && frame_h > 0; }
  PixelSize frame() const { return {frame_w, frame_h}; }
  bool operator==(const StackCanvas&) const = default;
};

// The canvas resized to `size`, ANCHORED: what lies at the anchor (0, 0.5
// or 1 across and down: left / centre / right, top / middle / bottom)
// stays where it is, the rest grows or shrinks away from it -- as
// Canvas Size does in an image editor. `own` is the stack's own frame;
// resizing again moves on from where the last left the stack. Back to
// the own frame, at its own place: unset (a frame kept).
StackCanvas resize_canvas(const StackCanvas& now, PixelSize own,
                          PixelSize size, double anchor_x, double anchor_y);

// {"w", "h", "x", "y"} (0s unset), {"fw", "fh"} framed; {} neither.
Json to_json(const StackCanvas&);
StackCanvas stack_canvas_from_json(const Json&);

// A clip's marks, as frames of the clip (0 is the first); -1 is unset.
// Both are kept, so the trimmed clip runs from `in` through `out`.
struct Trim {
  std::int64_t in = -1;
  std::int64_t out = -1;
  Rational     rate{0, 1};  // the clip's frame rate when it was marked

  bool identity() const { return in < 0 && out < 0; }
  bool operator==(const Trim&) const = default;
};

// {"in", "out", "rate_num", "rate_den"}; {} for the identity.
Json to_json(const Trim&);
Trim trim_from_json(const Json&);

}

#endif
