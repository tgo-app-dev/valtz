#include "valtz/media/markup.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>
#include <Foundation/Foundation.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <cmath>
#include <format>

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

// One object, on a context whose CTM is canvas pixels, y down.
void
draw_object(CGContextRef ctx, const Json& o)
{
  const auto kind = jget<std::string>(o, "kind", "");
  const double x0 = finite_or(o, "x0", 0), y0 = finite_or(o, "y0", 0);
  const double x1 = finite_or(o, "x1", 0), y1 = finite_or(o, "y1", 0);
  const auto stroke = rgba_of(jget(o, "stroke", Json()), {0, 0, 0, 1});
  const auto fill = rgba_of(jget(o, "fill", Json()), {0, 0, 0, 0});
  const double width = std::max(0.0, finite_or(o, "width", 0));
  CGContextSaveGState(ctx);
  if (kind == "line") {
    if (width > 0) {
      CGContextSetLineCap(ctx, kCGLineCapRound);
      CGContextSetLineWidth(ctx, width);
      set_stroke(ctx, stroke);
      CGContextMoveToPoint(ctx, x0, y0);
      CGContextAddLineToPoint(ctx, x1, y1);
      CGContextStrokePath(ctx);
    }
  } else if (kind == "rect" || kind == "ellipse") {
    const CGRect r = CGRectStandardize(CGRectMake(x0, y0, x1 - x0, y1 - y0));
    const bool ellipse = kind == "ellipse";
    if (fill[3] > 0) {
      set_fill(ctx, fill);
      ellipse ? CGContextFillEllipseInRect(ctx, r) : CGContextFillRect(ctx, r);
    }
    if (width > 0) {
      CGContextSetLineJoin(ctx, kCGLineJoinMiter);
      CGContextSetLineWidth(ctx, width);
      set_stroke(ctx, stroke);
      ellipse ? CGContextStrokeEllipseInRect(ctx, r)
              : CGContextStrokeRect(ctx, r);
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
      // only). Laid out y up from the box's bottom-left.
      const double bx = std::min(x0, x1), by = std::min(y0, y1);
      const double bw = std::abs(x1 - x0), bh = std::abs(y1 - y0);
      NSMutableDictionary* a = [attrs mutableCopy];
      if (underline) {
        a[(__bridge NSString*)kCTUnderlineStyleAttributeName] =
            @(kCTUnderlineStyleSingle);
      }
      NSString* s = [[NSString alloc] initWithBytes:text.data()
                                             length:text.size()
                                           encoding:NSUTF8StringEncoding];
      if (s.length > 0 && bw > 0 && bh > 0) {
        NSAttributedString* as =
            [[NSAttributedString alloc] initWithString:s attributes:a];
        CTFramesetterRef fs = CTFramesetterCreateWithAttributedString(
            (__bridge CFAttributedStringRef)as);
        CGPathRef path =
            CGPathCreateWithRect(CGRectMake(0, 0, bw, bh), nullptr);
        CTFrameRef frame = CTFramesetterCreateFrame(fs, CFRangeMake(0, 0),
                                                    path, nullptr);
        CGContextTranslateCTM(ctx, bx, by + bh);
        CGContextScaleCTM(ctx, 1, -1);
        CGContextSetTextMatrix(ctx, CGAffineTransformIdentity);
        CTFrameDraw(frame, ctx);
        CFRelease(frame);
        CGPathRelease(path);
        CFRelease(fs);
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
    const auto kind = jget<std::string>(o, "kind", "");
    if (kind != "line" && kind != "rect" && kind != "ellipse" &&
        kind != "text") {
      continue;
    }
    auto id = jget<std::string>(o, "id", "");
    while (id.empty() || ids.count(id)) {
      id = std::format("o{}", next++);
    }
    ids.insert(id);
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
    }
    out.push_back(std::move(n));
  }
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
        CGContextDrawRadialGradient(cov, fade, CGPointMake(x, y), r0,
                                    CGPointMake(x, y), r,
                                    kCGGradientDrawsBeforeStartLocation);
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
render_markup(const fs::path& raster, PixelSize canvas, const Json& objects,
              const std::set<std::string>& hidden, const fs::path& out)
{
  VALTZ_ASSIGN(DrawnPicture pic,
               render_markup_picture(raster, canvas, objects, hidden));
  return write_image_png(pic.get(), out);
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
    if (Status st = draw_raster(ctx, canvas, raster); !st.ok()) {
      CGContextRelease(ctx);
      return st.error();
    }
    CGContextTranslateCTM(ctx, 0, canvas.height);
    CGContextScaleCTM(ctx, 1, -1);
    if (objects.is_array()) {
      for (const auto& o : objects) {
        if (!hidden.count(jget<std::string>(o, "id", ""))) {
          draw_object(ctx, o);
        }
      }
    }
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGContextRelease(ctx);
    if (!img) {
      return make_error(Code::Internal, "cannot make the markup image");
    }
    return DrawnPicture(img, CGImageRelease);
  }
}

}
