#include "valtz/media/pdf.h"

#include "valtz/media/layers.h"
#include "valtz/media/markup.h"

#include <CoreGraphics/CoreGraphics.h>
#include <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

using Kind = PdfLayer::Kind;

// Stacks within stacks, at most (a composition's own nesting is held
// lower; this only stops a document made by hand).
constexpr int kMostDepth = 32;
// A picture kept at this many pixels a pixel it is shown at (at most its
// own); a mask drawn at this many a page's.
constexpr double kPictureScale = 2;
constexpr double kMaskScale = 2;
constexpr double kMostMaskPixels = 32.0 * 1024 * 1024;

std::string_view
kind_name(Kind k)
{
  switch (k) {
  case Kind::Picture: return "picture";
  case Kind::Markup: return "markup";
  case Kind::Stack: return "stack";
  case Kind::Empty: break;
  }
  return "empty";
}

Kind
kind_of(std::string_view s)
{
  return s == "picture" ? Kind::Picture
         : s == "markup" ? Kind::Markup
         : s == "stack"  ? Kind::Stack
                         : Kind::Empty;
}

Json
layer_json(const PdfLayer& l)
{
  Json j = {{"kind", kind_name(l.kind)}};
  if (!l.file.empty()) {
    j["file"] = l.file.string();
  }
  if (l.kind == Kind::Markup) {
    j["width"] = l.size.width;
    j["height"] = l.size.height;
    j["objects"] = l.objects;
  }
  if (l.kind == Kind::Stack) {
    Json ls = Json::array();
    for (const auto& c : l.layers) {
      ls.push_back(layer_json(c));
    }
    j["layers"] = std::move(ls);
    j["canvas"] = to_json(l.canvas);
  }
  if (!l.adjust.identity()) {
    j["adjust"] = to_json(l.adjust);
  }
  if (!l.crop.identity()) {
    j["crop"] = to_json(l.crop);
  }
  if (!l.visible) {
    j["visible"] = false;
  }
  if (l.mask) {
    j["mask"] = true;
  }
  return j;
}

PdfLayer
layer_from_json(const Json& j, int depth)
{
  PdfLayer l;
  if (!j.is_object() || depth > kMostDepth) {
    return l;
  }
  l.kind = kind_of(jget<std::string>(j, "kind", ""));
  l.file = jget<std::string>(j, "file", "");
  l.size = {jget<std::int32_t>(j, "width", 0),
            jget<std::int32_t>(j, "height", 0)};
  l.objects = jget(j, "objects", Json::array());
  for (const auto& c : jget(j, "layers", Json::array())) {
    l.layers.push_back(layer_from_json(c, depth + 1));
  }
  l.canvas = stack_canvas_from_json(jget(j, "canvas", Json::object()));
  l.adjust = adjustments_from_json(jget(j, "adjust", Json::object()));
  l.crop = crop_from_json(jget(j, "crop", Json::object()));
  l.visible = jget(j, "visible", true);
  l.mask = jget(j, "mask", false);
  return l;
}

void
collect_files(const PdfLayer& l, std::vector<fs::path>& out)
{
  if (!l.file.empty()) {
    out.push_back(l.file);
  }
  if (l.kind == Kind::Markup && l.objects.is_array()) {
    for (const auto& o : l.objects) {
      if (const auto f = jget<std::string>(o, "file", ""); !f.empty()) {
        out.push_back(f);
      }
    }
  }
  for (const auto& c : l.layers) {
    collect_files(c, out);
  }
}

// The context's graphics state saved for a scope, and put back as it
// ends -- an error's early return too.
class Saved {
public:
  explicit Saved(CGContextRef ctx) : _ctx(ctx) { CGContextSaveGState(ctx); }
  ~Saved() { CGContextRestoreGState(_ctx); }
  Saved(const Saved&) = delete;
  Saved& operator=(const Saved&) = delete;

private:
  CGContextRef _ctx;
};

// How many of the context's device pixels (a PDF's: points) a unit of
// its user space covers.
double
device_scale(CGContextRef ctx)
{
  const CGAffineTransform t = CGContextGetUserSpaceToDeviceSpaceTransform(ctx);
  return std::sqrt(std::abs(t.a * t.d - t.b * t.c));
}

// An sRGB, 8-bit, premultiplied RGBA bitmap, clear.
CGContextRef
bitmap(std::size_t w, std::size_t h)
{
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(nullptr, w, h, 8, 0, srgb,
                                           kCGImageAlphaPremultipliedLast);
  CGColorSpaceRelease(srgb);
  if (ctx) {
    CGContextClearRect(ctx, CGRectMake(0, 0, w, h));
  }
  return ctx;
}

Result<PixelSize> stack_extent(const PdfLayer& st, int depth);

// What a layer shows, at its own size before it is placed: 0 x 0 with
// nothing on it.
Result<PixelSize>
content_size(const PdfLayer& l, int depth)
{
  switch (l.kind) {
  case Kind::Picture:
    return look_size(l.file, {});
  case Kind::Markup:
    return l.size.width > 0 && l.size.height > 0 ? l.size : PixelSize{};
  case Kind::Stack:
    return stack_extent(l, depth + 1);
  case Kind::Empty:
    break;
  }
  return PixelSize{};
}

// A stack's FRAME -- its own, or its bottom layer through its crop -- and
// what is SEEN of it, its canvas: in the frame's coordinates, y up from
// its bottom left (compose_images' own).
struct Geometry {
  bool   framed = false;
  CGSize frame = CGSizeZero;
  CGRect seen = CGRectZero;
};

Result<Geometry>
geometry(const PdfLayer& st, PixelSize bottom)
{
  const StackCanvas& sc = st.canvas;
  Geometry g;
  g.framed = sc.framed();
  if (g.framed) {
    g.frame = CGSizeMake(sc.frame_w, sc.frame_h);
  } else {
    if (st.layers.empty() || bottom.width <= 0 || bottom.height <= 0) {
      return make_error(Code::InvalidArgument,
                        "a layer stack needs a picture at the bottom");
    }
    const Crop& c = st.layers.front().crop;
    if (!c.identity()) {
      const CropPlacement p =
          crop_placement(c, bottom.width, bottom.height);
      g.frame = CGSizeMake(p.canvas_w, p.canvas_h);
    } else {
      g.frame = CGSizeMake(bottom.width, bottom.height);
    }
  }
  g.seen = sc.set() ? CGRectMake(-sc.x, g.frame.height + sc.y - sc.height,
                                 sc.width, sc.height)
                    : CGRectMake(0, 0, g.frame.width, g.frame.height);
  return g;
}

Result<PixelSize>
stack_extent(const PdfLayer& st, int depth)
{
  if (depth > kMostDepth) {
    return make_error(Code::InvalidArgument, "the stacks nest too deep");
  }
  PixelSize bottom;
  if (!st.canvas.framed() && !st.layers.empty()) {
    VALTZ_ASSIGN(bottom, content_size(st.layers.front(), depth));
  }
  VALTZ_ASSIGN(Geometry g, geometry(st, bottom));
  return PixelSize{
      static_cast<std::int32_t>(std::lround(g.seen.size.width)),
      static_cast<std::int32_t>(std::lround(g.seen.size.height))};
}

// A layer READY to draw: its size, and -- where it is drawn as pixels
// whatever it is (a markup's or a composition's look) -- those.
struct Ready {
  PixelSize    size;
  DrawnPicture pixels;

  bool empty() const { return size.width <= 0 || size.height <= 0; }
};

Status draw_stack(CGContextRef ctx, const PdfLayer& st, bool vectors,
                  int depth);

Result<Ready>
ready(const PdfLayer& l, int depth)
{
  Ready r;
  VALTZ_ASSIGN(r.size, content_size(l, depth));
  if (r.empty() || l.adjust.identity() ||
      (l.kind != Kind::Markup && l.kind != Kind::Stack)) {
    return r;
  }
  // A LOOK on vectors: they are drawn into pixels, and those adjusted.
  DrawnPicture drawn;
  if (l.kind == Kind::Markup) {
    VALTZ_ASSIGN(drawn, render_markup_picture(l.file, l.size, l.objects,
                                              {}));
  } else {
    CGContextRef bm = bitmap(static_cast<std::size_t>(r.size.width),
                             static_cast<std::size_t>(r.size.height));
    if (!bm) {
      return make_error(Code::Internal, "cannot draw the composition");
    }
    Status st = draw_stack(bm, l, false, depth + 1);
    CGImageRef img = st.ok() ? CGBitmapContextCreateImage(bm) : nullptr;
    CGContextRelease(bm);
    VALTZ_TRY(st);
    if (!img) {
      return make_error(Code::Internal, "cannot draw the composition");
    }
    drawn = DrawnPicture(img, CGImageRelease);
  }
  VALTZ_ASSIGN(r.pixels, look_picture({}, drawn, l.adjust));
  return r;
}

// `l` placed on its stack's frame: through its crop, or centred at its
// own size (compose_images' `placed`).
void
place(CGContextRef ctx, const PdfLayer& l, const Ready& r, CGSize frame)
{
  if (!l.crop.identity()) {
    Crop c = l.crop;
    if (c.canvas.width == 0 || c.canvas.height == 0) {
      c.canvas = {static_cast<std::int32_t>(frame.width),
                  static_cast<std::int32_t>(frame.height)};
    }
    if (c.content.width == 0) {
      c.content = r.size;
    }
    const Affine m =
        crop_placement(c, r.size.width, r.size.height).transform;
    CGContextConcatCTM(ctx, CGAffineTransformMake(m.a, m.b, m.c, m.d,
                                                  m.tx, m.ty));
  } else {
    CGContextTranslateCTM(ctx, (frame.width - r.size.width) / 2,
                          (frame.height - r.size.height) / 2);
  }
}

void
draw_image(CGContextRef ctx, CGImageRef img, PixelSize size)
{
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, CGRectMake(0, 0, size.width, size.height), img);
}

// What `l` shows, unplaced: from (0, 0) to its size, y up.
Status
draw_content(CGContextRef ctx, const PdfLayer& l, const Ready& r,
             bool vectors, int depth)
{
  if (r.pixels) {
    draw_image(ctx, r.pixels.get(), r.size);
    return ok_status();
  }
  switch (l.kind) {
  case Kind::Picture: {
    // As many pixels as it is shown at, twice -- never more than its own.
    double scale = kPictureScale * device_scale(ctx);
    scale = scale >= 0.9 ? 1 : std::max(scale, 1e-3);
    VALTZ_ASSIGN(DrawnPicture pic, look_picture(l.file, {}, l.adjust,
                                                scale));
    if (pic) {
      draw_image(ctx, pic.get(), r.size);
    }
    return ok_status();
  }
  case Kind::Markup:
    return draw_markup(ctx, l.file, l.size, l.objects, {}, vectors);
  case Kind::Stack:
    return draw_stack(ctx, l, vectors, depth + 1);
  case Kind::Empty:
    break;
  }
  return ok_status();
}

// A MASK as a PDF takes it: the brightness of mask layer `m` -- placed on
// the frame, its sRGB luma times its alpha, as compose_images' `masked`
// reads it -- over what is seen, kMaskScale a device pixel; null when
// the mask has nothing on it (it then shows nothing). SOFT where it has
// half-tones other than its edges' -- a gradient, a soft brush: a PDF
// masks each mark drawn under it on its own, so there, of two marks one
// over the other, the lower shows through the upper (a group masked
// whole would not: Quartz blends a group in Generic RGB, sRGB's blue
// at half coming back 129, 153, 255). Where a mask is all on or off the
// two are the same.
struct Mask {
  CGImageRef image = nullptr;
  bool       soft = false;

  Mask() = default;
  Mask(const Mask&) = delete;
  Mask& operator=(const Mask&) = delete;
  ~Mask()
  {
    if (image) {
      CGImageRelease(image);
    }
  }
};

// A half-tone of a mask: neither off nor on.
constexpr int kOff = 16;
constexpr int kOn = 239;

// Whether `grey` (w x h) has half-tones away from an edge: a pixel
// neither off nor on with no off AND on pixel within two of it -- more
// of them than a stray few.
bool
soft_mask(const std::vector<std::uint8_t>& grey, std::size_t w,
          std::size_t h)
{
  constexpr std::size_t r = 2;
  // The least and most in each pixel's (2r + 1)-square: a row's, then a
  // column's of those.
  std::vector<std::uint8_t> lo(grey.size()), hi(grey.size());
  for (std::size_t y = 0; y < h; ++y) {
    for (std::size_t x = 0; x < w; ++x) {
      std::uint8_t a = 255, b = 0;
      for (std::size_t i = x >= r ? x - r : 0; i <= std::min(w - 1, x + r);
           ++i) {
        a = std::min(a, grey[y * w + i]);
        b = std::max(b, grey[y * w + i]);
      }
      lo[y * w + x] = a;
      hi[y * w + x] = b;
    }
  }
  std::size_t soft = 0;
  for (std::size_t y = 0; y < h; ++y) {
    for (std::size_t x = 0; x < w; ++x) {
      const int v = grey[y * w + x];
      if (v <= kOff || v >= kOn) {
        continue;
      }
      std::uint8_t a = 255, b = 0;
      for (std::size_t j = y >= r ? y - r : 0; j <= std::min(h - 1, y + r);
           ++j) {
        a = std::min(a, lo[j * w + x]);
        b = std::max(b, hi[j * w + x]);
      }
      soft += !(a <= kOff && b >= kOn);
    }
  }
  return soft > std::max<std::size_t>(8, w * h / 2000);
}

Status
make_mask(CGContextRef ctx, const PdfLayer& m, const Ready& r,
          const Geometry& g, int depth, Mask& out)
{
  if (r.empty()) {
    return ok_status();
  }
  double k = kMaskScale * device_scale(ctx);
  const double area = g.seen.size.width * g.seen.size.height * k * k;
  if (area > kMostMaskPixels) {
    k *= std::sqrt(kMostMaskPixels / area);
  }
  const auto w = static_cast<std::size_t>(
      std::max(1.0, std::ceil(g.seen.size.width * k)));
  const auto h = static_cast<std::size_t>(
      std::max(1.0, std::ceil(g.seen.size.height * k)));
  CGContextRef bm = bitmap(w, h);
  if (!bm) {
    return make_error(Code::Internal, "cannot draw the mask");
  }
  CGContextScaleCTM(bm, w / g.seen.size.width, h / g.seen.size.height);
  CGContextTranslateCTM(bm, -g.seen.origin.x, -g.seen.origin.y);
  place(bm, m, r, g.frame);
  if (Status st = draw_content(bm, m, r, false, depth); !st.ok()) {
    CGContextRelease(bm);
    return st;
  }
  // Each pixel over black in linear light, back to sRGB, its luma.
  auto decode = [](double v) {
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  };
  auto encode = [](double v) {
    return v <= 0.0031308 ? v * 12.92
                          : 1.055 * std::pow(v, 1 / 2.4) - 0.055;
  };
  const auto* px = static_cast<const std::uint8_t*>(
      CGBitmapContextGetData(bm));
  const std::size_t row = CGBitmapContextGetBytesPerRow(bm);
  std::vector<std::uint8_t> grey(w * h);
  constexpr double kLuma[3] = {0.2126, 0.7152, 0.0722};
  for (std::size_t y = 0; y < h; ++y) {
    const std::uint8_t* p = px + y * row;
    for (std::size_t x = 0; x < w; ++x, p += 4) {
      const double a = p[3] / 255.0;
      double luma = 0;
      if (a > 0) {
        for (int c = 0; c < 3; ++c) {
          const double s = std::min(1.0, p[c] / 255.0 / a);
          luma += kLuma[c] * encode(a * decode(s));
        }
      }
      grey[y * w + x] = static_cast<std::uint8_t>(
          std::lround(std::clamp(luma, 0.0, 1.0) * 255));
    }
  }
  CGContextRelease(bm);
  out.soft = soft_mask(grey, w, h);
  CFDataRef data = CFDataCreate(nullptr, grey.data(),
                                static_cast<CFIndex>(grey.size()));
  CGDataProviderRef dp = CGDataProviderCreateWithCFData(data);
  CFRelease(data);
  CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
  out.image = CGImageCreate(w, h, 8, 8, w, gray, kCGImageAlphaNone, dp,
                            nullptr, true, kCGRenderingIntentDefault);
  CGColorSpaceRelease(gray);
  CGDataProviderRelease(dp);
  if (!out.image) {
    return make_error(Code::Internal, "cannot make the mask");
  }
  return ok_status();
}

// The context clipped to mask layer `m` (a mask with nothing on it shows
// nothing); whether it is SOFT (make_mask).
Result<bool>
clip_to_mask(CGContextRef ctx, const PdfLayer& m, const Ready& r,
             const Geometry& g, int depth)
{
  Mask mask;
  VALTZ_TRY(make_mask(ctx, m, r, g, depth, mask));
  if (!mask.image) {
    CGContextClipToRect(ctx, CGRectZero);
    return false;
  }
  CGContextClipToMask(ctx, g.seen, mask.image);
  return mask.soft;
}

// What `l` shows drawn as PIXELS -- kPictureScale a device pixel, a
// bitmap's way (draw_content) -- unplaced, from (0, 0) to its size: a
// markup or a composition under a soft mask, which masks it whole.
Status
draw_as_pixels(CGContextRef ctx, const PdfLayer& l, const Ready& r,
               int depth)
{
  double k = std::max(kPictureScale * device_scale(ctx), 1e-3);
  const double area = static_cast<double>(r.size.width) * r.size.height *
                      k * k;
  if (area > kMostMaskPixels) {
    k *= std::sqrt(kMostMaskPixels / area);
  }
  const auto w = static_cast<std::size_t>(
      std::max(1.0, std::ceil(r.size.width * k)));
  const auto h = static_cast<std::size_t>(
      std::max(1.0, std::ceil(r.size.height * k)));
  CGContextRef bm = bitmap(w, h);
  if (!bm) {
    return make_error(Code::Internal, "cannot draw the layer");
  }
  CGContextScaleCTM(bm, static_cast<double>(w) / r.size.width,
                    static_cast<double>(h) / r.size.height);
  Status st = draw_content(bm, l, r, false, depth);
  CGImageRef img = st.ok() ? CGBitmapContextCreateImage(bm) : nullptr;
  CGContextRelease(bm);
  VALTZ_TRY(st);
  if (img) {
    draw_image(ctx, img, r.size);
    CGImageRelease(img);
  }
  return ok_status();
}

// A layer's content in a stack -- under a SOFT mask in a vector
// context, a markup's or a composition's (marks over one another) as
// pixels; a picture is one image either way.
Status
draw_layer(CGContextRef ctx, const PdfLayer& l, const Ready& r,
           bool vectors, bool soft, int depth)
{
  if (vectors && soft && !r.pixels &&
      (l.kind == Kind::Markup || l.kind == Kind::Stack)) {
    return draw_as_pixels(ctx, l, r, depth);
  }
  return draw_content(ctx, l, r, vectors, depth);
}

// A STACK drawn as compose_images composes one: its canvas from (0, 0)
// to its size in the context's user space, y up.
Status
draw_stack(CGContextRef ctx, const PdfLayer& st, bool vectors, int depth)
{
  if (depth > kMostDepth) {
    return make_error(Code::InvalidArgument, "the stacks nest too deep");
  }
  std::vector<Ready> rs;
  rs.reserve(st.layers.size());
  for (const auto& l : st.layers) {
    VALTZ_ASSIGN(Ready r, ready(l, depth));
    rs.push_back(std::move(r));
  }
  VALTZ_ASSIGN(Geometry g,
               geometry(st, rs.empty() ? PixelSize{} : rs.front().size));
  Saved stack(ctx);
  CGContextTranslateCTM(ctx, -g.seen.origin.x, -g.seen.origin.y);
  CGContextClipToRect(ctx, g.seen);
  const auto& ls = st.layers;
  auto masks_it = [&](std::size_t i) {
    return i + 1 < ls.size() && ls[i + 1].mask && ls[i + 1].visible;
  };
  // The BOTTOM of a stack without a frame of its own is the frame: on
  // its crop's canvas, padded with its colour, cut there.
  if (!g.framed && ls.front().visible) {
    Saved bottom(ctx);
    bool soft = false;
    if (masks_it(0)) {
      VALTZ_ASSIGN(soft, clip_to_mask(ctx, ls[1], rs[1], g, depth));
    }
    const Crop& c = ls.front().crop;
    if (!c.identity()) {
      const CropPlacement p =
          crop_placement(c, rs.front().size.width, rs.front().size.height);
      const CGRect canvas = CGRectMake(0, 0, p.canvas_w, p.canvas_h);
      CGContextClipToRect(ctx, canvas);
      if (c.pad[3] > 0) {
        CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        const CGFloat pad[4] = {c.pad[0], c.pad[1], c.pad[2], c.pad[3]};
        CGColorRef color = CGColorCreate(srgb, pad);
        CGColorSpaceRelease(srgb);
        CGContextSetFillColorWithColor(ctx, color);
        CGColorRelease(color);
        CGContextFillRect(ctx, canvas);
      }
      const Affine& m = p.transform;
      CGContextConcatCTM(ctx, CGAffineTransformMake(m.a, m.b, m.c, m.d,
                                                    m.tx, m.ty));
    }
    VALTZ_TRY(draw_layer(ctx, ls.front(), rs.front(), vectors, soft, depth));
  }
  for (std::size_t i = g.framed ? 0 : 1; i < ls.size(); ++i) {
    const PdfLayer& l = ls[i];
    if (rs[i].empty() || !l.visible || l.mask) {
      continue;
    }
    Saved layer(ctx);
    bool soft = false;
    if (masks_it(i)) {
      VALTZ_ASSIGN(soft, clip_to_mask(ctx, ls[i + 1], rs[i + 1], g, depth));
    }
    place(ctx, l, rs[i], g.frame);
    VALTZ_TRY(draw_layer(ctx, l, rs[i], vectors, soft, depth));
  }
  return ok_status();
}

}

Json
to_json(const PdfDocument& doc)
{
  Json pages = Json::array();
  for (const auto& p : doc.pages) {
    pages.push_back(layer_json(p));
  }
  return {{"title", doc.title}, {"pages", std::move(pages)}};
}

PdfDocument
pdf_document_from_json(const Json& j)
{
  PdfDocument doc;
  doc.title = jget<std::string>(j, "title", "");
  for (const auto& p : jget(j, "pages", Json::array())) {
    doc.pages.push_back(layer_from_json(p, 0));
  }
  return doc;
}

std::vector<fs::path>
files_of(const PdfDocument& doc)
{
  std::vector<fs::path> out;
  for (const auto& p : doc.pages) {
    collect_files(p, out);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

Status
write_pdf(const PdfDocument& doc, const fs::path& out,
          const std::function<bool(int, int)>& page_done)
{
  if (doc.pages.empty()) {
    return make_error(Code::InvalidArgument, "a PDF needs a page");
  }
  @autoreleasepool {
    std::vector<CGRect> boxes;
    for (const auto& page : doc.pages) {
      VALTZ_ASSIGN(PixelSize s, stack_extent(page, 0));
      if (s.width <= 0 || s.height <= 0) {
        return make_error(Code::InvalidArgument, "a page has no size");
      }
      boxes.push_back(CGRectMake(0, 0, s.width, s.height));
    }
    std::error_code ec;
    if (out.has_parent_path()) {
      fs::create_directories(out.parent_path(), ec);
    }
    NSMutableDictionary* info = [NSMutableDictionary dictionary];
    info[(__bridge NSString*)kCGPDFContextCreator] = @"Valtz";
    if (!doc.title.empty()) {
      info[(__bridge NSString*)kCGPDFContextTitle] =
          [[NSString alloc] initWithBytes:doc.title.data()
                                   length:doc.title.size()
                                 encoding:NSUTF8StringEncoding] ?: @"";
    }
    NSURL* url = [NSURL fileURLWithPath:@(out.c_str())];
    CGContextRef ctx = CGPDFContextCreateWithURL(
        (__bridge CFURLRef)url, &boxes.front(),
        (__bridge CFDictionaryRef)info);
    if (!ctx) {
      return make_error(Code::Io,
                        std::format("cannot write {}", out.string()));
    }
    Status st = ok_status();
    const int n = static_cast<int>(doc.pages.size());
    for (int i = 0; i < n && st.ok(); ++i) {
      @autoreleasepool {
        const CGRect box = boxes[static_cast<std::size_t>(i)];
        NSDictionary* page = @{
          (__bridge NSString*)kCGPDFContextMediaBox :
              [NSData dataWithBytes:&box length:sizeof box],
        };
        CGPDFContextBeginPage(ctx, (__bridge CFDictionaryRef)page);
        st = draw_stack(ctx, doc.pages[static_cast<std::size_t>(i)], true,
                        0);
        CGPDFContextEndPage(ctx);
      }
      if (st.ok() && page_done && !page_done(i + 1, n)) {
        st = make_error(Code::Cancelled, "stopped");
      }
    }
    CGPDFContextClose(ctx);
    CGContextRelease(ctx);
    if (!st.ok()) {
      fs::remove(out, ec);
    }
    return st;
  }
}

}
