// A picture's LAYER STACK made into one picture (project::Layer).
//
// The stack is data -- which pictures, in which order, each with its own
// adjustments and crop -- and pixels are made of it only where they leave
// Valtz: an export, a flattened picture dragged out of the stage. The
// app's stage composes the same way (the core's adjustment chain and crop
// placement), so what is shown is what is made.
//
// THE CANVAS of a picture's own stack is its bottom layer's (layer 0, its
// own image): its picture through its adjustments and crop, padded with
// its pad colour. Each layer above is drawn through its adjustments and
// placed by its crop -- on that canvas -- or, with no crop, centred at
// its own size; where it does not reach, the layers below show (an upper
// layer's padding is always clear). A hidden layer draws nothing; a
// hidden bottom one still sets the canvas. The PROJECT's composition has
// a frame of its own (crop.h StackCanvas::framed): that is the canvas,
// and every layer -- layer 0 too -- is placed on it as an upper one is.
//
// A MASK layer draws nothing either: the layer beneath it is shown only
// where the mask is bright -- its brightness (sRGB luma, as it looks)
// times its alpha, each pixel -- after that layer's own look and
// placement, and the mask's. Hidden, a mask masks nothing.

#ifndef VALTZ_MEDIA_LAYERS_H
#define VALTZ_MEDIA_LAYERS_H

#include "valtz/base/result.h"
#include "valtz/media/adjust.h"
#include "valtz/media/crop.h"

#include <CoreGraphics/CGImage.h>
#include <IOSurface/IOSurfaceRef.h>

#include <filesystem>
#include <memory>
#include <vector>

namespace valtz::media {

// A picture drawn in memory (Core Graphics) -- markup objects, a clear
// canvas -- that a stack takes as it is, with no file written and read
// back. Shared; released with its last holder.
using DrawnPicture = std::shared_ptr<CGImage>;

struct LayerPicture {
  std::filesystem::path file;  // empty: a layer with nothing on it yet
  DrawnPicture          drawn;  // in place of `file`, when set
  Adjustments           adjust;
  Crop                  crop;
  // Hidden: drawn as nothing. (The bottom one still sets the canvas.)
  bool                  visible = true;
  // It masks the layer beneath it, and is not drawn.
  bool                  mask = false;
};

// `layers`, bottom first, drawn into one picture and written to `out` as
// a 16-bit sRGB PNG with alpha. The bottom layer must have a picture.
// `canvas`, when set, is the frame it is shown in: the stack's own frame
// placed on it (crop.h StackCanvas), clear where it does not reach.
Status flatten_layers(const std::vector<LayerPicture>& layers,
                      const std::filesystem::path& out,
                      const StackCanvas& canvas = {});

// The same, for the SCREEN: drawn by the GPU into an 8-bit sRGB surface
// (premultiplied BGRA, tagged with its colour space) that a layer can
// show as it is -- no PNG encoded, written, read and decoded each time
// the stage composes the stack. Returned retained: the caller releases
// it.
Result<IOSurfaceRef> flatten_layers_surface(
    const std::vector<LayerPicture>& layers,
    const StackCanvas& canvas = {});

}

#endif
