import CoreGraphics
import CoreText
import Foundation

/// Markup objects drawn, measured and hit -- in canvas pixels, top-left
/// origin, y down -- the way the core draws them into the picture (core
/// media/markup.mm: the same Core Graphics and Core Text calls), so the
/// objects the app draws live while they are edited look as they will
/// once they are part of the picture.
enum MarkupRender {
    /// The font a text object names: its family at its size, bold and
    /// italic where the family has them (else the nearest it has).
    static func font(_ f: MarkupFont) -> CTFont {
        let size = CGFloat(min(2000, max(1, f.size)))
        let d = CTFontDescriptorCreateWithAttributes(
            [kCTFontFamilyNameAttribute: f.family] as CFDictionary)
        let base = CTFontCreateWithFontDescriptor(d, size, nil)
        var traits: CTFontSymbolicTraits = []
        if f.bold { traits.insert(.traitBold) }
        if f.italic { traits.insert(.traitItalic) }
        guard !traits.isEmpty else { return base }
        return CTFontCreateCopyWithSymbolicTraits(
            base, size, nil, traits, [.traitBold, .traitItalic]) ?? base
    }

    /// A text BOX's words as Core Text sets them, as the core does: its
    /// font, colour, underline and paragraph style -- the line spacing an
    /// adjustment BETWEEN lines, so the first is at the box's top.
    static func boxText(_ o: MarkupObject) -> NSAttributedString {
        let f = font(o.font)
        var attrs: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): f,
            NSAttributedString.Key(
                kCTForegroundColorAttributeName as String):
                o.stroke.cgColor,
        ]
        if o.font.underline {
            attrs[NSAttributedString.Key(
                kCTUnderlineStyleAttributeName as String)] =
                CTUnderlineStyle.single.rawValue
        }
        var align: CTTextAlignment = switch o.align {
        case "center": .center
        case "right": .right
        case "justified": .justified
        default: .left
        }
        let natural = CTFontGetAscent(f) + CTFontGetDescent(f)
            + CTFontGetLeading(f)
        var between = CGFloat(min(3, max(1, o.lineSpacing ?? 1)) - 1)
            * natural
        var after = CGFloat(min(2000, max(0, o.paragraphSpacing ?? 0)))
        let style = withUnsafeBytes(of: &align) { a in
            withUnsafeBytes(of: &between) { l in
                withUnsafeBytes(of: &after) { p in
                    let settings = [
                        CTParagraphStyleSetting(
                            spec: .alignment, valueSize: a.count,
                            value: a.baseAddress!),
                        CTParagraphStyleSetting(
                            spec: .lineSpacingAdjustment,
                            valueSize: l.count, value: l.baseAddress!),
                        CTParagraphStyleSetting(
                            spec: .paragraphSpacing, valueSize: p.count,
                            value: p.baseAddress!),
                    ]
                    return CTParagraphStyleCreate(settings, settings.count)
                }
            }
        }
        attrs[NSAttributedString.Key(
            kCTParagraphStyleAttributeName as String)] = style
        return NSAttributedString(string: o.text, attributes: attrs)
    }

    /// A text's lines, each laid out, and the font's line metrics.
    private static func lines(_ o: MarkupObject)
        -> (lines: [CTLine?], font: CTFont, ascent: CGFloat, lineH: CGFloat)
    {
        let f = font(o.font)
        let ascent = CTFontGetAscent(f)
        let lineH = ascent + CTFontGetDescent(f) + CTFontGetLeading(f)
        let attrs: [NSAttributedString.Key: Any] = [
            NSAttributedString.Key(kCTFontAttributeName as String): f,
            NSAttributedString.Key(kCTForegroundColorAttributeName as String):
                o.stroke.cgColor,
        ]
        let ls: [CTLine?] = o.text.components(separatedBy: "\n").map { s in
            s.isEmpty ? nil : CTLineCreateWithAttributedString(
                NSAttributedString(string: s, attributes: attrs))
        }
        return (ls, f, ascent, lineH)
    }

    /// `o` drawn on `ctx`, whose CTM is canvas pixels, y down.
    static func draw(_ o: MarkupObject, in ctx: CGContext) {
        if o.effects.isEmpty {
            drawBody(o, in: ctx)
        } else {
            drawWithEffects(o, in: ctx)
        }
    }

    /// An object with EFFECTS, as the core draws it: a shadow is measured
    /// in a bitmap's pixels, y up -- a view's context measures it
    /// otherwise -- so the object and its effects are drawn into a bitmap
    /// of their own at the context's resolution, then laid in.
    private static func drawWithEffects(_ o: MarkupObject,
                                        in ctx: CGContext) {
        let passes = self.passes(o)
        var pad = 0.0
        for p in passes { pad += p.blur * 2 + max(abs(p.dx), abs(p.dy)) }
        let body = bounds(o)
        guard !body.isEmpty else { return drawBody(o, in: ctx) }
        let r = body.insetBy(dx: -pad - 2, dy: -pad - 2).integral
        // Device pixels a canvas pixel, held to a bitmap of 4096 at most.
        let t = ctx.userSpaceToDeviceSpaceTransform
        var s = (abs(t.a * t.d - t.b * t.c)).squareRoot()
        s = min(max(s, 0.05), 4096 / max(r.width, r.height))
        let w = Int((r.width * s).rounded(.up))
        let h = Int((r.height * s).rounded(.up))
        guard w > 0, h > 0,
              let b = CGContext(data: nil, width: w, height: h,
                                bitsPerComponent: 8, bytesPerRow: 0,
                                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                bitmapInfo: CGImageAlphaInfo
                                    .premultipliedLast.rawValue)
        else { return drawBody(o, in: ctx) }
        // Canvas pixels, y down, as the core's markup canvas.
        b.translateBy(x: 0, y: CGFloat(h))
        b.scaleBy(x: s, y: -s)
        b.translateBy(x: -r.minX, y: -r.minY)
        drawEffects(passes, around: o, rect: r, in: b)
        guard let img = b.makeImage() else { return }
        ctx.saveGState()
        ctx.translateBy(x: r.minX, y: r.maxY)
        ctx.scaleBy(x: 1, y: -1)
        ctx.draw(img, in: CGRect(origin: .zero, size: r.size))
        ctx.restoreGState()
    }

    /// An effect's pass: a shadow (dx, dy) canvas pixels off, blurred.
    private struct Pass {
        var dx: Double, dy: Double, blur: Double, color: RGBA
    }

    /// Its effects as shadow passes, innermost first: a shadow one, a
    /// glow two at no offset (core media/markup.mm's passes_of).
    private static func passes(_ o: MarkupObject) -> [Pass] {
        o.effects.flatMap { e -> [Pass] in
            switch e.kind {
            case .shadow:
                [Pass(dx: e.dx, dy: e.dy, blur: max(0, e.blur),
                      color: e.color)]
            case .glow:
                Array(repeating: Pass(dx: 0, dy: 0, blur: max(0, e.radius),
                                      color: e.color), count: 2)
            }
        }
    }

    /// The core's draw_object, in a BITMAP `b`: each pass a transparency
    /// layer composited with its shadow -- the outermost begun first --
    /// the object drawn once inside them all; the offset through the CTM
    /// into device space, whose rows run down, while a shadow's base
    /// space runs up.
    private static func drawEffects(_ passes: [Pass], around o: MarkupObject,
                                    rect: CGRect, in b: CGContext) {
        let t = b.userSpaceToDeviceSpaceTransform
        let scale = (abs(t.a * t.d - t.b * t.c)).squareRoot()
        for p in passes.reversed() {
            b.saveGState()
            let a = b.convertToDeviceSpace(CGPoint.zero)
            let e = b.convertToDeviceSpace(CGPoint(x: p.dx, y: p.dy))
            b.setShadow(offset: CGSize(width: e.x - a.x, height: a.y - e.y),
                        blur: p.blur * scale, color: p.color.cgColor)
            b.beginTransparencyLayer(in: rect, auxiliaryInfo: nil)
        }
        drawBody(o, in: b)
        for _ in passes {
            b.endTransparencyLayer()
            b.restoreGState()
        }
    }

    /// `o` itself, without its effects.
    private static func drawBody(_ o: MarkupObject, in ctx: CGContext) {
        ctx.saveGState()
        defer { ctx.restoreGState() }
        switch o.kind {
        case .line:
            guard o.width > 0 else { return }
            ctx.setLineCap(.round)
            ctx.setLineWidth(o.width)
            ctx.setStrokeColor(o.stroke.cgColor)
            ctx.move(to: CGPoint(x: o.x0, y: o.y0))
            ctx.addLine(to: CGPoint(x: o.x1, y: o.y1))
            ctx.strokePath()
        case .rect, .ellipse, .polygon:
            // Its inside -- a gradient through its outline as a clip, else
            // its flat fill -- then its outline (the core's draw_body).
            let path = shapePath(o)
            if let g = o.gradient, g.stops.count >= 2 {
                ctx.saveGState()
                ctx.addPath(path)
                ctx.clip()
                drawGradient(g, of: o, in: ctx)
                ctx.restoreGState()
            } else if o.fill.a > 0 {
                ctx.setFillColor(o.fill.cgColor)
                ctx.addPath(path)
                ctx.fillPath()
            }
            if o.width > 0 {
                ctx.setLineJoin(.miter)
                ctx.setLineWidth(o.width)
                ctx.setStrokeColor(o.stroke.cgColor)
                ctx.addPath(path)
                ctx.strokePath()
            }
        case .text where o.box:
            // A text BOX: wrapped within its width, only the lines that
            // fit whole in its height (a framesetter sets whole lines
            // only) -- laid out y up from the box's bottom-left, as the
            // core does.
            let b = box(o)
            guard !o.text.isEmpty, b.width > 0, b.height > 0 else { return }
            // A gradient colours the words: drawn in a layer of their own,
            // the gradient laid only where they are (source-in).
            let grad = o.gradient.flatMap { $0.stops.count >= 2 ? $0 : nil }
            if grad != nil {
                let pad = CGFloat(o.font.size)
                ctx.beginTransparencyLayer(in: b.insetBy(dx: -pad, dy: -pad),
                                           auxiliaryInfo: nil)
            }
            defer {
                if let grad {
                    ctx.saveGState()
                    ctx.setBlendMode(.sourceIn)
                    drawGradient(grad, of: o, in: ctx)
                    ctx.restoreGState()
                    ctx.endTransparencyLayer()
                }
            }
            ctx.saveGState()
            defer { ctx.restoreGState() }
            let fs = CTFramesetterCreateWithAttributedString(boxText(o))
            let frame = CTFramesetterCreateFrame(
                fs, CFRange(location: 0, length: 0),
                CGPath(rect: CGRect(x: 0, y: 0, width: b.width,
                                    height: b.height), transform: nil),
                nil)
            ctx.translateBy(x: b.minX, y: b.maxY)
            ctx.scaleBy(x: 1, y: -1)
            ctx.textMatrix = .identity
            // A line at a time at the frame's origins, as CTFrameDraw:
            // a justified paragraph running on in another box has its
            // last line stretched too.
            let lines = CTFrameGetLines(frame) as? [CTLine] ?? []
            var origins = [CGPoint](repeating: .zero, count: lines.count)
            CTFrameGetLineOrigins(frame, CFRange(location: 0, length: 0),
                                  &origins)
            let length = (o.text as NSString).length
            let stretch = o.justifyLast && o.align == "justified"
            for (i, l) in lines.enumerated() {
                var drawn = l
                let r = CTLineGetStringRange(l)
                if stretch, i == lines.count - 1,
                   r.location + r.length >= length,
                   let j = CTLineCreateJustifiedLine(l, 1.0,
                                                     Double(b.width)) {
                    drawn = j
                }
                ctx.textPosition = origins[i]
                CTLineDraw(drawn, ctx)
            }
        case .text:
            let t = lines(o)
            // Glyphs are drawn y up: flip the text matrix against the
            // y-down CTM.
            ctx.textMatrix = CGAffineTransform(scaleX: 1, y: -1)
            for (i, l) in t.lines.enumerated() {
                guard let l else { continue }
                let baseline = o.y0 + t.ascent + CGFloat(i) * t.lineH
                ctx.textPosition = CGPoint(x: o.x0, y: baseline)
                CTLineDraw(l, ctx)
                if o.font.underline {
                    let w = CTLineGetTypographicBounds(l, nil, nil, nil)
                    let th = max(1, CTFontGetUnderlineThickness(t.font))
                    let y = baseline - CTFontGetUnderlinePosition(t.font)
                    ctx.setFillColor(o.stroke.cgColor)
                    ctx.fill(CGRect(x: o.x0, y: y - th / 2, width: w,
                                    height: th))
                }
            }
        }
    }

    /// A rectangle's or ellipse's box (its corners in any order).
    static func box(_ o: MarkupObject) -> CGRect {
        CGRect(x: o.x0, y: o.y0, width: o.x1 - o.x0, height: o.y1 - o.y0)
            .standardized
    }

    /// A point in `o`'s box's units, on the canvas: 0 at (x0, y0), 1 at
    /// (x1, y1) -- as a polygon's corners and a gradient's ends are kept.
    static func onCanvas(_ u: CGPoint, of o: MarkupObject) -> CGPoint {
        CGPoint(x: o.x0 + u.x * (o.x1 - o.x0), y: o.y0 + u.y * (o.y1 - o.y0))
    }

    /// A canvas point in `o`'s box's units (a box with no width or height
    /// takes its middle).
    static func inBox(_ p: CGPoint, of o: MarkupObject) -> CGPoint {
        let (w, h) = (o.x1 - o.x0, o.y1 - o.y0)
        return CGPoint(x: abs(w) > 1e-6 ? (p.x - o.x0) / w : 0.5,
                       y: abs(h) > 1e-6 ? (p.y - o.y0) / h : 0.5)
    }

    /// A polygon's corners on the canvas.
    static func polygonPoints(_ o: MarkupObject) -> [CGPoint] {
        o.points.map { onCanvas($0, of: o) }
    }

    /// `o` made a polygon through `pts` (canvas pixels): its box their
    /// bounds, its points in its units.
    static func setPolygon(_ o: inout MarkupObject, _ pts: [CGPoint]) {
        let xs = pts.map(\.x), ys = pts.map(\.y)
        guard let x0 = xs.min(), let x1 = xs.max(),
              let y0 = ys.min(), let y1 = ys.max() else { return }
        (o.x0, o.y0, o.x1, o.y1) = (x0, y0, x1, y1)
        o.points = pts.map { inBox($0, of: o) }
    }

    /// A rectangle's, an ellipse's or a polygon's outline (the core's
    /// shape_path).
    static func shapePath(_ o: MarkupObject) -> CGPath {
        let path = CGMutablePath()
        switch o.kind {
        case .polygon:
            let pts = polygonPoints(o)
            if let first = pts.first {
                path.move(to: first)
                for p in pts.dropFirst() { path.addLine(to: p) }
                path.closeSubpath()
            }
        case .ellipse:
            path.addEllipse(in: box(o))
        default:
            path.addRect(box(o))
        }
        return path
    }

    /// `g` drawn over what the clip leaves, its ends in `o`'s box's units,
    /// carried on past them (the core's draw_gradient).
    static func drawGradient(_ g: MarkupGradient, of o: MarkupObject,
                             in ctx: CGContext) {
        let stops = g.stops.sorted { $0.at < $1.at }
        guard let cg = CGGradient(
            colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!,
            colorComponents: stops.flatMap {
                [$0.color.r, $0.color.g, $0.color.b, $0.color.a]
                    .map { CGFloat($0) }
            },
            locations: stops.map { CGFloat(min(1, max(0, $0.at))) },
            count: stops.count) else { return }
        let a = onCanvas(g.from, of: o), b = onCanvas(g.to, of: o)
        switch g.kind {
        case .linear:
            ctx.drawLinearGradient(cg, start: a, end: b,
                                   options: [.drawsBeforeStartLocation,
                                             .drawsAfterEndLocation])
        case .radial:
            ctx.drawRadialGradient(cg, startCenter: a, startRadius: 0,
                                   endCenter: a,
                                   endRadius: hypot(b.x - a.x, b.y - a.y),
                                   options: [.drawsAfterEndLocation])
        }
    }

    /// A gradient's two handles on the canvas: a linear one's ends, a
    /// radial one's centre and a point on its edge.
    static func gradientHandles(_ o: MarkupObject) -> [CGPoint] {
        guard let g = o.gradient else { return [] }
        return [onCanvas(g.from, of: o), onCanvas(g.to, of: o)]
    }

    /// `o` with its gradient's handle `i` (0 from, 1 to) at `p`.
    static func movingGradient(_ o: MarkupObject, handle i: Int,
                               to p: CGPoint) -> MarkupObject {
        var o = o
        guard var g = o.gradient else { return o }
        let u = inBox(p, of: o)
        if i == 0 { g.from = u } else { g.to = u }
        o.gradient = g
        return o
    }

    /// A line's height in a text's font: a text box is at least one.
    static func lineHeight(_ f: MarkupFont) -> CGFloat {
        let ft = font(f)
        return CTFontGetAscent(ft) + CTFontGetDescent(ft)
            + CTFontGetLeading(ft)
    }

    /// What `o` covers, its stroke included.
    static func bounds(_ o: MarkupObject) -> CGRect {
        switch o.kind {
        case .text where o.box:
            return box(o)
        case .text:
            let t = lines(o)
            let w = t.lines.map { l in
                l.map { CGFloat(CTLineGetTypographicBounds($0, nil, nil,
                                                            nil)) } ?? 0
            }.max() ?? 0
            return CGRect(x: o.x0, y: o.y0,
                          width: max(w, CGFloat(o.font.size) * 0.3),
                          height: t.lineH * CGFloat(max(1, t.lines.count)))
        default:
            return box(o).insetBy(dx: -o.width / 2, dy: -o.width / 2)
        }
    }

    /// Whether a point hits `o`, within `tolerance` canvas pixels: a
    /// filled shape anywhere inside, an outline or line on its stroke, a
    /// text in its box.
    static func hits(_ o: MarkupObject, _ p: CGPoint,
                     tolerance: Double) -> Bool {
        let reach = o.width / 2 + tolerance
        switch o.kind {
        case .line:
            return distance(p, CGPoint(x: o.x0, y: o.y0),
                            CGPoint(x: o.x1, y: o.y1)) <= reach
        case .rect:
            let r = box(o)
            if (o.fill.a > 0 || o.gradient != nil) && r.contains(p) {
                return true
            }
            let out = r.insetBy(dx: -reach, dy: -reach)
            let inner = r.insetBy(dx: reach, dy: reach)
            return out.contains(p) && (inner.isNull || !inner.contains(p))
        case .ellipse:
            let r = box(o)
            let (rx, ry) = (max(1, r.width / 2), max(1, r.height / 2))
            let nx = (p.x - r.midX) / rx, ny = (p.y - r.midY) / ry
            let d = (nx * nx + ny * ny).squareRoot()
            if (o.fill.a > 0 || o.gradient != nil) && d <= 1 { return true }
            return abs(d - 1) * min(rx, ry) <= reach
        case .polygon:
            // Inside (filled), or near an edge.
            let pts = polygonPoints(o)
            if (o.fill.a > 0 || o.gradient != nil),
               shapePath(o).contains(p) {
                return true
            }
            guard pts.count > 1 else { return false }
            return zip(pts, pts.dropFirst() + [pts[0]]).contains {
                distance(p, $0.0, $0.1) <= reach
            }
        case .text:
            return bounds(o).insetBy(dx: -tolerance, dy: -tolerance)
                .contains(p)
        }
    }

    /// Where its handles are: a line's two ends, a box's four corners --
    /// (x0, y0), (x1, y0), (x1, y1), (x0, y1) -- a text's its box's (one
    /// from before: around what it shows); a polygon's corners first (one
    /// on its box's corner is the polygon's), then its box's.
    static func handles(_ o: MarkupObject) -> [CGPoint] {
        switch o.kind {
        case .line:
            return [CGPoint(x: o.x0, y: o.y0), CGPoint(x: o.x1, y: o.y1)]
        case .polygon:
            return polygonPoints(o)
                + [CGPoint(x: o.x0, y: o.y0), CGPoint(x: o.x1, y: o.y0),
                   CGPoint(x: o.x1, y: o.y1), CGPoint(x: o.x0, y: o.y1)]
        case .rect, .ellipse:
            return [CGPoint(x: o.x0, y: o.y0), CGPoint(x: o.x1, y: o.y0),
                    CGPoint(x: o.x1, y: o.y1), CGPoint(x: o.x0, y: o.y1)]
        case .text:
            let b = bounds(o)
            return [CGPoint(x: b.minX, y: b.minY),
                    CGPoint(x: b.maxX, y: b.minY),
                    CGPoint(x: b.maxX, y: b.maxY),
                    CGPoint(x: b.minX, y: b.maxY)]
        }
    }

    /// `o` with handle `i` (handles(_:)) at `p`. A text becomes a box
    /// (one from before, around what it showed), at least a character
    /// wide and a line tall -- a shorter one would show nothing.
    static func moving(_ o: MarkupObject, handle i: Int,
                       to p: CGPoint) -> MarkupObject {
        var o = o
        if o.kind == .text {
            let b = bounds(o)
            (o.x0, o.y0, o.x1, o.y1) = (b.minX, b.minY, b.maxX, b.maxY)
            o.box = true
            let minW = CGFloat(o.font.size) * 0.6
            let minH = lineHeight(o.font)
            switch i {
            case 0: (o.x0, o.y0) = (min(p.x, o.x1 - minW),
                                    min(p.y, o.y1 - minH))
            case 1: (o.x1, o.y0) = (max(p.x, o.x0 + minW),
                                    min(p.y, o.y1 - minH))
            case 2: (o.x1, o.y1) = (max(p.x, o.x0 + minW),
                                    max(p.y, o.y0 + minH))
            default: (o.x0, o.y1) = (min(p.x, o.x1 - minW),
                                     max(p.y, o.y0 + minH))
            }
            return o
        }
        if o.kind == .polygon {
            // A corner moved: the box made again round them all, the
            // gradient's ends kept where they were on the canvas.
            let n = o.points.count
            if i < n {
                var pts = polygonPoints(o)
                pts[i] = p
                let ends = gradientHandles(o)
                setPolygon(&o, pts)
                if ends.count == 2, var g = o.gradient {
                    g.from = inBox(ends[0], of: o)
                    g.to = inBox(ends[1], of: o)
                    o.gradient = g
                }
                return o
            }
            switch i - n {
            case 0: (o.x0, o.y0) = (p.x, p.y)
            case 1: (o.x1, o.y0) = (p.x, p.y)
            case 2: (o.x1, o.y1) = (p.x, p.y)
            default: (o.x0, o.y1) = (p.x, p.y)
            }
            return o
        }
        switch (o.kind, i) {
        case (.line, 0): (o.x0, o.y0) = (p.x, p.y)
        case (.line, _): (o.x1, o.y1) = (p.x, p.y)
        case (_, 0): (o.x0, o.y0) = (p.x, p.y)
        case (_, 1): (o.x1, o.y0) = (p.x, p.y)
        case (_, 2): (o.x1, o.y1) = (p.x, p.y)
        default: (o.x0, o.y1) = (p.x, p.y)
        }
        return o
    }

    private static func distance(_ p: CGPoint, _ a: CGPoint,
                                 _ b: CGPoint) -> Double {
        let (dx, dy) = (b.x - a.x, b.y - a.y)
        let len2 = dx * dx + dy * dy
        let t = len2 > 0
            ? max(0, min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2))
            : 0
        let q = CGPoint(x: a.x + t * dx, y: a.y + t * dy)
        return ((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y))
            .squareRoot()
    }
}
