// A STILL written as a PDF, its VECTORS kept (export-media's "pdf",
// 2026-10-14).
//
// Each page is a layer stack drawn into the page as layers.h draws it
// into pixels -- the same canvas, frame, placement, masks and order --
// but what is drawn as vectors stays vectors: a markup's lines, shapes,
// polygons and outlines as PDF paths, its gradients as shadings, its
// words as text in their fonts (Core Text into Core Graphics' PDF
// context); a still composition a layer shows is drawn the same way, its
// own layers in its place. Only pixels are images: pictures, a clip's
// frame, a markup's raster -- brush strokes are pixels from the moment
// they are painted -- and its drawings. A still with pages is one PDF of
// as many pages.
//
// A canvas pixel is a POINT (1/72 inch), as design tools write a page:
// a 1920 x 1080 still is a 26.7 x 15 inch page, and its vectors are as
// sharp at any size it is printed or viewed at. A picture is kept at
// twice the pixels it is shown at, at most its own.
//
// What a PDF cannot take as a bitmap draws it is drawn as near as it
// can be (markup.h draw_markup): a gradient in words fills their glyphs'
// outlines; an object's shadows and glows are pixels beneath the object.
// And:
// * a LOOK (adjustments) on a markup or a composition: its pixels,
//   adjusted, at the canvas's -- as a flattened picture has them;
// * a MASK: its brightness a soft mask (pixels, at twice the page's)
//   over what it masks, which stays vectors.
// Colours are sRGB, as a markup's are; pictures 8-bit sRGB. Layers are
// laid over one another as a PDF viewer blends them -- in sRGB, not the
// linear light a flattened picture is composed in -- so a translucent
// edge over another layer is a shade off the PNG's.

#ifndef VALTZ_MEDIA_PDF_H
#define VALTZ_MEDIA_PDF_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/media/adjust.h"
#include "valtz/media/crop.h"
#include "valtz/media/format.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace valtz::media {

// One layer of a stack, as layers.h lays it (LayerPicture), and what it
// shows -- as data, so an export job carries it.
struct PdfLayer {
  enum class Kind { Empty, Picture, Markup, Stack };
  Kind                  kind = Kind::Empty;
  // A PICTURE's file (pixels: a photo, a clip's frame, a rendering); a
  // MARKUP's raster (empty: none).
  std::filesystem::path file;
  // A MARKUP: its canvas, and its objects (markup.h; a drawing's "file"
  // set).
  PixelSize             size;
  Json                  objects = Json::array();
  // A STACK -- a page, or a still composition a layer shows -- its
  // layers, bottom first, on its canvas.
  std::vector<PdfLayer> layers;
  StackCanvas           canvas;
  // Its look and place in the stack it is in.
  Adjustments           adjust;
  Crop                  crop;
  bool                  visible = true;
  bool                  mask = false;
};

struct PdfDocument {
  std::string           title;
  std::vector<PdfLayer> pages;  // each a STACK
};

Json to_json(const PdfDocument&);
PdfDocument pdf_document_from_json(const Json&);

// Every file it reads: pictures, markup rasters, drawings.
std::vector<std::filesystem::path> files_of(const PdfDocument&);

// `doc` written to `out`, a page at a time. `page_done` (pages from 1,
// of all) after each stops it by answering false: nothing is left at
// `out`, and Code::Cancelled.
Status write_pdf(const PdfDocument& doc, const std::filesystem::path& out,
                 const std::function<bool(int, int)>& page_done = {});

}

#endif
