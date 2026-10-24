#include "valtz/media/markup.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <Foundation/Foundation.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <tuple>
#include <vector>

namespace valtz::media {

namespace fs = std::filesystem;

namespace {

double
clamp01(double v)
{
  return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.0;
}

std::array<double, 4>
rgba_of(const Json& j, std::array<double, 4> fallback)
{
  if (!j.is_array() || j.size() != 4) {
    return fallback;
  }
  std::array<double, 4> c{};
  for (std::size_t i = 0; i < 4; ++i) {
    c[i] = j[i].is_number() ? clamp01(j[i].get<double>()) : fallback[i];
  }
  return c;
}

double
finite_or(const Json& j, const char* key, double fallback)
{
  const double v = jget(j, key, fallback);
  return std::isfinite(v) ? v : fallback;
}

// An sRGB, 8-bit, premultiplied RGBA context `size` big, clear.
CGContextRef
rgba_context(PixelSize size)
{
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
      nullptr, static_cast<std::size_t>(size.width),
      static_cast<std::size_t>(size.height), 8, 0, srgb,
      kCGImageAlphaPremultipliedLast);
  CGColorSpaceRelease(srgb);
  if (ctx) {
    CGContextClearRect(ctx, CGRectMake(0, 0, size.width, size.height));
  }
  return ctx;
}

// A picture file read; null when it cannot be (or there is none).
CGImageRef
read_image(const fs::path& file)
{
  if (file.empty()) {
    return nullptr;
  }
  NSURL* url = [NSURL fileURLWithPath:@(file.c_str())];
  CGImageSourceRef src =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
  CGImageRef img =
      src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
  if (src) {
    CFRelease(src);
  }
  return img;
}

// `raster` drawn on `ctx` (`canvas` big), centred at its own size.
Status
draw_raster(CGContextRef ctx, PixelSize canvas, const fs::path& raster)
{
  if (raster.empty()) {
    return ok_status();
  }
  NSURL* url = [NSURL fileURLWithPath:@(raster.c_str())];
  CGImageSourceRef src =
      CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
  CGImageRef img =
      src ? CGImageSourceCreateImageAtIndex(src, 0, nullptr) : nullptr;
  if (src) {
    CFRelease(src);
  }
  if (!img) {
    return make_error(Code::Corrupt,
                      std::format("cannot read {}", raster.string()));
  }
  const double w = static_cast<double>(CGImageGetWidth(img));
  const double h = static_cast<double>(CGImageGetHeight(img));
  CGContextDrawImage(ctx, CGRectMake((canvas.width - w) / 2,
                                     (canvas.height - h) / 2, w, h),
                     img);
  CGImageRelease(img);
  return ok_status();
}

Status
write_image_png(CGImageRef img, const fs::path& out)
{
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  NSURL* url = [NSURL fileURLWithPath:@(out.c_str())];
  CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
      (__bridge CFURLRef)url, CFSTR("public.png"), 1, nullptr);
  bool ok = false;
  if (dst) {
    CGImageDestinationAddImage(dst, img, nullptr);
    ok = CGImageDestinationFinalize(dst);
    CFRelease(dst);
  }
  if (!ok) {
    return make_error(Code::Io, std::format("cannot write {}", out.string()));
  }
  return ok_status();
}

Status
write_png(CGContextRef ctx, const fs::path& out)
{
  CGImageRef img = CGBitmapContextCreateImage(ctx);
  if (!img) {
    return make_error(Code::Internal, "cannot make the markup image");
  }
  Status st = write_image_png(img, out);
  CGImageRelease(img);
  return st;
}

// The font a text object names: its family at its size, bold and italic
// where the family has them (else the nearest it has).
CTFontRef
font_of(const Json& font)
{
  const std::string family =
      jget<std::string>(font, "family", "Helvetica Neue");
  const double size = std::clamp(finite_or(font, "size", 48), 1.0, 2000.0);
  NSDictionary* attrs = @{
    (__bridge NSString*)kCTFontFamilyNameAttribute : @(family.c_str())
  };
  CTFontDescriptorRef d =
      CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
  CTFontRef base = CTFontCreateWithFontDescriptor(d, size, nullptr);
  CFRelease(d);
  CTFontSymbolicTraits traits = 0;
  if (jget(font, "bold", false)) {
    traits |= kCTFontTraitBold;
  }
  if (jget(font, "italic", false)) {
    traits |= kCTFontTraitItalic;
  }
  if (traits == 0) {
    return base;
  }
  CTFontRef styled = CTFontCreateCopyWithSymbolicTraits(
      base, size, nullptr, traits, kCTFontTraitBold | kCTFontTraitItalic);
  if (!styled) {
    return base;
  }
  CFRelease(base);
  return styled;
}

// A text box's alignment, by its name.
CTTextAlignment
alignment_of(const Json& o)
{
  const auto a = jget<std::string>(o, "align", "left");
  return a == "center"      ? kCTTextAlignmentCenter
         : a == "right"     ? kCTTextAlignmentRight
         : a == "justified" ? kCTTextAlignmentJustified
                            : kCTTextAlignmentLeft;
}

// A text BOX's words as Core Text sets them -- drawn and measured alike:
// its font, its colour (none: measured only), an underline, and its
// paragraph style. The line spacing is an ADJUSTMENT between lines (a
// line-height multiple would push the first line down from the box's
// top as well).
NSAttributedString*
box_text(const Json& o, NSString* s, CTFontRef f, CGColorRef color)
{
  NSMutableDictionary* a = [NSMutableDictionary dictionary];
  a[(__bridge NSString*)kCTFontAttributeName] = (__bridge id)f;
  if (color) {
    a[(__bridge NSString*)kCTForegroundColorAttributeName] =
        (__bridge id)color;
  }
  if (jget(jget(o, "font", Json::object()), "underline", false)) {
    a[(__bridge NSString*)kCTUnderlineStyleAttributeName] =
        @(kCTUnderlineStyleSingle);
  }
  CTTextAlignment align = alignment_of(o);
  const double natural =
      CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f);
  CGFloat between = static_cast<CGFloat>(
      (std::clamp(finite_or(o, "line_spacing", 1), 1.0, 3.0) - 1) *
      natural);
  CGFloat after = static_cast<CGFloat>(
      std::clamp(finite_or(o, "paragraph_spacing", 0), 0.0, 2000.0));
  const CTParagraphStyleSetting settings[] = {
      {kCTParagraphStyleSpecifierAlignment, sizeof align, &align},
      {kCTParagraphStyleSpecifierLineSpacingAdjustment, sizeof between,
       &between},
      {kCTParagraphStyleSpecifierParagraphSpacing, sizeof after, &after},
  };
  CTParagraphStyleRef ps = CTParagraphStyleCreate(settings, 3);
  a[(__bridge NSString*)kCTParagraphStyleAttributeName] = (__bridge id)ps;
  CFRelease(ps);
  return [[NSAttributedString alloc] initWithString:s attributes:a];
}

// The UTF-8 bytes of `s`'s first `units` UTF-16 units.
std::size_t
utf8_bytes(NSString* s, CFIndex units)
{
  units = std::clamp<CFIndex>(units, 0, static_cast<CFIndex>(s.length));
  return [[s substringToIndex:static_cast<NSUInteger>(units)]
      lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
}

void
set_fill(CGContextRef ctx, const std::array<double, 4>& c)
{
  CGContextSetRGBFillColor(ctx, c[0], c[1], c[2], c[3]);
}

void
set_stroke(CGContextRef ctx, const std::array<double, 4>& c)
{
  CGContextSetRGBStrokeColor(ctx, c[0], c[1], c[2], c[3]);
}

// A GRADIENT (markup.h): its stops sorted and held to 0..1, at least two
// (else none), sRGB; where it runs, in the object's box's units.
struct Gradient {
  bool                radial = false;
  std::vector<CGFloat> locs;
  std::vector<CGFloat> comps;  // r, g, b, a a stop
  double              fx = 0, fy = 0.5, tx = 1, ty = 0.5;
};

std::optional<Gradient>
gradient_of(const Json& o)
{
  const Json g = jget(o, "gradient", Json());
  if (!g.is_object()) {
    return std::nullopt;
  }
  std::vector<std::pair<double, std::array<double, 4>>> stops;
  for (const auto& st : jget(g, "stops", Json::array())) {
    stops.push_back({clamp01(finite_or(st, "at", 0)),
                     rgba_of(jget(st, "color", Json()), {0, 0, 0, 1})});
  }
  if (stops.size() < 2) {
    return std::nullopt;
  }
  std::stable_sort(stops.begin(), stops.end(),
                   [](const auto& a, const auto& b) {
                     return a.first < b.first;
                   });
  Gradient out;
  out.radial = jget<std::string>(g, "kind", "linear") == "radial";
  for (const auto& [at, c] : stops) {
    out.locs.push_back(at);
    out.comps.insert(out.comps.end(), {c[0], c[1], c[2], c[3]});
  }
  auto point = [&](const char* k, double dx, double dy) {
    const Json p = jget(g, k, Json());
    if (p.is_array() && p.size() == 2 && p[0].is_number() &&
        p[1].is_number() && std::isfinite(p[0].get<double>()) &&
        std::isfinite(p[1].get<double>())) {
      return std::pair{p[0].get<double>(), p[1].get<double>()};
    }
    return std::pair{dx, dy};
  };
  std::tie(out.fx, out.fy) =
      point("from", out.radial ? 0.5 : 0.0, 0.5);
  std::tie(out.tx, out.ty) = point("to", 1.0, 0.5);
  return out;
}

// `g` drawn over what the clip leaves, its ends at the box's units
// (x0, y0)-(x1, y1) -- as an object's corners, so it moves and scales
// with it -- extended past both: a linear one from FROM to TO, a radial
// one round FROM out to TO.
void
draw_gradient(CGContextRef ctx, const Gradient& g, double x0, double y0,
              double x1, double y1)
{
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGGradientRef cg = CGGradientCreateWithColorComponents(
      srgb, g.comps.data(), g.locs.data(), g.locs.size());
  CGColorSpaceRelease(srgb);
  const CGPoint a = CGPointMake(x0 + g.fx * (x1 - x0), y0 + g.fy * (y1 - y0));
  const CGPoint b = CGPointMake(x0 + g.tx * (x1 - x0), y0 + g.ty * (y1 - y0));
  if (g.radial) {
    CGContextDrawRadialGradient(ctx, cg, a, 0, a,
                                std::hypot(b.x - a.x, b.y - a.y),
                                kCGGradientDrawsAfterEndLocation);
  } else {
    CGContextDrawLinearGradient(ctx, cg, a, b,
                                kCGGradientDrawsBeforeStartLocation |
                                    kCGGradientDrawsAfterEndLocation);
  }
  CGGradientRelease(cg);
}

// A cubic Bezier piece of a path: from A to B, its control points C1, C2.
struct Piece {
  CGPoint a, c1, c2, b;
};

// The share of a segment a smooth point's handle reaches along it, for a
// turn of `theta` there: what puts a circle's arc through points evenly
// round it -- (2/3) tan(θ/4) / sin(θ/2), a third for no turn -- the turn
// held to 120° (a sharper one would loop).
double
handle_share(double theta)
{
  const double t = std::min(theta, 2 * std::numbers::pi / 3);
  if (t < 1e-4) {
    return 1.0 / 3;
  }
  return 2.0 / 3 * std::tan(t / 4) / std::sin(t / 2);
}

// A PATH's pieces (2026-10-15): the curve through a polygon's points on
// the canvas, as a curvature pen draws it -- a cubic Bezier from each
// point to the next (and the last back to the first, closed). A SMOOTH
// point is passed through without a turn: its tangent runs along the
// line from the point before it to the one after, its handles that way,
// each a share of the segment it reaches into (handle_share) -- four
// smooth points round a square make a circle. A CORNER -- and an open
// path's ends -- has none: its segments leave it straight for the next
// handle. Computed on the canvas, not in the box's units: the curve the
// points make where they are. The app draws it the same (MarkupRender).
std::vector<Piece>
path_pieces(const std::vector<CGPoint>& p, const std::vector<bool>& smooth,
            bool closed)
{
  const std::size_t n = p.size();
  std::vector<CGPoint> in(p), out(p);
  for (std::size_t i = 0; i < n; ++i) {
    if (i >= smooth.size() || !smooth[i] ||
        (!closed && (i == 0 || i + 1 == n)) || n < 3) {
      continue;
    }
    const CGPoint a = p[(i + n - 1) % n], b = p[(i + 1) % n];
    const double dx = b.x - a.x, dy = b.y - a.y;
    const double len = std::hypot(dx, dy);
    if (len < 1e-9) {
      continue;
    }
    const double vx = p[i].x - a.x, vy = p[i].y - a.y;
    const double wx = b.x - p[i].x, wy = b.y - p[i].y;
    const double turn =
        std::atan2(std::abs(vx * wy - vy * wx), vx * wx + vy * wy);
    const double f = handle_share(turn);
    const double back = std::hypot(vx, vy) * f / len;
    const double on = std::hypot(wx, wy) * f / len;
    in[i] = CGPointMake(p[i].x - dx * back, p[i].y - dy * back);
    out[i] = CGPointMake(p[i].x + dx * on, p[i].y + dy * on);
  }
  std::vector<Piece> pieces;
  for (std::size_t i = 0; i + 1 < n || (closed && i < n && n > 1); ++i) {
    const std::size_t j = (i + 1) % n;
    pieces.push_back({p[i], out[i], in[j], p[j]});
  }
  return pieces;
}

// A rectangle's, an ellipse's or a POLYGON's outline: a polygon's points
// are its box's units (as its corners move, so do they), closed -- or, a
// PATH, through its smooth points (path_pieces), OPEN where it says so.
CGMutablePathRef
shape_path(const Json& o, const std::string& kind, double x0, double y0,
           double x1, double y1)
{
  CGMutablePathRef path = CGPathCreateMutable();
  if (kind == "polygon") {
    std::vector<CGPoint> pts;
    std::vector<bool> smooth;
    const Json flags = jget(o, "smooth", Json::array());
    const Json all = jget(o, "points", Json::array());
    for (std::size_t i = 0; i < all.size(); ++i) {
      const Json& p = all[i];
      if (!p.is_array() || p.size() != 2 || !p[0].is_number() ||
          !p[1].is_number()) {
        continue;
      }
      pts.push_back(CGPointMake(x0 + p[0].get<double>() * (x1 - x0),
                                y0 + p[1].get<double>() * (y1 - y0)));
      smooth.push_back(flags.is_array() && i < flags.size() &&
                       flags[i].is_boolean() && flags[i].get<bool>());
    }
    if (pts.empty()) {
      return path;
    }
    const bool closed = !jget(o, "open", false);
    CGPathMoveToPoint(path, nullptr, pts[0].x, pts[0].y);
    const auto pieces = path_pieces(pts, smooth, closed);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      const Piece& c = pieces[i];
      const bool straight = CGPointEqualToPoint(c.c1, c.a) &&
                            CGPointEqualToPoint(c.c2, c.b);
      if (straight && closed && i + 1 == pieces.size()) {
        break;  // the closing edge: the subpath's close draws it
      }
      if (straight) {
        CGPathAddLineToPoint(path, nullptr, c.b.x, c.b.y);
      } else {
        CGPathAddCurveToPoint(path, nullptr, c.c1.x, c.c1.y, c.c2.x,
                              c.c2.y, c.b.x, c.b.y);
      }
    }
    if (closed) {
      CGPathCloseSubpath(path);
    }
    return path;
  }
  const CGRect r = CGRectStandardize(CGRectMake(x0, y0, x1 - x0, y1 - y0));
  if (kind == "ellipse") {
    CGPathAddEllipseInRect(path, nullptr, r);
  } else {
    CGPathAddRect(path, nullptr, r);
  }
  return path;
}

// A LINE's glyphs as outlines into `glyphs`, its underlines' bars into
// `bars`, its origin at `origin` (y up): words a gradient fills in a
// vector context (draw_body).
void
add_outlines(CGMutablePathRef glyphs, CGMutablePathRef bars, CTLineRef line,
             CGPoint origin)
{
  CFArrayRef runs = CTLineGetGlyphRuns(line);
  for (CFIndex r = 0; r < CFArrayGetCount(runs); ++r) {
    auto run = static_cast<CTRunRef>(CFArrayGetValueAtIndex(runs, r));
    NSDictionary* attrs = (__bridge NSDictionary*)CTRunGetAttributes(run);
    auto font = (__bridge CTFontRef)
        attrs[(__bridge NSString*)kCTFontAttributeName];
    const CFIndex n = CTRunGetGlyphCount(run);
    if (!font || n <= 0) {
      continue;
    }
    std::vector<CGGlyph> ids(static_cast<std::size_t>(n));
    std::vector<CGPoint> at(static_cast<std::size_t>(n));
    CTRunGetGlyphs(run, CFRangeMake(0, 0), ids.data());
    CTRunGetPositions(run, CFRangeMake(0, 0), at.data());
    for (std::size_t i = 0; i < ids.size(); ++i) {
      const CGAffineTransform t = CGAffineTransformMakeTranslation(
          origin.x + at[i].x, origin.y + at[i].y);
      if (CGPathRef g = CTFontCreatePathForGlyph(font, ids[i], &t)) {
        CGPathAddPath(glyphs, nullptr, g);
        CGPathRelease(g);
      }
    }
    if ([attrs[(__bridge NSString*)kCTUnderlineStyleAttributeName]
            intValue] != 0) {
      const double w = CTRunGetTypographicBounds(run, CFRangeMake(0, 0),
                                                 nullptr, nullptr, nullptr);
      const double th = std::max(1.0, CTFontGetUnderlineThickness(font));
      const double y = origin.y + CTFontGetUnderlinePosition(font);
      CGPathAddRect(bars, nullptr, CGRectMake(origin.x + at[0].x,
                                              y - th / 2, w, th));
    }
  }
}

// A stroke's DASHES (markup.h "dash"), set on `ctx` for the outline or
// line about to be stroked: in its own widths, so they scale with it.
void
set_dash(CGContextRef ctx, const Json& o, double width)
{
  const auto d = jget<std::string>(o, "dash", "");
  const CGFloat w = static_cast<CGFloat>(std::max(1.0, width));
  if (d == "dashed") {
    const CGFloat lengths[2] = {3 * w, 2 * w};
    CGContextSetLineDash(ctx, 0, lengths, 2);
    CGContextSetLineCap(ctx, kCGLineCapButt);
  } else if (d == "dotted") {
    // A dash of no length with a round cap is a dot.
    const CGFloat lengths[2] = {0, 2 * w};
    CGContextSetLineDash(ctx, 0, lengths, 2);
    CGContextSetLineCap(ctx, kCGLineCapRound);
  }
}

// A line's arrowhead (markup.h "arrows") with its tip at `tip`, pointing
// along (ux, uy), `len` long and `half` either side.
void
fill_arrowhead(CGContextRef ctx, double tx, double ty, double ux, double uy,
               double len, double half)
{
  const double bx = tx - ux * len, by = ty - uy * len;
  CGContextMoveToPoint(ctx, tx, ty);
  CGContextAddLineToPoint(ctx, bx - uy * half, by + ux * half);
  CGContextAddLineToPoint(ctx, bx + uy * half, by - ux * half);
  CGContextClosePath(ctx);
  CGContextFillPath(ctx);
}

// A shape's TEXT AREA in its own pixels (markup.h): its box -- an
// ellipse's inscribed rectangle -- inset by half its outline and a fifth
// of its font's size; empty where nothing fits.
CGRect
shape_text_area(const Json& o, double x0, double y0, double x1, double y1)
{
  CGRect r = CGRectStandardize(CGRectMake(x0, y0, x1 - x0, y1 - y0));
  if (jget<std::string>(o, "kind", "") == "ellipse") {
    const double k = (1 - std::numbers::sqrt2 / 2) / 2;
    r = CGRectInset(r, r.size.width * k, r.size.height * k);
  }
  const double size =
      std::clamp(finite_or(jget(o, "font", Json::object()), "size", 48),
                 1.0, 2000.0);
  const double pad =
      std::max(0.0, finite_or(o, "width", 0)) / 2 + size * 0.2;
  r = CGRectInset(r, pad, pad);
  return r.size.width > 0 && r.size.height > 0 ? r : CGRectNull;
}

// A shape's WORDS (markup.h): set in its text area as a text box's are --
// wrapped, the lines that fit whole -- the block of them centred down
// it, centred across unless aligned otherwise. On a context whose CTM is
// the shape's own pixels turned AND flipped: the flips taken back (its
// box's centre stays), so the words are never mirrored.
void
draw_shape_text(CGContextRef ctx, const Json& o, double x0, double y0,
                double x1, double y1)
{
  const std::string text = jget<std::string>(o, "text", "");
  const CGRect area = shape_text_area(o, x0, y0, x1, y1);
  if (text.empty() || CGRectIsNull(area)) {
    return;
  }
  NSString* s = [[NSString alloc] initWithBytes:text.data()
                                         length:text.size()
                                       encoding:NSUTF8StringEncoding];
  if (s.length == 0) {
    return;
  }
  Json styled = o;
  if (!styled.contains("align")) {
    styled["align"] = "center";
  }
  CTFontRef f = font_of(jget(o, "font", Json::object()));
  const auto tc = rgba_of(jget(o, "text_color", Json()), {0, 0, 0, 1});
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  const CGFloat comps[4] = {tc[0], tc[1], tc[2], tc[3]};
  CGColorRef color = CGColorCreate(srgb, comps);
  CGColorSpaceRelease(srgb);
  NSAttributedString* as = box_text(styled, s, f, color);
  CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(
      (__bridge CFAttributedStringRef)as);
  const double w = area.size.width, h = area.size.height;
  CGPathRef path = CGPathCreateWithRect(CGRectMake(0, 0, w, h), nullptr);
  CTFrameRef frame =
      CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, nullptr);
  CFArrayRef lines = CTFrameGetLines(frame);
  const CFIndex n = CFArrayGetCount(lines);
  if (n > 0) {
    std::vector<CGPoint> origins(static_cast<std::size_t>(n));
    CTFrameGetLineOrigins(frame, CFRangeMake(0, 0), origins.data());
    // The set lines' height: the frame's top to the last one's bottom.
    CGFloat descent = 0;
    CTLineGetTypographicBounds(
        static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, n - 1)),
        nullptr, &descent, nullptr);
    const double bottom = origins.back().y - descent;
    const double drop = std::max(0.0, bottom / 2);
    CGContextSaveGState(ctx);
    if (jget(o, "flip_x", false) || jget(o, "flip_y", false)) {
      const double cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
      CGAffineTransform t = CGAffineTransformMakeTranslation(cx, cy);
      t = CGAffineTransformScale(t, jget(o, "flip_x", false) ? -1 : 1,
                                 jget(o, "flip_y", false) ? -1 : 1);
      CGContextConcatCTM(ctx, CGAffineTransformTranslate(t, -cx, -cy));
    }
    CGContextTranslateCTM(ctx, area.origin.x, area.origin.y + h);
    CGContextScaleCTM(ctx, 1, -1);
    CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
    for (CFIndex i = 0; i < n; ++i) {
      auto l = static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, i));
      const auto& at = origins[static_cast<std::size_t>(i)];
      CGContextSetTextPosition(ctx, at.x, at.y - drop);
      CTLineDraw(l, ctx);
    }
    CGContextRestoreGState(ctx);
  }
  CFRelease(frame);
  CGPathRelease(path);
  CFRelease(fs);
  CGColorRelease(color);
  CFRelease(f);
}

// One object, on a context whose CTM is canvas pixels, y down: itself,
// without its effects. For a VECTOR context (`vectors`: a PDF's), a
// gradient in a text box's words is laid through their glyphs' outlines
// as a clip -- the words made shapes -- not into a layer of them
// source-in, which a PDF cannot hold (Quartz lays the gradient over the
// whole layer).
void
draw_body(CGContextRef ctx, const Json& o, bool vectors)
{
  const auto kind = jget<std::string>(o, "kind", "");
  const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
  const double x1 = finite_or(o, "x1", 0), y1 = finite_or(o, "y1", 0);
  const auto stroke = rgba_of(jget(o, "stroke", Json()), {0, 0, 0, 1});
  const auto fill = rgba_of(jget(o, "fill", Json()), {0, 0, 0, 0});
  const double width = std::max(0.0, finite_or(o, "width", 0));
  CGContextSaveGState(ctx);
  // Its turn and flips about its box's centre: all of it below is drawn
  // in its own pixels.
  CGContextConcatCTM(ctx, object_transform(o));
  if (kind == "drawing") {
    // Its pixels in its box, upright on a y-down canvas.
    if (CGImageRef img = read_image(jget<std::string>(o, "file", ""))) {
      const CGRect r = CGRectStandardize(
          CGRectMake(x0, y0, x1 - x0, y1 - y0));
      CGContextTranslateCTM(ctx, r.origin.x, r.origin.y + r.size.height);
      CGContextScaleCTM(ctx, 1, -1);
      CGContextDrawImage(ctx, CGRectMake(0, 0, r.size.width,
                                         r.size.height), img);
      CGImageRelease(img);
    }
  } else if (kind == "line") {
    if (width > 0) {
      // Its arrowheads, at either end or both: the line stopped inside
      // each, so neither its round cap nor a dash shows past the tip.
      const auto arrows = jget<std::string>(o, "arrows", "");
      const bool at0 = arrows == "start" || arrows == "both";
      const bool at1 = arrows == "end" || arrows == "both";
      const double dx = x1 - x0, dy = y1 - y0;
      const double run = std::hypot(dx, dy);
      const double ux = run > 0 ? dx / run : 1, uy = run > 0 ? dy / run : 0;
      const int heads = (at0 ? 1 : 0) + (at1 ? 1 : 0);
      double len = std::max(10.0, 4 * width);
      if (heads > 0 && run > 0) {
        len = std::min(len, run / heads);
      }
      const double half = std::max(5.0, 2 * width);
      const double in = len * 0.8;
      CGContextSetLineCap(ctx, kCGLineCapRound);
      CGContextSetLineWidth(ctx, width);
      set_dash(ctx, o, width);
      set_stroke(ctx, stroke);
      CGContextMoveToPoint(ctx, x0 + (at0 ? ux * in : 0),
                           y0 + (at0 ? uy * in : 0));
      CGContextAddLineToPoint(ctx, x1 - (at1 ? ux * in : 0),
                              y1 - (at1 ? uy * in : 0));
      CGContextStrokePath(ctx);
      if (heads > 0 && run > 0) {
        set_fill(ctx, stroke);
        if (at1) {
          fill_arrowhead(ctx, x1, y1, ux, uy, len, half);
        }
        if (at0) {
          fill_arrowhead(ctx, x0, y0, -ux, -uy, len, half);
        }
      }
    }
  } else if (kind == "rect" || kind == "ellipse" || kind == "polygon") {
    // Its inside: a gradient through its outline as a clip, else its
    // flat fill; then its outline. An OPEN path has no inside: a line,
    // its ends and turns round.
    const bool open = kind == "polygon" && jget(o, "open", false);
    CGMutablePathRef path = shape_path(o, kind, x0, y0, x1, y1);
    const std::optional<Gradient> g =
        open ? std::optional<Gradient>() : gradient_of(o);
    if (g) {
      CGContextSaveGState(ctx);
      CGContextAddPath(ctx, path);
      CGContextClip(ctx);
      draw_gradient(ctx, *g, x0, y0, x1, y1);
      CGContextRestoreGState(ctx);
    } else if (!open && fill[3] > 0) {
      set_fill(ctx, fill);
      CGContextAddPath(ctx, path);
      CGContextFillPath(ctx);
    }
    if (width > 0) {
      CGContextSetLineJoin(ctx, open ? kCGLineJoinRound : kCGLineJoinMiter);
      if (open) {
        CGContextSetLineCap(ctx, kCGLineCapRound);
      }
      CGContextSetLineWidth(ctx, width);
      set_dash(ctx, o, width);
      set_stroke(ctx, stroke);
      CGContextAddPath(ctx, path);
      CGContextStrokePath(ctx);
    }
    CGPathRelease(path);
    // Its words, over its inside and outline (an open path has none).
    if (!open) {
      draw_shape_text(ctx, o, x0, y0, x1, y1);
    }
  } else if (kind == "text") {
    const Json font = jget(o, "font", Json::object());
    CTFontRef f = font_of(font);
    const double ascent = CTFontGetAscent(f);
    const double line_h = ascent + CTFontGetDescent(f) + CTFontGetLeading(f);
    CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const CGFloat comps[4] = {stroke[0], stroke[1], stroke[2], stroke[3]};
    CGColorRef color = CGColorCreate(srgb, comps);
    CGColorSpaceRelease(srgb);
    NSDictionary* attrs = @{
      (__bridge NSString*)kCTFontAttributeName : (__bridge id)f,
      (__bridge NSString*)kCTForegroundColorAttributeName :
          (__bridge id)color,
    };
    const bool underline = jget(font, "underline", false);
    const std::string text = jget<std::string>(o, "text", "");
    if (jget(o, "box", false)) {
      // A text BOX: its words wrapped within its width, only the lines
      // that fit whole in its height set (a framesetter sets whole lines
      // only). Laid out y up from the box's bottom-left, a line at a time
      // at the frame's origins (as CTFrameDraw does) -- so a justified
      // paragraph running on in another box has its last line stretched
      // too.
      const double bx = std::min(x0, x1), by = std::min(y0, y1);
      const double bw = std::abs(x1 - x0), bh = std::abs(y1 - y0);
      NSString* s = [[NSString alloc] initWithBytes:text.data()
                                             length:text.size()
                                           encoding:NSUTF8StringEncoding];
      // A gradient colours the words: drawn in a layer of their own, the
      // gradient laid only where they are (source-in) -- or, in a vector
      // context, through their outlines.
      const auto grad = gradient_of(o);
      const bool outlined = vectors && grad;
      const double size = finite_or(font, "size", 48);
      if (grad && !outlined) {
        CGContextBeginTransparencyLayerWithRect(
            ctx, CGRectInset(CGRectMake(bx, by, bw, bh), -size, -size),
            nullptr);
      }
      CGContextSaveGState(ctx);
      if (s.length > 0 && bw > 0 && bh > 0) {
        NSAttributedString* as = box_text(o, s, f, color);
        CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(
            (__bridge CFAttributedStringRef)as);
        CGPathRef path =
            CGPathCreateWithRect(CGRectMake(0, 0, bw, bh), nullptr);
        CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0),
                                                    path, nullptr);
        CGContextTranslateCTM(ctx, bx, by + bh);
        CGContextScaleCTM(ctx, 1, -1);
        CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
        CFArrayRef lines = CTFrameGetLines(frame);
        const CFIndex n = CFArrayGetCount(lines);
        std::vector<CGPoint> origins(static_cast<std::size_t>(n));
        CTFrameGetLineOrigins(frame, CFRangeMake(0, 0), origins.data());
        const bool stretch = jget(o, "justify_last", false) &&
                             alignment_of(o) == kCTTextAlignmentJustified;
        CGMutablePathRef glyphs = outlined ? CGPathCreateMutable() : nullptr;
        CGMutablePathRef bars = outlined ? CGPathCreateMutable() : nullptr;
        for (CFIndex i = 0; i < n; ++i) {
          auto l = static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, i));
          const CFRange r = CTLineGetStringRange(l);
          CTLineRef drawn = nullptr;
          if (stretch && i == n - 1 &&
              r.location + r.length >= static_cast<CFIndex>(s.length)) {
            drawn = CTLineCreateJustifiedLine(l, 1.0, bw);
          }
          if (outlined) {
            add_outlines(glyphs, bars, drawn ? drawn : l, origins[i]);
          } else {
            CGContextSetTextPosition(ctx, origins[i].x, origins[i].y);
            CTLineDraw(drawn ? drawn : l, ctx);
          }
          if (drawn) {
            CFRelease(drawn);
          }
        }
        if (outlined) {
          // The gradient through the glyphs, then through the underlines
          // apart (one path, their windings could cancel where they
          // cross), at the words' colour's alpha -- as source-in leaves
          // it. Its ends are the object's: back to canvas pixels, the
          // clip kept.
          for (CGPathRef clip : {static_cast<CGPathRef>(glyphs),
                                 static_cast<CGPathRef>(bars)}) {
            if (CGPathIsEmpty(clip)) {
              continue;
            }
            CGContextSaveGState(ctx);
            CGContextAddPath(ctx, clip);
            CGContextClip(ctx);
            CGContextScaleCTM(ctx, 1, -1);
            CGContextTranslateCTM(ctx, -bx, -(by + bh));
            CGContextSetAlpha(ctx, stroke[3]);
            draw_gradient(ctx, *grad, x0, y0, x1, y1);
            CGContextRestoreGState(ctx);
          }
          CGPathRelease(glyphs);
          CGPathRelease(bars);
        }
        CFRelease(frame);
        CGPathRelease(path);
        CFRelease(fs);
      }
      CGContextRestoreGState(ctx);
      if (grad && !outlined) {
        CGContextSaveGState(ctx);
        CGContextSetBlendMode(ctx, kCGBlendModeSourceIn);
        draw_gradient(ctx, *grad, x0, y0, x1, y1);
        CGContextRestoreGState(ctx);
        CGContextEndTransparencyLayer(ctx);
      }
      CGColorRelease(color);
      CFRelease(f);
      CGContextRestoreGState(ctx);
      return;
    }
    // Glyphs are drawn y up: flip the text matrix against the y-down CTM.
    CGContextSetTextMatrix(ctx, CGAffineTransformMakeScale(1, -1));
    std::size_t start = 0;
    for (int i = 0;; ++i) {
      const auto nl = text.find('\n', start);
      const std::string line = text.substr(
          start, nl == std::string::npos ? std::string::npos : nl - start);
      NSString* s = [[NSString alloc] initWithBytes:line.data()
                                             length:line.size()
                                           encoding:NSUTF8StringEncoding];
      if (s.length > 0) {
        NSAttributedString* as =
            [[NSAttributedString alloc] initWithString:s attributes:attrs];
        CTLineRef l = CTLineCreateWithAttributedString(
            (__bridge CFAttributedStringRef)as);
        const double baseline = y0 + ascent + i * line_h;
        CGContextSetTextPosition(ctx, x0, baseline);
        CTLineDraw(l, ctx);
        if (underline) {
          const double w = CTLineGetTypographicBounds(l, nullptr, nullptr,
                                                      nullptr);
          const double t = std::max(1.0, CTFontGetUnderlineThickness(f));
          // Its position is below the baseline, y up: down here.
          const double y = baseline - CTFontGetUnderlinePosition(f);
          CGContextSetFillColorWithColor(ctx, color);
          CGContextFillRect(ctx, CGRectMake(x0, y - t / 2, w, t));
        }
        CFRelease(l);
      }
      if (nl == std::string::npos) {
        break;
      }
      start = nl + 1;
    }
    CGColorRelease(color);
    CFRelease(f);
  }
  CGContextRestoreGState(ctx);
}

// An effect's pass: a shadow at (dx, dy) canvas pixels, blurred `blur`.
struct Pass {
  double                dx = 0, dy = 0, blur = 0;
  std::array<double, 4> color{0, 0, 0, 1};
};

// An object's effects as shadow passes, innermost first (markup.h): a
// shadow one, a glow two at no offset.
std::vector<Pass>
passes_of(const Json& o)
{
  std::vector<Pass> out;
  const Json fx = jget(o, "effects", Json::array());
  for (const auto& e : fx.is_array() ? fx : Json::array()) {
    const auto kind = jget<std::string>(e, "kind", "");
    const auto c = rgba_of(jget(e, "color", Json()), {0, 0, 0, 0.5});
    if (kind == "shadow") {
      out.push_back({finite_or(e, "dx", 0), finite_or(e, "dy", 0),
                     std::max(0.0, finite_or(e, "blur", 0)), c});
    } else if (kind == "glow") {
      const double r = std::max(0.0, finite_or(e, "radius", 0));
      out.push_back({0, 0, r, c});
      out.push_back({0, 0, r, c});
    }
  }
  return out;
}

// An object's box -- a polygon's with its path's, which a curve takes
// past its points.
CGRect
shape_box(const Json& o, double x0, double y0, double x1, double y1)
{
  CGRect r = CGRectStandardize(CGRectMake(x0, y0, x1 - x0, y1 - y0));
  if (jget<std::string>(o, "kind", "") == "polygon") {
    CGMutablePathRef p = shape_path(o, "polygon", x0, y0, x1, y1);
    if (!CGPathIsEmpty(p)) {
      r = CGRectUnion(r, CGPathGetPathBoundingBox(p));
    }
    CGPathRelease(p);
  }
  return r;
}

// What an object covers, its outline included; a text without a box
// (unmeasured here): none -- the whole canvas then.
CGRect
object_rect(const Json& o)
{
  const auto kind = jget<std::string>(o, "kind", "");
  if (kind == "text" && !jget(o, "box", false)) {
    return CGRectNull;
  }
  const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
  const double x1 = finite_or(o, "x1", 0), y1 = finite_or(o, "y1", 0);
  const double w = kind == "text" ? 0 : std::max(0.0, finite_or(o, "width",
                                                                0));
  return CGRectApplyAffineTransform(
      CGRectInset(shape_box(o, x0, y0, x1, y1), -w, -w), object_transform(o));
}

void draw_object(CGContextRef ctx, const Json& o, bool vectors);

// The pixels in an object's effects drawn in a VECTOR context: as many a
// canvas pixel as twice the context's own, and no more than this all
// told (a 16-bit bitmap of them, twice).
constexpr double kEffectsScale = 2;
constexpr double kMostEffectsPixels = 16.0 * 1024 * 1024;

// What an object's EFFECTS lay BENEATH it -- its shadows and glows, each
// round the ones before, as a bitmap draws them -- over `r` (canvas
// pixels, y down) at `k` pixels a canvas pixel: for a vector context,
// which draws the object itself over it. A bitmap draws the object WITH
// its effects as the object over what lies beneath; that is taken back
// out of it -- (with - alone) / (1 - the object's alpha), in 16 bits --
// and where the object covers all, nothing beneath shows. 8-bit
// premultiplied sRGB, cut to what it covers: `r` made that. Null with
// nothing beneath.
CGImageRef
effects_beneath(const Json& o, CGRect& r, double k)
{
  const auto w = static_cast<std::size_t>(std::ceil(r.size.width * k));
  const auto h = static_cast<std::size_t>(std::ceil(r.size.height * k));
  if (w == 0 || h == 0) {
    return nullptr;
  }
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  auto bitmap = [&] {
    CGContextRef c = CGBitmapContextCreate(
        nullptr, w, h, 16, 0, srgb,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder16Host);
    if (c) {
      CGContextClearRect(c, CGRectMake(0, 0, w, h));
      CGContextTranslateCTM(c, 0, static_cast<CGFloat>(h));
      CGContextScaleCTM(c, k, -k);
      CGContextTranslateCTM(c, -r.origin.x, -r.origin.y);
    }
    return c;
  };
  CGContextRef with = bitmap();
  CGContextRef alone = bitmap();
  CGImageRef out = nullptr;
  if (with && alone) {
    draw_object(with, o, false);
    draw_body(alone, o, false);
    const auto* a =
        static_cast<const std::uint16_t*>(CGBitmapContextGetData(with));
    const auto* b =
        static_cast<const std::uint16_t*>(CGBitmapContextGetData(alone));
    const std::size_t ra = CGBitmapContextGetBytesPerRow(with) / 2;
    const std::size_t rb = CGBitmapContextGetBytesPerRow(alone) / 2;
    std::vector<std::uint8_t> under(w * h * 4, 0);
    std::size_t x0 = w, y0 = h, x1 = 0, y1 = 0;  // what it covers
    for (std::size_t y = 0; y < h; ++y) {
      const std::uint16_t* pa = a + y * ra;
      const std::uint16_t* pb = b + y * rb;
      std::uint8_t* pu = under.data() + y * w * 4;
      for (std::size_t x = 0; x < w; ++x, pa += 4, pb += 4, pu += 4) {
        const double clear = 1 - pb[3] / 65535.0;
        if (clear < 1.0 / 1024) {
          continue;
        }
        double v[4];
        for (int c = 0; c < 4; ++c) {
          v[c] = std::clamp((pa[c] - pb[c]) / 65535.0 / clear, 0.0, 1.0);
        }
        for (int c = 0; c < 3; ++c) {
          v[c] = std::min(v[c], v[3]);  // premultiplied
        }
        for (int c = 0; c < 4; ++c) {
          pu[c] = static_cast<std::uint8_t>(std::lround(v[c] * 255));
        }
        if (pu[3] > 0) {
          x0 = std::min(x0, x);
          y0 = std::min(y0, y);
          x1 = std::max(x1, x + 1);
          y1 = std::max(y1, y + 1);
        }
      }
    }
    if (x1 > x0 && y1 > y0) {
      CGContextRef cut = CGBitmapContextCreate(
          under.data() + (y0 * w + x0) * 4, x1 - x0, y1 - y0, 8, w * 4,
          srgb, kCGImageAlphaPremultipliedLast);
      if (cut) {
        out = CGBitmapContextCreateImage(cut);
        CGContextRelease(cut);
      }
      r = CGRectMake(r.origin.x + x0 / k, r.origin.y + y0 / k,
                     (x1 - x0) / k, (y1 - y0) / k);
    }
  }
  CGColorSpaceRelease(srgb);
  if (with) {
    CGContextRelease(with);
  }
  if (alone) {
    CGContextRelease(alone);
  }
  return out;
}

// One object with its effects: each a transparency layer composited with
// its shadow -- the outermost begun first -- the object drawn once inside
// them all. A bitmap's shadow is measured in its pixels, y up: the offset
// and blur converted from canvas pixels through the CTM. In a VECTOR
// context (a PDF's) the effects are pixels beneath it (effects_beneath),
// the object itself drawn as vectors: Quartz would draw the layers as PDF
// groups blended in Generic RGB, a saturated colour in them shifting
// (sRGB's red came back 255, 37, 0), and its shadows' offsets are its
// base space's, y up.
void
draw_object(CGContextRef ctx, const Json& o, bool vectors)
{
  const std::vector<Pass> passes = passes_of(o);
  if (passes.empty()) {
    draw_body(ctx, o, vectors);
    return;
  }
  const CGAffineTransform dev =
      CGContextGetUserSpaceToDeviceSpaceTransform(ctx);
  const double scale = std::sqrt(std::abs(dev.a * dev.d - dev.b * dev.c));
  double pad = 0;
  for (const auto& p : passes) {
    pad += p.blur * 2 + std::max(std::abs(p.dx), std::abs(p.dy));
  }
  if (vectors) {
    const auto b = object_bounds(o);
    CGRect r = CGRectInset(CGRectMake(b[0], b[1], b[2], b[3]), -pad - 2,
                           -pad - 2);
    double k = std::clamp(kEffectsScale * scale, 0.25, 8.0);
    const double area = r.size.width * r.size.height * k * k;
    if (area > kMostEffectsPixels) {
      k *= std::sqrt(kMostEffectsPixels / area);
    }
    if (CGImageRef under = effects_beneath(o, r, k)) {
      // Upright on the y-down canvas.
      CGContextSaveGState(ctx);
      CGContextTranslateCTM(ctx, r.origin.x, r.origin.y + r.size.height);
      CGContextScaleCTM(ctx, 1, -1);
      CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
      CGContextDrawImage(ctx, CGRectMake(0, 0, r.size.width, r.size.height),
                         under);
      CGContextRestoreGState(ctx);
      CGImageRelease(under);
    }
    draw_body(ctx, o, true);
    return;
  }
  CGRect r = object_rect(o);
  r = CGRectIsNull(r) ? CGContextGetClipBoundingBox(ctx)
                      : CGRectInset(r, -pad - 2, -pad - 2);
  CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  for (std::size_t i = passes.size(); i-- > 0;) {
    const Pass& p = passes[i];
    const CGFloat comps[4] = {p.color[0], p.color[1], p.color[2],
                              p.color[3]};
    CGColorRef c = CGColorCreate(srgb, comps);
    CGContextSaveGState(ctx);
    // The offset as a vector through the CTM -- two points' difference
    // (a size converted loses the canvas's flip) -- into a bitmap's
    // device space, whose rows run DOWN (its first is the top); a
    // shadow's base space runs up.
    const CGPoint a = CGContextConvertPointToDeviceSpace(ctx, CGPointZero);
    const CGPoint b =
        CGContextConvertPointToDeviceSpace(ctx, CGPointMake(p.dx, p.dy));
    CGContextSetShadowWithColor(ctx, CGSizeMake(b.x - a.x, a.y - b.y),
                                p.blur * scale, c);
    CGColorRelease(c);
    CGContextBeginTransparencyLayerWithRect(ctx, r, nullptr);
  }
  CGColorSpaceRelease(srgb);
  draw_body(ctx, o, false);
  for (std::size_t i = 0; i < passes.size(); ++i) {
    CGContextEndTransparencyLayer(ctx);
    CGContextRestoreGState(ctx);
  }
}

}

TextFit
text_fit(const Json& o, bool all_lines)
{
  TextFit fit;
  if (jget<std::string>(o, "kind", "") != "text") {
    return fit;
  }
  const std::string text = jget<std::string>(o, "text", "");
  const double bw = std::abs(finite_or(o, "x1", 0) - finite_or(o, "x0", 0));
  const double bh = std::abs(finite_or(o, "y1", 0) - finite_or(o, "y0", 0));
  NSString* s = [[NSString alloc] initWithBytes:text.data()
                                         length:text.size()
                                       encoding:NSUTF8StringEncoding];
  if (s.length == 0 || bw <= 0 || bh <= 0) {
    return fit;
  }
  CTFontRef f = font_of(jget(o, "font", Json::object()));
  NSAttributedString* as = box_text(o, s, f, nullptr);
  CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(
      (__bridge CFAttributedStringRef)as);
  CFRange all{};
  const CGSize need = CTFramesetterSuggestFrameSizeWithConstraints(
      fs, CFRangeMake(0, 0), nullptr, CGSizeMake(bw, CGFLOAT_MAX), &all);
  fit.needed_height = std::ceil(need.height);
  CGPathRef path = CGPathCreateWithRect(CGRectMake(0, 0, bw, bh), nullptr);
  CTFrameRef frame =
      CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), path, nullptr);
  const CFRange shown = CTFrameGetVisibleStringRange(frame);
  fit.fits = shown.location + shown.length >= static_cast<CFIndex>(s.length);
  fit.shown = fit.fits ? text.size()
                       : utf8_bytes(s, shown.location + shown.length);
  CFArrayRef lines = CTFrameGetLines(frame);
  fit.lines = static_cast<int>(CFArrayGetCount(lines));
  for (CFIndex i = 0; i < CFArrayGetCount(lines); ++i) {
    auto l = static_cast<CTLineRef>(CFArrayGetValueAtIndex(lines, i));
    const double w = CTLineGetTypographicBounds(l, nullptr, nullptr, nullptr) -
                     CTLineGetTrailingWhitespaceWidth(l);
    fit.widest = std::max(fit.widest, w);
  }
  CFRelease(frame);
  CGPathRelease(path);
  if (all_lines) {
    // Every line at the box's width, the box as tall as they need; the
    // height to show the lines up to each is the framesetter's own for
    // that much of the text, so a box of it sets exactly those lines.
    CGPathRef tall = CGPathCreateWithRect(
        CGRectMake(0, 0, bw, fit.needed_height + 1), nullptr);
    CTFrameRef whole =
        CTFramesetterCreateFrame(fs, CFRangeMake(0, 0), tall, nullptr);
    CFArrayRef ls = CTFrameGetLines(whole);
    CFIndex units = 0;
    std::size_t bytes = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(ls); ++i) {
      auto l = static_cast<CTLineRef>(CFArrayGetValueAtIndex(ls, i));
      const CFRange r = CTLineGetStringRange(l);
      const CFIndex end = r.location + r.length;
      bytes += [[s substringWithRange:NSMakeRange(
                     static_cast<NSUInteger>(units),
                     static_cast<NSUInteger>(end - units))]
          lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
      units = end;
      fit.line_ends.push_back(bytes);
      const CGSize h = CTFramesetterSuggestFrameSizeWithConstraints(
          fs, CFRangeMake(0, end), nullptr, CGSizeMake(bw, CGFLOAT_MAX),
          nullptr);
      fit.line_heights.push_back(std::ceil(h.height));
    }
    CFRelease(whole);
    CGPathRelease(tall);
  }
  CFRelease(fs);
  CFRelease(f);
  return fit;
}

namespace {

// What an object covers in its OWN pixels, before its turn and flips.
std::array<double, 4>
own_bounds(const Json& o)
{
  const auto kind = jget<std::string>(o, "kind", "");
  const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
  const double x1 = finite_or(o, "x1", 0), y1 = finite_or(o, "y1", 0);
  if (kind == "text" && !jget(o, "box", false)) {
    // Its lines as draw_body sets them: one a "\n", unwrapped, from its
    // top-left.
    const Json font = jget(o, "font", Json::object());
    CTFontRef f = font_of(font);
    const double line_h =
        CTFontGetAscent(f) + CTFontGetDescent(f) + CTFontGetLeading(f);
    NSDictionary* attrs =
        @{(__bridge NSString*)kCTFontAttributeName : (__bridge id)f};
    const std::string text = jget<std::string>(o, "text", "");
    double widest = 0;
    int lines = 0;
    for (std::size_t start = 0;;) {
      ++lines;
      const auto nl = text.find('\n', start);
      const std::string line = text.substr(
          start, nl == std::string::npos ? std::string::npos : nl - start);
      NSString* s = [[NSString alloc] initWithBytes:line.data()
                                             length:line.size()
                                           encoding:NSUTF8StringEncoding];
      if (s.length > 0) {
        NSAttributedString* as =
            [[NSAttributedString alloc] initWithString:s attributes:attrs];
        CTLineRef l = CTLineCreateWithAttributedString(
            (__bridge CFAttributedStringRef)as);
        widest = std::max(widest, CTLineGetTypographicBounds(
                                      l, nullptr, nullptr, nullptr));
        CFRelease(l);
      }
      if (nl == std::string::npos) {
        break;
      }
      start = nl + 1;
    }
    CFRelease(f);
    return {x0, y0, std::max(widest, finite_or(font, "size", 48) * 0.3),
            line_h * lines};
  }
  const double half = kind == "text"
                          ? 0 : std::max(0.0, finite_or(o, "width", 0)) / 2;
  const CGRect r = shape_box(o, x0, y0, x1, y1);
  return {r.origin.x - half, r.origin.y - half, r.size.width + 2 * half,
          r.size.height + 2 * half};
}

}  // namespace

CGAffineTransform
object_transform(const Json& o)
{
  const double turn = finite_or(o, "rotate", 0);
  const bool fx = jget(o, "flip_x", false), fy = jget(o, "flip_y", false);
  if (turn == 0 && !fx && !fy) {
    return CGAffineTransformIdentity;
  }
  // The centre of its box -- a line's ends', a polygon's points' (not the
  // curve's bulge) -- or a text without one, of its lines as set.
  double cx = (finite_or(o, "x0", 0) + finite_or(o, "x1", 0)) / 2;
  double cy = (finite_or(o, "y0", 0) + finite_or(o, "y1", 0)) / 2;
  if (jget<std::string>(o, "kind", "") == "text" && !jget(o, "box", false)) {
    const auto b = own_bounds(o);
    cx = b[0] + b[2] / 2;
    cy = b[1] + b[3] / 2;
  }
  CGAffineTransform t = CGAffineTransformMakeTranslation(cx, cy);
  t = CGAffineTransformRotate(t, turn * std::numbers::pi / 180);
  t = CGAffineTransformScale(t, fx ? -1 : 1, fy ? -1 : 1);
  return CGAffineTransformTranslate(t, -cx, -cy);
}

std::array<double, 4>
object_bounds(const Json& o)
{
  const auto b = own_bounds(o);
  const CGRect r = CGRectApplyAffineTransform(
      CGRectMake(b[0], b[1], b[2], b[3]), object_transform(o));
  return {r.origin.x, r.origin.y, r.size.width, r.size.height};
}

// ---- Connectors and anchors (markup.h) ------------------------------

bool
takes_connectors(const std::string& kind)
{
  return kind == "line" || kind == "rect" || kind == "ellipse" ||
         kind == "polygon";
}

bool
takes_anchors(const std::string& kind)
{
  return kind == "line" || kind == "polygon";
}

namespace {

// A polygon's points in its own pixels -- its box's units laid out -- and
// which are smooth.
std::vector<CGPoint>
own_points(const Json& o, std::vector<bool>* smooth = nullptr)
{
  const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
  const double x1 = finite_or(o, "x1", 0), y1 = finite_or(o, "y1", 0);
  const Json all = jget(o, "points", Json::array());
  const Json flags = jget(o, "smooth", Json::array());
  std::vector<CGPoint> pts;
  for (std::size_t i = 0; all.is_array() && i < all.size(); ++i) {
    const Json& p = all[i];
    if (!p.is_array() || p.size() != 2 || !p[0].is_number() ||
        !p[1].is_number()) {
      continue;
    }
    pts.push_back(CGPointMake(x0 + p[0].get<double>() * (x1 - x0),
                              y0 + p[1].get<double>() * (y1 - y0)));
    if (smooth) {
      smooth->push_back(flags.is_array() && i < flags.size() &&
                        flags[i].is_boolean() && flags[i].get<bool>());
    }
  }
  return pts;
}

// An outline's SEGMENTS, in its own pixels, as its connectors lie on them
// (markup.h): a line's one, a rectangle's four from (x0, y0) clockwise,
// a path's pieces. An ellipse's is its turn: none here.
std::vector<Piece>
outline_pieces(const Json& o)
{
  const auto kind = jget<std::string>(o, "kind", "");
  const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
  const double x1 = finite_or(o, "x1", 0), y1 = finite_or(o, "y1", 0);
  auto straight = [](CGPoint a, CGPoint b) { return Piece{a, a, b, b}; };
  if (kind == "line") {
    return {straight(CGPointMake(x0, y0), CGPointMake(x1, y1))};
  }
  if (kind == "rect") {
    const CGPoint c[4] = {CGPointMake(x0, y0), CGPointMake(x1, y0),
                          CGPointMake(x1, y1), CGPointMake(x0, y1)};
    return {straight(c[0], c[1]), straight(c[1], c[2]),
            straight(c[2], c[3]), straight(c[3], c[0])};
  }
  if (kind == "polygon") {
    std::vector<bool> smooth;
    const auto pts = own_points(o, &smooth);
    return path_pieces(pts, smooth, !jget(o, "open", false));
  }
  return {};
}

// A piece's point at `t`: along a straight one evenly, a curve's own
// parameter on a curve (the app's MarkupRender.Piece.along alike).
CGPoint
piece_at(const Piece& c, double t)
{
  if (CGPointEqualToPoint(c.c1, c.a) && CGPointEqualToPoint(c.c2, c.b)) {
    return CGPointMake(c.a.x + t * (c.b.x - c.a.x),
                       c.a.y + t * (c.b.y - c.a.y));
  }
  const double u = 1 - t;
  const double k0 = u * u * u, k1 = 3 * u * u * t, k2 = 3 * u * t * t;
  const double k3 = t * t * t;
  return CGPointMake(k0 * c.a.x + k1 * c.c1.x + k2 * c.c2.x + k3 * c.b.x,
                     k0 * c.a.y + k1 * c.c1.y + k2 * c.c2.y + k3 * c.b.y);
}

// Halfway along a piece, as a parameter: its curve measured in short
// steps (a straight one's, 0.5).
double
piece_middle(const Piece& c)
{
  if (CGPointEqualToPoint(c.c1, c.a) && CGPointEqualToPoint(c.c2, c.b)) {
    return 0.5;
  }
  constexpr int kSteps = 64;
  std::array<double, kSteps + 1> run{};
  CGPoint from = c.a;
  for (int s = 1; s <= kSteps; ++s) {
    const CGPoint to = piece_at(c, static_cast<double>(s) / kSteps);
    run[s] = run[s - 1] + std::hypot(to.x - from.x, to.y - from.y);
    from = to;
  }
  const double half = run[kSteps] / 2;
  for (int s = 1; s <= kSteps; ++s) {
    if (run[s] >= half) {
      const double span = run[s] - run[s - 1];
      const double f = span > 0 ? (half - run[s - 1]) / span : 0;
      return (s - 1 + f) / kSteps;
    }
  }
  return 0.5;
}

// Connector (seg, t)'s place in `o`'s own pixels; none off its outline.
std::optional<CGPoint>
own_connector(const Json& o, int seg, double t)
{
  if (jget<std::string>(o, "kind", "") == "ellipse") {
    if (seg != 0) {
      return std::nullopt;
    }
    const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
    const double x1 = finite_or(o, "x1", 0), y1 = finite_or(o, "y1", 0);
    const double a = 2 * std::numbers::pi * t;
    return CGPointMake((x0 + x1) / 2 + std::abs(x1 - x0) / 2 * std::cos(a),
                       (y0 + y1) / 2 + std::abs(y1 - y0) / 2 * std::sin(a));
  }
  const auto pieces = outline_pieces(o);
  if (seg < 0 || static_cast<std::size_t>(seg) >= pieces.size()) {
    return std::nullopt;
  }
  return piece_at(pieces[static_cast<std::size_t>(seg)], t);
}

// Where connector `id` of `o` is shown, canvas pixels; none without it.
std::optional<CGPoint>
connector_at(const Json& o, const std::string& id)
{
  for (const auto& c : jget(o, "connectors", Json::array())) {
    if (jget<std::string>(c, "id", "") != id) {
      continue;
    }
    const auto p = own_connector(o, jget(c, "seg", 0),
                                 clamp01(finite_or(c, "t", 0)));
    if (!p) {
      return std::nullopt;
    }
    return CGPointApplyAffineTransform(*p, object_transform(o));
  }
  return std::nullopt;
}

// A polygon laid through `pts` (its own pixels): its box their bounds,
// its points in the box's units, its gradient's ends left where they lay.
void
set_polygon(Json& o, const std::vector<CGPoint>& pts)
{
  if (pts.empty()) {
    return;
  }
  const double ox0 = finite_or(o, "x0", 0), oy0 = finite_or(o, "y0", 0);
  const double ow = finite_or(o, "x1", 0) - ox0;
  const double oh = finite_or(o, "y1", 0) - oy0;
  double x0 = pts[0].x, y0 = pts[0].y, x1 = x0, y1 = y0;
  for (const auto& p : pts) {
    x0 = std::min(x0, p.x);
    y0 = std::min(y0, p.y);
    x1 = std::max(x1, p.x);
    y1 = std::max(y1, p.y);
  }
  const double w = x1 - x0, h = y1 - y0;
  auto unit = [&](double v, double lo, double size) {
    return std::abs(size) > 1e-6 ? (v - lo) / size : 0.5;
  };
  if (Json g = jget(o, "gradient", Json()); g.is_object()) {
    for (const char* k : {"from", "to"}) {
      const Json e = jget(g, k, Json());
      if (e.is_array() && e.size() == 2 && e[0].is_number() &&
          e[1].is_number()) {
        const double cx = ox0 + e[0].get<double>() * ow;
        const double cy = oy0 + e[1].get<double>() * oh;
        g[k] = {unit(cx, x0, w), unit(cy, y0, h)};
      }
    }
    o["gradient"] = std::move(g);
  }
  Json units = Json::array();
  for (const auto& p : pts) {
    units.push_back({unit(p.x, x0, w), unit(p.y, y0, h)});
  }
  o["points"] = std::move(units);
  o["x0"] = x0;
  o["y0"] = y0;
  o["x1"] = x1;
  o["y1"] = y1;
}

// An anchored object's turn and flips made its points': a line's ends, a
// path's points where they are shown (its gradient's ends with them).
void
bake_transform(Json& o)
{
  const CGAffineTransform t = object_transform(o);
  if (CGAffineTransformIsIdentity(t)) {
    return;
  }
  if (jget<std::string>(o, "kind", "") == "line") {
    const CGPoint a = CGPointApplyAffineTransform(
        CGPointMake(finite_or(o, "x0", 0), finite_or(o, "y0", 0)), t);
    const CGPoint b = CGPointApplyAffineTransform(
        CGPointMake(finite_or(o, "x1", 0), finite_or(o, "y1", 0)), t);
    o["x0"] = a.x;
    o["y0"] = a.y;
    o["x1"] = b.x;
    o["y1"] = b.y;
  } else {
    auto pts = own_points(o);
    for (auto& p : pts) {
      p = CGPointApplyAffineTransform(p, t);
    }
    // Its gradient's ends where they were shown, in its own pixels now.
    if (Json g = jget(o, "gradient", Json()); g.is_object()) {
      const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
      const double w = finite_or(o, "x1", 0) - x0;
      const double h = finite_or(o, "y1", 0) - y0;
      for (const char* k : {"from", "to"}) {
        const Json e = jget(g, k, Json());
        if (e.is_array() && e.size() == 2 && e[0].is_number() &&
            e[1].is_number()) {
          const CGPoint c = CGPointApplyAffineTransform(
              CGPointMake(x0 + e[0].get<double>() * w,
                          y0 + e[1].get<double>() * h), t);
          // Back into the old box's units: set_polygon moves them on.
          g[k] = {std::abs(w) > 1e-6 ? (c.x - x0) / w : 0.5,
                  std::abs(h) > 1e-6 ? (c.y - y0) / h : 0.5};
        }
      }
      o["gradient"] = std::move(g);
    }
    set_polygon(o, pts);
  }
  o.erase("rotate");
  o.erase("flip_x");
  o.erase("flip_y");
}

// `o`'s point `k` put at `p` (canvas pixels: it has no turn); whether it
// moved.
bool
put_point(Json& o, int k, CGPoint p)
{
  constexpr double kNear = 1e-6;
  if (jget<std::string>(o, "kind", "") == "line") {
    const char* xk = k == 0 ? "x0" : "x1";
    const char* yk = k == 0 ? "y0" : "y1";
    if (std::abs(finite_or(o, xk, 0) - p.x) < kNear &&
        std::abs(finite_or(o, yk, 0) - p.y) < kNear) {
      return false;
    }
    o[xk] = p.x;
    o[yk] = p.y;
    return true;
  }
  auto pts = own_points(o);
  if (k < 0 || static_cast<std::size_t>(k) >= pts.size()) {
    return false;
  }
  auto& q = pts[static_cast<std::size_t>(k)];
  if (std::abs(q.x - p.x) < kNear && std::abs(q.y - p.y) < kNear) {
    return false;
  }
  q = p;
  set_polygon(o, pts);
  return true;
}

// How many points an anchored object has: a line's two ends, a path's.
int
point_count(const Json& o)
{
  return jget<std::string>(o, "kind", "") == "line"
             ? 2
             : static_cast<int>(jget(o, "points", Json::array()).size());
}

}  // namespace

std::vector<ConnectorPoint>
connector_points(const Json& o)
{
  std::vector<ConnectorPoint> out;
  if (!takes_connectors(jget<std::string>(o, "kind", ""))) {
    return out;
  }
  const CGAffineTransform t = object_transform(o);
  for (const auto& c : jget(o, "connectors", Json::array())) {
    const auto p = own_connector(o, jget(c, "seg", 0),
                                 clamp01(finite_or(c, "t", 0)));
    if (p) {
      const CGPoint s = CGPointApplyAffineTransform(*p, t);
      out.push_back({jget<std::string>(c, "id", ""), s.x, s.y});
    }
  }
  return out;
}

Json
with_default_connectors(const Json& object)
{
  Json o = object;
  const auto kind = jget<std::string>(o, "kind", "");
  if (!takes_connectors(kind)) {
    return o;
  }
  std::vector<std::pair<int, double>> wanted;
  if (kind == "ellipse") {
    for (const double t : {0.0, 0.25, 0.5, 0.75}) {
      wanted.emplace_back(0, t);
    }
  } else {
    const auto pieces = outline_pieces(o);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      wanted.emplace_back(static_cast<int>(i), piece_middle(pieces[i]));
    }
  }
  Json have = jget(o, "connectors", Json::array());
  if (!have.is_array()) {
    have = Json::array();
  }
  std::set<std::string> ids;
  for (const auto& c : have) {
    ids.insert(jget<std::string>(c, "id", ""));
  }
  int next = 1;
  for (const auto& [seg, t] : wanted) {
    bool taken = false;
    for (const auto& c : have) {
      const double u = finite_or(c, "t", 0);
      // Round an ellipse, 0 and 1 are one place.
      const double d = kind == "ellipse"
                           ? std::min(std::abs(u - t), 1 - std::abs(u - t))
                           : std::abs(u - t);
      taken = taken || (jget(c, "seg", 0) == seg && d < 0.01);
    }
    if (taken) {
      continue;
    }
    std::string id;
    while (id.empty() || ids.count(id)) {
      id = std::format("c{}", next++);
    }
    ids.insert(id);
    have.push_back({{"id", id}, {"seg", seg}, {"t", t}});
  }
  o["connectors"] = std::move(have);
  return o;
}

namespace {

// Object `i` of `out` with its anchored points on their connectors now
// (resolve_anchors): an anchor whose object or connector is gone, or its
// own, dropped; its turn made its points'. Whether a point moved.
bool
resolve_one(Json& out, std::size_t i,
            const std::map<std::string, std::size_t>& by_id)
{
  Json& o = out[i];
  if (!o.contains("anchors")) {
    return false;
  }
  Json kept = Json::array();
  std::vector<std::pair<int, CGPoint>> at;
  for (const auto& a : jget(o, "anchors", Json::array())) {
    const auto it = by_id.find(jget<std::string>(a, "to", ""));
    if (it == by_id.end() || it->second == i) {
      continue;
    }
    const auto p = connector_at(out[it->second],
                                jget<std::string>(a, "connector", ""));
    if (!p) {
      continue;
    }
    kept.push_back(a);
    at.emplace_back(jget(a, "point", 0), *p);
  }
  if (kept.empty()) {
    o.erase("anchors");
    return false;
  }
  if (kept.size() != o["anchors"].size()) {
    o["anchors"] = kept;
  }
  bake_transform(o);
  bool moved = false;
  for (const auto& [k, p] : at) {
    moved = put_point(o, k, p) || moved;
  }
  return moved;
}

std::map<std::string, std::size_t>
index_of(const Json& out)
{
  std::map<std::string, std::size_t> by_id;
  for (std::size_t i = 0; i < out.size(); ++i) {
    by_id[jget<std::string>(out[i], "id", "")] = i;
  }
  return by_id;
}

}  // namespace

Json
resolve_anchors(const Json& objects)
{
  Json out = objects.is_array() ? objects : Json::array();
  const auto by_id = index_of(out);
  // Through chains -- a line anchored to a line anchored to a shape --
  // a pass a link, a few at most (a loop of anchors settles somewhere).
  const std::size_t passes = std::clamp<std::size_t>(out.size(), 1, 8);
  for (std::size_t pass = 0; pass < passes; ++pass) {
    bool moved = false;
    for (std::size_t i = 0; i < out.size(); ++i) {
      moved = resolve_one(out, i, by_id) || moved;
    }
    if (!moved) {
      break;
    }
  }
  return out;
}

Json
resolve_anchors(Json objects, const std::set<std::string>& moved,
                std::set<std::string>& touched)
{
  Json out = std::move(objects);
  if (!out.is_array()) {
    out = Json::array();
  }
  const auto by_id = index_of(out);
  // Who is anchored to whom: an object's id -> those anchored to it.
  std::map<std::string, std::vector<std::size_t>> anchored_to;
  for (std::size_t i = 0; i < out.size(); ++i) {
    for (const auto& a : jget(out[i], "anchors", Json::array())) {
      anchored_to[jget<std::string>(a, "to", "")].push_back(i);
    }
  }
  // The moved ones still there, and what hangs on them, through chains.
  std::set<std::size_t> reach;
  std::vector<std::string> todo(moved.begin(), moved.end());
  std::set<std::string> seen(moved.begin(), moved.end());
  while (!todo.empty()) {
    const std::string id = std::move(todo.back());
    todo.pop_back();
    if (auto it = by_id.find(id); it != by_id.end()) {
      reach.insert(it->second);
    }
    if (auto it = anchored_to.find(id); it != anchored_to.end()) {
      for (const auto i : it->second) {
        const auto dep = jget<std::string>(out[i], "id", "");
        reach.insert(i);
        if (seen.insert(dep).second) {
          todo.push_back(dep);
        }
      }
    }
  }
  const std::size_t passes = std::clamp<std::size_t>(reach.size(), 1, 8);
  for (std::size_t pass = 0; pass < passes; ++pass) {
    bool any = false;
    for (const auto i : reach) {
      any = resolve_one(out, i, by_id) || any;
    }
    if (!any) {
      break;
    }
  }
  for (const auto i : reach) {
    touched.insert(jget<std::string>(out[i], "id", ""));
  }
  return out;
}

Json
detach_moved(const Json& objects, const std::set<std::string>& moved)
{
  Json out = objects.is_array() ? objects : Json::array();
  for (auto& o : out) {
    if (!moved.count(jget<std::string>(o, "id", "")) ||
        !o.contains("anchors")) {
      continue;
    }
    Json kept = Json::array();
    for (const auto& a : o["anchors"]) {
      if (moved.count(jget<std::string>(a, "to", ""))) {
        kept.push_back(a);
      }
    }
    if (kept.empty()) {
      o.erase("anchors");
    } else {
      o["anchors"] = std::move(kept);
    }
  }
  return resolve_anchors(out);
}

namespace {

// An object moved whole: its corners (its points and gradient are in its
// box's units).
void
move_object(Json& o, double dx, double dy)
{
  for (const char* k : {"x0", "x1"}) {
    o[k] = finite_or(o, k, 0) + dx;
  }
  for (const char* k : {"y0", "y1"}) {
    o[k] = finite_or(o, k, 0) + dy;
  }
}

}  // namespace

Result<Json>
arrange_objects(const Json& objects, const std::vector<std::string>& ids,
                const std::string& how, PixelSize canvas)
{
  Json out = objects.is_array() ? objects : Json::array();
  const std::set<std::string> named(ids.begin(), ids.end());
  const std::size_t n = out.size();
  std::vector<bool> sel(n);
  std::vector<std::size_t> picked;
  for (std::size_t i = 0; i < n; ++i) {
    sel[i] = named.count(jget<std::string>(out[i], "id", "")) > 0;
    if (sel[i]) {
      picked.push_back(i);
    }
  }
  if (how == "to-top" || how == "to-bottom") {
    Json moved = Json::array(), rest = Json::array();
    for (std::size_t i = 0; i < n; ++i) {
      (sel[i] ? moved : rest).push_back(out[i]);
    }
    Json r = Json::array();
    for (auto* part : how == "to-top" ? std::array{&rest, &moved}
                                      : std::array{&moved, &rest}) {
      for (auto& o : *part) {
        r.push_back(std::move(o));
      }
    }
    return r;
  }
  if (how == "up" || how == "down") {
    // Each past its neighbour above (below) unless that one moves too:
    // from the far end, so a run moves as one.
    auto pass = [&](std::size_t lo) {
      std::size_t hi = lo + 1;
      if (how == "up" ? sel[lo] && !sel[hi] : sel[hi] && !sel[lo]) {
        std::swap(out[lo], out[hi]);
        std::swap(sel[lo], sel[hi]);
      }
    };
    if (how == "up") {
      for (std::size_t lo = n > 0 ? n - 1 : 0; lo-- > 0;) {
        pass(lo);
      }
    } else {
      for (std::size_t lo = 0; lo + 1 < n; ++lo) {
        pass(lo);
      }
    }
    return out;
  }
  // Their bounds, x, y, w, h.
  std::vector<std::array<double, 4>> b;
  for (const auto i : picked) {
    b.push_back(object_bounds(out[i]));
  }
  const bool across = how == "left" || how == "center" || how == "right" ||
                      how == "distribute-h";
  const int ax = across ? 0 : 1;  // the axis: x or y
  if (how == "left" || how == "center" || how == "right" || how == "top" ||
      how == "middle" || how == "bottom") {
    if (picked.empty()) {
      return out;
    }
    // Onto their bounds together -- or, one alone, the canvas.
    double lo = 0, hi = across ? canvas.width : canvas.height;
    if (picked.size() > 1) {
      lo = std::numeric_limits<double>::infinity();
      hi = -lo;
      for (const auto& r : b) {
        lo = std::min(lo, r[ax]);
        hi = std::max(hi, r[ax] + r[ax + 2]);
      }
    }
    const int edge = how == "left" || how == "top"      ? 0
                     : how == "right" || how == "bottom" ? 2
                                                         : 1;
    for (std::size_t k = 0; k < picked.size(); ++k) {
      const double start = b[k][ax], size = b[k][ax + 2];
      const double d = edge == 0   ? lo - start
                       : edge == 2 ? hi - (start + size)
                                   : (lo + hi) / 2 - (start + size / 2);
      move_object(out[picked[k]], across ? d : 0, across ? 0 : d);
    }
    // Moved whole: let go of the shapes they were not moved with.
    return detach_moved(out, named);
  }
  if (how == "distribute-h" || how == "distribute-v") {
    if (picked.size() < 3) {
      return out;
    }
    // In order of their middles; the first and the last stay.
    std::vector<std::size_t> order(picked.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
      order[k] = k;
    }
    std::stable_sort(order.begin(), order.end(),
                     [&](std::size_t p, std::size_t q) {
                       return b[p][ax] + b[p][ax + 2] / 2 <
                              b[q][ax] + b[q][ax + 2] / 2;
                     });
    const auto& first = b[order.front()];
    const auto& last = b[order.back()];
    double sizes = 0;
    for (const auto& r : b) {
      sizes += r[ax + 2];
    }
    const double gap = (last[ax] + last[ax + 2] - first[ax] - sizes) /
                       static_cast<double>(order.size() - 1);
    double at = first[ax];
    for (const auto k : order) {
      const double d = at - b[k][ax];
      move_object(out[picked[k]], across ? d : 0, across ? 0 : d);
      at += b[k][ax + 2] + gap;
    }
    return detach_moved(out, named);
  }
  return make_error(Code::InvalidArgument,
                    std::format("cannot arrange objects by '{}'", how));
}

Stroke
stroke_from_json(const Json& j)
{
  Stroke s;
  for (const auto& p : jget(j, "points", Json::array())) {
    if (p.is_array() && p.size() == 2 && p[0].is_number() &&
        p[1].is_number()) {
      const double x = p[0].get<double>(), y = p[1].get<double>();
      if (std::isfinite(x) && std::isfinite(y)) {
        s.points.push_back({x, y});
      }
    }
  }
  s.radius = std::clamp(finite_or(j, "radius", 8), 0.5, 2000.0);
  s.softness = clamp01(finite_or(j, "softness", 0));
  s.color = rgba_of(jget(j, "color", Json()), {0, 0, 0, 1});
  s.erase = jget(j, "erase", false);
  return s;
}

namespace {

// An object normalize_objects keeps: of a known kind -- a lifted drawing
// with its picture's blob (its box and that, no file: whoever draws it
// says where its blob is).
bool
kept_kind(const Json& o)
{
  const auto kind = jget<std::string>(o, "kind", "");
  if (kind != "line" && kind != "rect" && kind != "ellipse" &&
      kind != "polygon" && kind != "text" && kind != "drawing") {
    return false;
  }
  const Json image = jget(o, "image", Json());
  return kind != "drawing" ||
         (image.is_object() &&
          jget<std::string>(image, "hash", "").size() == 64);
}

// One object as it is kept (kept_kind), its id `id`: every field there
// and in range. Its anchors are not resolved (resolve_anchors).
Json
normalize_one(const Json& o, const std::string& id)
{
  const auto kind = jget<std::string>(o, "kind", "");
  // Its turn, within half a turn either way, and its flips (absent:
  // none).
  auto turned = [&o](Json& n) {
    double turn = std::fmod(finite_or(o, "rotate", 0), 360.0);
    if (turn > 180) {
      turn -= 360;
    } else if (turn <= -180) {
      turn += 360;
    }
    if (std::abs(turn) > 1e-9) {
      n["rotate"] = turn;
    }
    for (const char* k : {"flip_x", "flip_y"}) {
      if (jget(o, k, false)) {
        n[k] = true;
      }
    }
  };
  if (kind == "drawing") {
    const Json image = jget(o, "image", Json());
    Json n = {{"id", id},
              {"kind", kind},
              {"x0", finite_or(o, "x0", 0)},
              {"y0", finite_or(o, "y0", 0)},
              {"x1", finite_or(o, "x1", 0)},
              {"y1", finite_or(o, "y1", 0)},
              {"image", {{"hash", jget<std::string>(image, "hash", "")},
                         {"size", jget<std::uint64_t>(image, "size", 0)},
                         {"ext", jget<std::string>(image, "ext", "png")}}}};
    turned(n);
    return n;
  }
  auto rgba = [](const std::array<double, 4>& c) {
    return Json::array({c[0], c[1], c[2], c[3]});
  };
  Json n = {
    {"id", id},
    {"kind", kind},
    {"x0", finite_or(o, "x0", 0)},
    {"y0", finite_or(o, "y0", 0)},
    {"x1", finite_or(o, "x1", 0)},
    {"y1", finite_or(o, "y1", 0)},
    {"stroke", rgba(rgba_of(jget(o, "stroke", Json()), {0, 0, 0, 1}))},
    {"fill", rgba(rgba_of(jget(o, "fill", Json()), {0, 0, 0, 0}))},
    {"width", std::clamp(finite_or(o, "width", 4), 0.0, 1000.0)},
  };
  turned(n);
  // A line's or an outline's dashes; a line's arrowheads (absent:
  // solid, none).
  if (kind != "text") {
    const auto d = jget<std::string>(o, "dash", "");
    if (d == "dashed" || d == "dotted") {
      n["dash"] = d;
    }
  }
  if (kind == "line") {
    const auto a = jget<std::string>(o, "arrows", "");
    if (a == "start" || a == "end" || a == "both") {
      n["arrows"] = a;
    }
  }
  if (kind == "text") {
    const Json f = jget(o, "font", Json::object());
    n["text"] = jget<std::string>(o, "text", "");
    n["box"] = jget(o, "box", false);
    n["font"] = {
      {"family", jget<std::string>(f, "family", "Helvetica Neue")},
      {"size", std::clamp(finite_or(f, "size", 48), 1.0, 2000.0)},
      {"bold", jget(f, "bold", false)},
      {"italic", jget(f, "italic", false)},
      {"underline", jget(f, "underline", false)},
    };
    // Its paragraph style, where it has one (absent: as before).
    if (o.contains("align")) {
      const auto a = jget<std::string>(o, "align", "left");
      n["align"] = a == "center" || a == "right" || a == "justified"
                       ? a : std::string("left");
    }
    if (o.contains("line_spacing")) {
      n["line_spacing"] =
          std::clamp(finite_or(o, "line_spacing", 1), 1.0, 3.0);
    }
    if (o.contains("paragraph_spacing")) {
      n["paragraph_spacing"] =
          std::clamp(finite_or(o, "paragraph_spacing", 0), 0.0, 2000.0);
    }
    if (jget(o, "justify_last", false)) {
      n["justify_last"] = true;
    }
  }
  // A polygon's points, its box's units -- a PATH's smooth ones marked
  // (none: all corners, as before), and whether it is open.
  if (kind == "polygon") {
    Json pts = Json::array(), smooth = Json::array();
    const Json all = jget(o, "points", Json::array());
    const Json flags = jget(o, "smooth", Json::array());
    bool curved = false;
    for (std::size_t i = 0; i < all.size(); ++i) {
      const Json& p = all[i];
      if (p.is_array() && p.size() == 2 && p[0].is_number() &&
          p[1].is_number() && std::isfinite(p[0].get<double>()) &&
          std::isfinite(p[1].get<double>())) {
        pts.push_back({p[0].get<double>(), p[1].get<double>()});
        const bool s = flags.is_array() && i < flags.size() &&
                       flags[i].is_boolean() && flags[i].get<bool>();
        smooth.push_back(s);
        curved = curved || s;
      }
    }
    n["points"] = std::move(pts);
    if (curved) {
      n["smooth"] = std::move(smooth);
    }
    if (jget(o, "open", false)) {
      n["open"] = true;
    }
  }
  // Its gradient (a shape's inside, a text's words): its stops sorted
  // and held, at least two; its ends where they were put.
  if (const Json g = jget(o, "gradient", Json());
      g.is_object() && kind != "line") {
    Json stops = Json::array();
    for (const auto& st : jget(g, "stops", Json::array())) {
      stops.push_back(
          {{"at", clamp01(finite_or(st, "at", 0))},
           {"color", rgba(rgba_of(jget(st, "color", Json()),
                                  {0, 0, 0, 1}))}});
    }
    std::stable_sort(stops.begin(), stops.end(),
                     [](const Json& a, const Json& b) {
                       return jget(a, "at", 0.0) < jget(b, "at", 0.0);
                     });
    if (stops.size() >= 2) {
      const bool radial =
          jget<std::string>(g, "kind", "linear") == "radial";
      Json kept = {{"kind", radial ? "radial" : "linear"},
                   {"stops", std::move(stops)}};
      for (const char* k : {"from", "to"}) {
        const Json p = jget(g, k, Json());
        if (p.is_array() && p.size() == 2 && p[0].is_number() &&
            p[1].is_number() && std::isfinite(p[0].get<double>()) &&
            std::isfinite(p[1].get<double>())) {
          kept[k] = {p[0].get<double>(), p[1].get<double>()};
        }
      }
      n["gradient"] = std::move(kept);
    }
  }
  // Its effects: a shadow's and a glow's values held to their ranges;
  // a kind not known here kept as it is (a later Valtz's).
  if (const Json fx = jget(o, "effects", Json()); fx.is_array()) {
    Json kept = Json::array();
    for (const auto& e : fx) {
      const auto k = jget<std::string>(e, "kind", "");
      const auto c = rgba(rgba_of(jget(e, "color", Json()),
                                  {0, 0, 0, 0.5}));
      if (k == "shadow") {
        kept.push_back(
            {{"kind", k}, {"color", c},
             {"blur", std::clamp(finite_or(e, "blur", 8), 0.0, 1000.0)},
             {"dx", std::clamp(finite_or(e, "dx", 0), -4000.0, 4000.0)},
             {"dy", std::clamp(finite_or(e, "dy", 0), -4000.0, 4000.0)}});
      } else if (k == "glow") {
        kept.push_back({{"kind", k}, {"color", c},
                        {"radius", std::clamp(finite_or(e, "radius", 12),
                                              0.0, 1000.0)}});
      } else if (!k.empty() && e.is_object()) {
        kept.push_back(e);
      }
    }
    if (!kept.empty()) {
      n["effects"] = std::move(kept);
    }
  }
  // A shape's words: its text, font and paragraph style, as a text
  // box's, and their colour (absent: none, black, centred).
  if (kind == "rect" || kind == "ellipse" || kind == "polygon") {
    if (auto t = jget<std::string>(o, "text", ""); !t.empty()) {
      const Json f = jget(o, "font", Json::object());
      n["text"] = std::move(t);
      n["font"] = {
        {"family", jget<std::string>(f, "family", "Helvetica Neue")},
        {"size", std::clamp(finite_or(f, "size", 48), 1.0, 2000.0)},
        {"bold", jget(f, "bold", false)},
        {"italic", jget(f, "italic", false)},
        {"underline", jget(f, "underline", false)},
      };
      if (o.contains("align")) {
        const auto a = jget<std::string>(o, "align", "center");
        n["align"] = a == "left" || a == "right" || a == "justified"
                         ? a : std::string("center");
      }
      if (o.contains("line_spacing")) {
        n["line_spacing"] =
            std::clamp(finite_or(o, "line_spacing", 1), 1.0, 3.0);
      }
      if (o.contains("text_color")) {
        n["text_color"] =
            rgba(rgba_of(jget(o, "text_color", Json()), {0, 0, 0, 1}));
      }
    }
  }
  // Its connectors: on its outline's segments, ids its own.
  if (takes_connectors(kind)) {
    const int segs = kind == "ellipse"   ? 1
                     : kind == "line"    ? 1
                     : kind == "rect"    ? 4
                     : static_cast<int>(outline_pieces(n).size());
    Json kept = Json::array();
    std::set<std::string> cids;
    int next = 1;
    for (const auto& c : jget(o, "connectors", Json::array())) {
      const int seg = jget(c, "seg", 0);
      if (!c.is_object() || seg < 0 || seg >= segs) {
        continue;
      }
      auto cid = jget<std::string>(c, "id", "");
      while (cid.empty() || cids.count(cid)) {
        cid = std::format("c{}", next++);
      }
      cids.insert(cid);
      kept.push_back({{"id", cid},
                      {"seg", seg},
                      {"t", clamp01(finite_or(c, "t", 0))}});
    }
    if (!kept.empty()) {
      n["connectors"] = std::move(kept);
    }
  }
  // Its anchors: a point each at most (the last), to another object.
  if (takes_anchors(kind)) {
    const int points = point_count(n);
    std::map<int, Json> by_point;
    for (const auto& a : jget(o, "anchors", Json::array())) {
      const int k = jget(a, "point", -1);
      const auto to = jget<std::string>(a, "to", "");
      const auto c = jget<std::string>(a, "connector", "");
      if (k >= 0 && k < points && !to.empty() && to != id && !c.empty()) {
        by_point[k] = {{"point", k}, {"to", to}, {"connector", c}};
      }
    }
    if (!by_point.empty()) {
      Json kept = Json::array();
      for (auto& [k, a] : by_point) {
        kept.push_back(std::move(a));
      }
      n["anchors"] = std::move(kept);
    }
  }
  return n;
}

}  // namespace

Json
normalize_objects(const Json& in)
{
  Json out = Json::array();
  if (!in.is_array()) {
    return out;
  }
  std::set<std::string> ids;
  int next = 1;
  for (const auto& o : in) {
    if (!kept_kind(o)) {
      continue;
    }
    auto id = jget<std::string>(o, "id", "");
    while (id.empty() || ids.count(id)) {
      id = std::format("o{}", next++);
    }
    ids.insert(id);
    out.push_back(normalize_one(o, id));
  }
  return resolve_anchors(out);
}

Json
patch_objects(Json objects, const Json& upsert,
              const std::set<std::string>& remove,
              std::set<std::string>& changed)
{
  // In place: a page of thousands is not copied.
  Json out = std::move(objects);
  if (!out.is_array()) {
    out = Json::array();
  }
  if (!remove.empty()) {
    std::erase_if(out.get_ref<Json::array_t&>(), [&](const Json& o) {
      return remove.count(jget<std::string>(o, "id", "")) > 0;
    });
  }
  std::map<std::string, std::size_t> at;
  for (std::size_t i = 0; i < out.size(); ++i) {
    at[jget<std::string>(out[i], "id", "")] = i;
  }
  std::set<std::string> moved(remove.begin(), remove.end());
  int next = 1;
  for (const auto& u : upsert.is_array() ? upsert : Json::array()) {
    if (!kept_kind(u)) {
      continue;
    }
    auto id = jget<std::string>(u, "id", "");
    while (id.empty() || (remove.count(id) && !at.count(id))) {
      do {
        id = std::format("o{}", next++);
      } while (at.count(id));
    }
    Json n = normalize_one(u, id);
    if (auto it = at.find(id); it != at.end()) {
      out[it->second] = std::move(n);
    } else {
      at[id] = out.size();
      out.push_back(std::move(n));
    }
    moved.insert(id);
    changed.insert(id);
  }
  std::set<std::string> touched;
  out = resolve_anchors(std::move(out), moved, touched);
  changed.insert(touched.begin(), touched.end());
  return out;
}

Status
paint_stroke(const fs::path& raster, PixelSize canvas, const Stroke& s,
             const fs::path& out)
{
  if (canvas.width <= 0 || canvas.height <= 0) {
    return make_error(Code::InvalidArgument, "the canvas has no size");
  }
  @autoreleasepool {
    CGContextRef ctx = rgba_context(canvas);
    if (!ctx) {
      return make_error(Code::Internal, "cannot make the markup canvas");
    }
    if (Status st = draw_raster(ctx, canvas, raster); !st.ok()) {
      CGContextRelease(ctx);
      return st;
    }
    // The stroke's coverage: dabs joined as their maximum, so it is as
    // opaque as one dab wherever it crosses itself.
    CGColorSpaceRef gray = CGColorSpaceCreateWithName(kCGColorSpaceLinearGray);
    CGContextRef cov = CGBitmapContextCreate(
        nullptr, static_cast<std::size_t>(canvas.width),
        static_cast<std::size_t>(canvas.height), 8, 0, gray,
        kCGImageAlphaNone);
    if (!cov) {
      CGColorSpaceRelease(gray);
      CGContextRelease(ctx);
      return make_error(Code::Internal, "cannot make the stroke's mask");
    }
    CGContextSetGrayFillColor(cov, 0, 1);
    CGContextFillRect(cov, CGRectMake(0, 0, canvas.width, canvas.height));
    CGContextTranslateCTM(cov, 0, canvas.height);
    CGContextScaleCTM(cov, 1, -1);
    CGContextSetBlendMode(cov, kCGBlendModeLighten);
    const double r = s.radius;
    const double r0 = r * (1 - s.softness);
    const CGFloat comps[4] = {1, 1, 0, 1};  // white, opaque -> black
    const CGFloat locs[2] = {0, 1};
    CGGradientRef fade = CGGradientCreateWithColorComponents(gray, comps,
                                                             locs, 2);
    CGColorSpaceRelease(gray);
    auto dab = [&](double x, double y) {
      if (s.softness < 0.01) {
        CGContextSetGrayFillColor(cov, 1, 1);
        CGContextFillEllipseInRect(cov, CGRectMake(x - r, y - r, 2 * r,
                                                   2 * r));
      } else {
        // Clipped to the dab's own square: unclipped, Core Graphics
        // evaluates the shading over the whole canvas for every dab
        // (W8_shade_radial), though nothing is drawn past its outer
        // circle -- 3.6 s for a 600-point stroke on a 1024 x 1024
        // canvas, growing with the stroke. The same pixels either way.
        CGContextSaveGState(cov);
        CGContextClipToRect(cov, CGRectMake(x - r - 1, y - r - 1,
                                            2 * r + 2, 2 * r + 2));
        CGContextDrawRadialGradient(cov, fade, CGPointMake(x, y), r0,
                                    CGPointMake(x, y), r,
                                    kCGGradientDrawsBeforeStartLocation);
        CGContextRestoreGState(cov);
      }
    };
    const double step = std::max(0.5, r * 0.15);
    for (std::size_t i = 0; i < s.points.size(); ++i) {
      const auto& p = s.points[i];
      if (i == 0) {
        dab(p[0], p[1]);
        continue;
      }
      const auto& q = s.points[i - 1];
      const double len = std::hypot(p[0] - q[0], p[1] - q[1]);
      const int n = std::max(1, static_cast<int>(std::ceil(len / step)));
      for (int k = 1; k <= n; ++k) {
        const double t = static_cast<double>(k) / n;
        dab(q[0] + (p[0] - q[0]) * t, q[1] + (p[1] - q[1]) * t);
      }
    }
    CGGradientRelease(fade);
    CGImageRef mask = CGBitmapContextCreateImage(cov);
    CGContextRelease(cov);
    // The colour laid over the raster through the coverage -- or the
    // raster taken away by as much.
    const CGRect all = CGRectMake(0, 0, canvas.width, canvas.height);
    CGContextSaveGState(ctx);
    CGContextClipToMask(ctx, all, mask);
    if (s.erase) {
      CGContextSetBlendMode(ctx, kCGBlendModeDestinationOut);
      CGContextSetRGBFillColor(ctx, 0, 0, 0, 1);
    } else {
      set_fill(ctx, s.color);
    }
    CGContextFillRect(ctx, all);
    CGContextRestoreGState(ctx);
    CGImageRelease(mask);
    Status st = write_png(ctx, out);
    CGContextRelease(ctx);
    return st;
  }
}

Status
paste_drawing(const fs::path& raster, PixelSize canvas,
              const fs::path& drawing, double dx, double dy,
              const fs::path& out)
{
  if (canvas.width <= 0 || canvas.height <= 0) {
    return make_error(Code::InvalidArgument, "the canvas has no size");
  }
  @autoreleasepool {
    CGContextRef ctx = rgba_context(canvas);
    if (!ctx) {
      return make_error(Code::Internal, "cannot make the markup canvas");
    }
    Status st = draw_raster(ctx, canvas, raster);
    if (st.ok()) {
      // Canvas pixels are y-down; the context's y is up.
      CGContextSaveGState(ctx);
      CGContextTranslateCTM(ctx, dx, -dy);
      st = draw_raster(ctx, canvas, drawing);
      CGContextRestoreGState(ctx);
    }
    if (st.ok()) {
      st = write_png(ctx, out);
    }
    CGContextRelease(ctx);
    return st;
  }
}

namespace {

// A raster on its canvas, as draw_raster places it, in memory: 8-bit
// premultiplied RGBA, its first row the canvas's top.
struct CanvasPixels {
  CGContextRef   ctx = nullptr;
  std::uint8_t*  data = nullptr;
  std::size_t    row = 0;
  int            w = 0;
  int            h = 0;

  CanvasPixels() = default;
  CanvasPixels(const CanvasPixels&) = delete;
  CanvasPixels& operator=(const CanvasPixels&) = delete;
  ~CanvasPixels()
  {
    if (ctx) {
      CGContextRelease(ctx);
    }
  }

  std::uint8_t alpha(int x, int y) const
  {
    return data[static_cast<std::size_t>(y) * row +
                static_cast<std::size_t>(x) * 4 + 3];
  }
};

Status
canvas_pixels(const fs::path& raster, PixelSize canvas, CanvasPixels& c)
{
  if (canvas.width <= 0 || canvas.height <= 0) {
    return make_error(Code::InvalidArgument, "the canvas has no size");
  }
  c.ctx = rgba_context(canvas);
  if (!c.ctx) {
    return make_error(Code::Internal, "cannot make the markup canvas");
  }
  VALTZ_TRY(draw_raster(c.ctx, canvas, raster));
  c.data = static_cast<std::uint8_t*>(CGBitmapContextGetData(c.ctx));
  c.row = CGBitmapContextGetBytesPerRow(c.ctx);
  c.w = canvas.width;
  c.h = canvas.height;
  return ok_status();
}

// The drawing painted within `reach` of (x, y): its pixels marked in
// `mask` (the canvas's size; 1 its body, 2 its faint edge), its box.
DrawingBox
find_drawing(const CanvasPixels& c, double x, double y, int reach,
             std::vector<std::uint8_t>& mask)
{
  const int cx = static_cast<int>(std::floor(x));
  const int cy = static_cast<int>(std::floor(y));
  // The nearest pixel of a body.
  int seed = -1;
  int best = std::numeric_limits<int>::max();
  for (int dy = -reach; dy <= reach; ++dy) {
    for (int dx = -reach; dx <= reach; ++dx) {
      const int px = cx + dx, py = cy + dy;
      if (px < 0 || py < 0 || px >= c.w || py >= c.h ||
          c.alpha(px, py) <= kDrawingAlpha) {
        continue;
      }
      if (dx * dx + dy * dy < best) {
        best = dx * dx + dy * dy;
        seed = py * c.w + px;
      }
    }
  }
  if (seed < 0) {
    return {};
  }
  mask.assign(static_cast<std::size_t>(c.w) * c.h, 0);
  // Its body: the pixels that touch, 8-connected.
  std::vector<int> body{seed};
  mask[seed] = 1;
  for (std::size_t i = 0; i < body.size(); ++i) {
    const int px = body[i] % c.w, py = body[i] / c.w;
    for (int dy = -1; dy <= 1; ++dy) {
      for (int dx = -1; dx <= 1; ++dx) {
        const int nx = px + dx, ny = py + dy;
        if (nx < 0 || ny < 0 || nx >= c.w || ny >= c.h) {
          continue;
        }
        const int n = ny * c.w + nx;
        if (mask[n] == 0 && c.alpha(nx, ny) > kDrawingAlpha) {
          mask[n] = 1;
          body.push_back(n);
        }
      }
    }
  }
  // Its faint edge: fainter pixels reached from it, kDrawingFringe at
  // most.
  std::vector<int> front = body, next;
  for (int step = 0; step < kDrawingFringe && !front.empty(); ++step) {
    next.clear();
    for (const int i : front) {
      const int px = i % c.w, py = i / c.w;
      for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
          const int nx = px + dx, ny = py + dy;
          if (nx < 0 || ny < 0 || nx >= c.w || ny >= c.h) {
            continue;
          }
          const int n = ny * c.w + nx;
          if (mask[n] == 0 && c.alpha(nx, ny) > 0) {
            mask[n] = 2;
            next.push_back(n);
          }
        }
      }
    }
    front.swap(next);
  }
  int x0 = c.w, y0 = c.h, x1 = -1, y1 = -1;
  for (int py = 0; py < c.h; ++py) {
    const std::uint8_t* m = &mask[static_cast<std::size_t>(py) * c.w];
    for (int px = 0; px < c.w; ++px) {
      if (m[px]) {
        x0 = std::min(x0, px);
        x1 = std::max(x1, px);
        y0 = std::min(y0, py);
        y1 = std::max(y1, py);
      }
    }
  }
  return {x0, y0, x1 - x0 + 1, y1 - y0 + 1};
}

// The pixels of `c` in `box` -- those `mask` marks, without one all --
// into a PNG the box's size.
Status
write_piece(const CanvasPixels& c, const DrawingBox& box,
            const std::vector<std::uint8_t>* mask, const fs::path& out)
{
  CGContextRef p = rgba_context({box.w, box.h});
  if (!p) {
    return make_error(Code::Internal, "cannot make the drawing's picture");
  }
  auto* dst = static_cast<std::uint8_t*>(CGBitmapContextGetData(p));
  const std::size_t drow = CGBitmapContextGetBytesPerRow(p);
  for (int r = 0; r < box.h; ++r) {
    const int py = box.y + r;
    for (int col = 0; col < box.w; ++col) {
      const int px = box.x + col;
      if (mask && !(*mask)[static_cast<std::size_t>(py) * c.w + px]) {
        continue;
      }
      std::memcpy(dst + static_cast<std::size_t>(r) * drow +
                      static_cast<std::size_t>(col) * 4,
                  c.data + static_cast<std::size_t>(py) * c.row +
                      static_cast<std::size_t>(px) * 4,
                  4);
    }
  }
  Status st = write_png(p, out);
  CGContextRelease(p);
  return st;
}

}

Result<DrawingBox>
drawing_at(const fs::path& raster, PixelSize canvas, double x, double y,
           int reach)
{
  @autoreleasepool {
    CanvasPixels c;
    VALTZ_TRY(canvas_pixels(raster, canvas, c));
    std::vector<std::uint8_t> mask;
    return find_drawing(c, x, y, reach, mask);
  }
}

Result<DrawingBox>
lift_drawing(const fs::path& raster, PixelSize canvas, double x, double y,
             int reach, const fs::path& rest, const fs::path& piece)
{
  @autoreleasepool {
    CanvasPixels c;
    VALTZ_TRY(canvas_pixels(raster, canvas, c));
    std::vector<std::uint8_t> mask;
    const DrawingBox box = find_drawing(c, x, y, reach, mask);
    if (box.empty()) {
      return box;
    }
    if (!piece.empty()) {
      VALTZ_TRY(write_piece(c, box, &mask, piece));
    }
    if (!rest.empty()) {
      for (std::size_t i = 0; i < mask.size(); ++i) {
        if (mask[i]) {
          const std::size_t px = i % c.w, py = i / c.w;
          std::memset(c.data + py * c.row + px * 4, 0, 4);
        }
      }
      VALTZ_TRY(write_png(c.ctx, rest));
    }
    return box;
  }
}

Result<DrawingBox>
crop_drawing(const fs::path& png, PixelSize canvas, const fs::path& piece)
{
  @autoreleasepool {
    CanvasPixels c;
    VALTZ_TRY(canvas_pixels(png, canvas, c));
    int x0 = c.w, y0 = c.h, x1 = -1, y1 = -1;
    for (int py = 0; py < c.h; ++py) {
      for (int px = 0; px < c.w; ++px) {
        if (c.alpha(px, py) > 0) {
          x0 = std::min(x0, px);
          x1 = std::max(x1, px);
          y0 = std::min(y0, py);
          y1 = std::max(y1, py);
        }
      }
    }
    if (x1 < 0) {
      return DrawingBox{};
    }
    const DrawingBox box{x0, y0, x1 - x0 + 1, y1 - y0 + 1};
    VALTZ_TRY(write_piece(c, box, nullptr, piece));
    return box;
  }
}

Status
render_markup(const fs::path& raster, PixelSize canvas, const Json& objects,
              const std::set<std::string>& hidden, const fs::path& out)
{
  VALTZ_ASSIGN(DrawnPicture pic,
               render_markup_picture(raster, canvas, objects, hidden));
  return write_image_png(pic.get(), out);
}

Status
draw_markup(CGContextRef ctx, const fs::path& raster, PixelSize canvas,
            const Json& objects, const std::set<std::string>& hidden,
            bool vectors)
{
  VALTZ_TRY(draw_raster(ctx, canvas, raster));
  CGContextSaveGState(ctx);
  CGContextTranslateCTM(ctx, 0, canvas.height);
  CGContextScaleCTM(ctx, 1, -1);
  if (objects.is_array()) {
    for (const auto& o : objects) {
      if (!hidden.count(jget<std::string>(o, "id", ""))) {
        draw_object(ctx, o, vectors);
      }
    }
  }
  CGContextRestoreGState(ctx);
  return ok_status();
}

Result<DrawnPicture>
render_markup_picture(const fs::path& raster, PixelSize canvas,
                      const Json& objects,
                      const std::set<std::string>& hidden)
{
  if (canvas.width <= 0 || canvas.height <= 0) {
    return make_error(Code::InvalidArgument, "the canvas has no size");
  }
  @autoreleasepool {
    CGContextRef ctx = rgba_context(canvas);
    if (!ctx) {
      return make_error(Code::Internal, "cannot make the markup canvas");
    }
    if (Status st = draw_markup(ctx, raster, canvas, objects, hidden, false);
        !st.ok()) {
      CGContextRelease(ctx);
      return st.error();
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) {
      return make_error(Code::Internal, "cannot make the markup image");
    }
    return DrawnPicture(img, CGImageRelease);
  }
}

namespace {

// Where an object's pixels can fall, canvas pixels (MarkupCanvas): its
// outline's box padded for what draws past it -- a line's arrowheads, a
// path's sharp corners (its miters, five widths), its effects' layers,
// words' ink, the edge's antialiasing; null where it is not measured (a
// text without a box: anywhere).
CGRect
paint_rect(const Json& o)
{
  const CGRect r = object_rect(o);
  if (CGRectIsNull(r)) {
    return r;
  }
  const auto kind = jget<std::string>(o, "kind", "");
  const double w = std::max(0.0, finite_or(o, "width", 0));
  double pad = 2;
  for (const auto& p : passes_of(o)) {
    pad += p.blur * 2 + std::max(std::abs(p.dx), std::abs(p.dy)) + 2;
  }
  if (kind == "line" && o.contains("arrows")) {
    pad += std::max(5.0, 2 * w) + w;
  }
  if (kind == "polygon") {
    pad += 5 * w;
  }
  if (o.contains("text")) {
    pad += finite_or(jget(o, "font", Json::object()), "size", 48) * 0.3;
  }
  return CGRectIntegral(CGRectInset(r, -pad, -pad));
}

}  // namespace

MarkupCanvas::~MarkupCanvas()
{
  for (CGContextRef c : {_ctx, _scratch}) {
    if (c) {
      CGContextRelease(c);
    }
  }
}

std::optional<DrawnPicture>
MarkupCanvas::kept(const std::string& stamp, const fs::path& raster,
                   PixelSize canvas, const std::set<std::string>& hidden)
{
  std::lock_guard lk(_mu);
  if (stamp.empty() || stamp != _stamp || !_last || raster != _raster ||
      canvas.width != _size.width || canvas.height != _size.height ||
      hidden != _hidden) {
    return std::nullopt;
  }
  _whole = false;
  _area = 0;
  return _last;
}

Result<DrawnPicture>
MarkupCanvas::draw(const fs::path& raster, PixelSize canvas, Json objects,
                   const std::set<std::string>& hidden,
                   const std::string& stamp)
{
  if (canvas.width <= 0 || canvas.height <= 0) {
    return make_error(Code::InvalidArgument, "the canvas has no size");
  }
  if (!objects.is_array()) {
    objects = Json::array();
  }
  std::lock_guard lk(_mu);
  @autoreleasepool {
    const CGRect page = CGRectMake(0, 0, canvas.width, canvas.height);
    bool whole = !_ctx || canvas.width != _size.width ||
                 canvas.height != _size.height || raster != _raster;
    std::vector<CGRect> rects(objects.size());
    std::map<std::string, std::size_t> at;
    std::vector<CGRect> dirty;
    double area = 0;
    auto mark = [&](CGRect r) {
      if (CGRectIsNull(r)) {
        whole = true;
        return;
      }
      r = CGRectIntersection(r, page);
      if (!CGRectIsEmpty(r)) {
        dirty.push_back(r);
        area += r.size.width * r.size.height;
      }
    };
    // What differs from the last drawing: put in, changed, hidden or
    // shown -- where each lies now and lay then; reordered, all of it.
    std::size_t last = 0;
    for (std::size_t i = 0; i < objects.size(); ++i) {
      const auto id = jget<std::string>(objects[i], "id", "");
      at[id] = i;
      const auto was = whole ? _at.end() : _at.find(id);
      if (was != _at.end()) {
        whole = whole || was->second < last;
        last = was->second;
      }
      if (was != _at.end() && _objects[was->second] == objects[i]) {
        rects[i] = _rects[was->second];
        if (_hidden.count(id) != hidden.count(id)) {
          mark(rects[i]);
        }
        continue;
      }
      rects[i] = paint_rect(objects[i]);
      if (!whole) {
        if (was != _at.end()) {
          mark(_rects[was->second]);
        }
        mark(rects[i]);
      }
    }
    if (!whole) {
      for (const auto& [id, j] : _at) {
        if (!at.count(id)) {
          mark(_rects[j]);  // taken out
        }
      }
      whole = whole || area > page.size.width * page.size.height / 2;
    }
    if (whole) {
      if (!_ctx || canvas.width != _size.width ||
          canvas.height != _size.height) {
        if (_ctx) {
          CGContextRelease(_ctx);
        }
        if (_scratch) {
          CGContextRelease(_scratch);
          _scratch = nullptr;
        }
        _ctx = rgba_context(canvas);
        if (!_ctx) {
          return make_error(Code::Internal, "cannot make the markup canvas");
        }
      } else {
        CGContextClearRect(_ctx, page);
      }
      VALTZ_TRY(draw_markup(_ctx, raster, canvas, objects, hidden, false));
      area = page.size.width * page.size.height;
    } else if (!dirty.empty()) {
      // What changed in clusters -- the areas that touch merged, so an
      // object with effects lies whole in one -- each drawn again in a
      // second bitmap of the canvas's size: the part cleared, the raster
      // there, then EVERY object over it, whole, where it lies; only the
      // part copied in. Not under a clip, nor in a bitmap of its own (its
      // place moved): Core Graphics rounds coverage otherwise -- 1 or 2 of
      // 255 off where edges or translucent objects meet.
      std::vector<CGRect> parts = dirty;
      for (bool merged = true; merged;) {
        merged = false;
        for (std::size_t i = 0; i < parts.size() && !merged; ++i) {
          for (std::size_t k = i + 1; k < parts.size(); ++k) {
            if (CGRectIntersectsRect(parts[i], parts[k])) {
              parts[i] = CGRectUnion(parts[i], parts[k]);
              parts.erase(parts.begin() + static_cast<std::ptrdiff_t>(k));
              merged = true;
              break;
            }
          }
        }
      }
      if (!_scratch) {
        _scratch = rgba_context(canvas);
      }
      auto* dst = static_cast<std::uint8_t*>(CGBitmapContextGetData(_ctx));
      const auto* src =
          static_cast<const std::uint8_t*>(CGBitmapContextGetData(_scratch));
      if (!_scratch || !dst || !src) {
        return make_error(Code::Internal, "cannot make the markup canvas");
      }
      const std::size_t row = CGBitmapContextGetBytesPerRow(_ctx);
      const std::size_t src_row = CGBitmapContextGetBytesPerRow(_scratch);
      for (const auto& r : parts) {
        const CGRect up = CGRectMake(
            r.origin.x, canvas.height - r.origin.y - r.size.height,
            r.size.width, r.size.height);
        CGContextClearRect(_scratch, up);
        CGContextSaveGState(_scratch);
        CGContextClipToRect(_scratch, up);  // the raster's pixels alone
        const Status st = draw_raster(_scratch, canvas, raster);
        CGContextRestoreGState(_scratch);
        VALTZ_TRY(st);
        CGContextSaveGState(_scratch);
        // Not clipped: any clip set rounds coverage a level off, even far
        // from its edge -- an object over the part is drawn whole (a line
        // across the page costs its length).
        CGContextTranslateCTM(_scratch, 0, canvas.height);
        CGContextScaleCTM(_scratch, 1, -1);
        for (std::size_t i = 0; i < objects.size(); ++i) {
          if (!hidden.count(jget<std::string>(objects[i], "id", "")) &&
              CGRectIntersectsRect(r, rects[i])) {
            draw_object(_scratch, objects[i], false);
          }
        }
        CGContextRestoreGState(_scratch);
        // Its rows, the first in memory the top.
        const auto x = static_cast<std::size_t>(r.origin.x);
        const auto y = static_cast<std::size_t>(r.origin.y);
        const auto w = static_cast<std::size_t>(r.size.width);
        for (std::size_t j = 0; j < static_cast<std::size_t>(r.size.height);
             ++j) {
          std::memcpy(dst + (y + j) * row + x * 4,
                      src + (y + j) * src_row + x * 4, w * 4);
        }
      }
    }
    _whole = whole;
    _area = area;
    if (whole || !dirty.empty() || !_last) {
      CGImageRef img = CGBitmapContextCreateImage(_ctx);
      if (!img) {
        return make_error(Code::Internal, "cannot make the markup image");
      }
      _last = DrawnPicture(img, CGImageRelease);
    }
    _size = canvas;
    _raster = raster;
    _objects = std::move(objects);
    _hidden = hidden;
    _stamp = stamp;
    _rects = std::move(rects);
    _at = std::move(at);
    return _last;
  }
}

}
