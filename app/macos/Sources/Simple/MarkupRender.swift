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
    /// Lifted drawings' pictures, by their blob's hash: read by the model
    /// as one is selected (AppModel.loadDrawingImages), drawn from here.
    static let drawingImages = DrawingImageCache()

    /// A line's or outline's dashes, in its own widths (the core's
    /// set_dash): dashes three long and two apart, square-ended; round
    /// dots two apart.
    private static func setDash(_ o: MarkupObject, in ctx: CGContext) {
        let w = max(1, o.width)
        switch o.dash {
        case "dashed":
            ctx.setLineDash(phase: 0, lengths: [3 * w, 2 * w])
            ctx.setLineCap(.butt)
        case "dotted":
            ctx.setLineDash(phase: 0, lengths: [0, 2 * w])
            ctx.setLineCap(.round)
        default: break
        }
    }

    /// An arrowhead with its tip at `tip`, pointing along (ux, uy).
    private static func arrowhead(at tip: CGPoint, _ ux: Double,
                                  _ uy: Double, _ len: Double,
                                  _ half: Double, in ctx: CGContext) {
        let b = CGPoint(x: tip.x - ux * len, y: tip.y - uy * len)
        ctx.move(to: tip)
        ctx.addLine(to: CGPoint(x: b.x - uy * half, y: b.y + ux * half))
        ctx.addLine(to: CGPoint(x: b.x + uy * half, y: b.y - ux * half))
        ctx.closePath()
        ctx.fillPath()
    }

    private static func drawBody(_ o: MarkupObject, in ctx: CGContext) {
        ctx.saveGState()
        defer { ctx.restoreGState() }
        // Its turn and flips: all of it below in its own pixels.
        ctx.concatenate(transform(o))
        switch o.kind {
        case .drawing:
            // Its pixels in its box, upright on the y-down canvas.
            guard let hash = o.image?.hash,
                  let img = drawingImages[hash] else { return }
            let b = box(o)
            ctx.translateBy(x: b.minX, y: b.maxY)
            ctx.scaleBy(x: 1, y: -1)
            ctx.draw(img, in: CGRect(origin: .zero, size: b.size))
        case .line:
            guard o.width > 0 else { return }
            // Its arrowheads, the line stopped inside each (the core's
            // draw_body: four widths long and wide, 10 at least).
            let at0 = o.arrows == "start" || o.arrows == "both"
            let at1 = o.arrows == "end" || o.arrows == "both"
            let dx = o.x1 - o.x0, dy = o.y1 - o.y0
            let run = (dx * dx + dy * dy).squareRoot()
            let ux = run > 0 ? dx / run : 1, uy = run > 0 ? dy / run : 0
            let heads = (at0 ? 1 : 0) + (at1 ? 1 : 0)
            var len = max(10, 4 * o.width)
            if heads > 0 && run > 0 { len = min(len, run / Double(heads)) }
            let half = max(5, 2 * o.width)
            let inset = len * 0.8
            ctx.setLineCap(.round)
            ctx.setLineWidth(o.width)
            setDash(o, in: ctx)
            ctx.setStrokeColor(o.stroke.cgColor)
            ctx.move(to: CGPoint(x: o.x0 + (at0 ? ux * inset : 0),
                                 y: o.y0 + (at0 ? uy * inset : 0)))
            ctx.addLine(to: CGPoint(x: o.x1 - (at1 ? ux * inset : 0),
                                    y: o.y1 - (at1 ? uy * inset : 0)))
            ctx.strokePath()
            if heads > 0 && run > 0 {
                ctx.setFillColor(o.stroke.cgColor)
                if at1 {
                    arrowhead(at: CGPoint(x: o.x1, y: o.y1), ux, uy, len,
                              half, in: ctx)
                }
                if at0 {
                    arrowhead(at: CGPoint(x: o.x0, y: o.y0), -ux, -uy, len,
                              half, in: ctx)
                }
            }
        case .rect, .ellipse, .polygon:
            // Its inside -- a gradient through its outline as a clip, else
            // its flat fill -- then its outline (the core's draw_body). An
            // OPEN path has no inside: a line, its ends and turns round.
            let path = shapePath(o)
            let open = o.kind == .polygon && o.open
            if !open, let g = o.gradient, g.stops.count >= 2 {
                ctx.saveGState()
                ctx.addPath(path)
                ctx.clip()
                drawGradient(g, of: o, in: ctx)
                ctx.restoreGState()
            } else if !open && o.fill.a > 0 {
                ctx.setFillColor(o.fill.cgColor)
                ctx.addPath(path)
                ctx.fillPath()
            }
            if o.width > 0 {
                ctx.setLineJoin(open ? .round : .miter)
                if open { ctx.setLineCap(.round) }
                ctx.setLineWidth(o.width)
                setDash(o, in: ctx)
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

    // MARK: Its turn and flips

    // An object is kept in its OWN pixels -- corners, ends, points,
    // gradient, words -- and drawn flipped, then turned, about its box's
    // centre (the core's object_transform). What the pointer touches and
    // the stage outlines is on the canvas: hits, handles, points and
    // bounds below take and give canvas pixels, the edits made in its
    // own and put back where they were (keepingPlace).

    /// Its own pixels to the canvas; the identity unturned, unflipped.
    static func transform(_ o: MarkupObject) -> CGAffineTransform {
        guard o.rotate != 0 || o.flipX || o.flipY else { return .identity }
        let c = centre(o)
        return CGAffineTransform(translationX: c.x, y: c.y)
            .rotated(by: o.rotate * .pi / 180)
            .scaledBy(x: o.flipX ? -1 : 1, y: o.flipY ? -1 : 1)
            .translatedBy(x: -c.x, y: -c.y)
    }

    /// What it turns about: its box's centre -- a line's ends', a
    /// polygon's points' (not its curve's bulge) -- a text without a box,
    /// of its lines.
    static func centre(_ o: MarkupObject) -> CGPoint {
        if o.kind == .text && !o.box {
            let b = ownBounds(o)
            return CGPoint(x: b.midX, y: b.midY)
        }
        return CGPoint(x: (o.x0 + o.x1) / 2, y: (o.y0 + o.y1) / 2)
    }

    /// A canvas point in `o`'s own pixels.
    static func own(_ p: CGPoint, of o: MarkupObject) -> CGPoint {
        p.applying(transform(o).inverted())
    }

    /// `after` -- `before` changed in its own pixels (a corner, a point,
    /// its box) -- moved so that what the change left alone stays where
    /// it was on the canvas: its centre, and so its turn's, moved with
    /// the change; the rest lies (R - 1)(c' - c) off unless put back.
    static func keepingPlace(_ before: MarkupObject,
                             _ after: MarkupObject) -> MarkupObject {
        let t = transform(before)
        guard !t.isIdentity else { return after }
        let (c, c2) = (centre(before), centre(after))
        let d = CGPoint(x: c2.x - c.x, y: c2.y - c.y)
        return offset(after, dx: t.a * d.x + t.c * d.y - d.x,
                      dy: t.b * d.x + t.d * d.y - d.y)
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

    /// A polygon's corners in its own pixels (its box's units laid out).
    static func polygonPoints(_ o: MarkupObject) -> [CGPoint] {
        o.points.map { onCanvas($0, of: o) }
    }

    /// Its corners where they are shown, turned and flipped.
    static func shownPoints(_ o: MarkupObject) -> [CGPoint] {
        let t = transform(o)
        return polygonPoints(o).map { $0.applying(t) }
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

    /// A cubic Bezier piece of a path: from `a` to `b` by `c1`, `c2`.
    struct Piece {
        var a, c1, c2, b: CGPoint
        var straight: Bool { c1 == a && c2 == b }

        func at(_ t: CGFloat) -> CGPoint {
            let u = 1 - t
            let (k0, k1, k2, k3) = (u * u * u, 3 * u * u * t, 3 * u * t * t,
                                    t * t * t)
            return CGPoint(x: k0 * a.x + k1 * c1.x + k2 * c2.x + k3 * b.x,
                           y: k0 * a.y + k1 * c1.y + k2 * c2.y + k3 * b.y)
        }
    }

    /// The share of a segment a smooth point's handle reaches along it,
    /// for a turn of `theta` there (the core's handle_share): a circle's
    /// arc through points evenly round it, the turn held to 120°.
    static func handleShare(_ theta: CGFloat) -> CGFloat {
        let t = min(theta, 2 * .pi / 3)
        if t < 1e-4 { return 1.0 / 3 }
        return 2.0 / 3 * tan(t / 4) / sin(t / 2)
    }

    /// A PATH's pieces through `p` (canvas pixels), as the core works them
    /// out (path_pieces): a smooth point's tangent along the line from
    /// the point before it to the one after, its handles each a share of
    /// the segment it reaches into; a corner -- an open path's ends --
    /// none.
    static func pieces(_ p: [CGPoint], smooth: [Bool],
                       closed: Bool) -> [Piece] {
        let n = p.count
        var inH = p, outH = p
        for i in 0..<n {
            guard i < smooth.count, smooth[i], n >= 3,
                  closed || (i > 0 && i + 1 < n) else { continue }
            let a = p[(i + n - 1) % n], b = p[(i + 1) % n]
            let dx = b.x - a.x, dy = b.y - a.y
            let len = hypot(dx, dy)
            guard len >= 1e-9 else { continue }
            let vx = p[i].x - a.x, vy = p[i].y - a.y
            let wx = b.x - p[i].x, wy = b.y - p[i].y
            let turn = atan2(abs(vx * wy - vy * wx), vx * wx + vy * wy)
            let f = handleShare(turn)
            let back = hypot(vx, vy) * f / len
            let on = hypot(wx, wy) * f / len
            inH[i] = CGPoint(x: p[i].x - dx * back, y: p[i].y - dy * back)
            outH[i] = CGPoint(x: p[i].x + dx * on, y: p[i].y + dy * on)
        }
        var out: [Piece] = []
        var i = 0
        while i + 1 < n || (closed && i < n && n > 1) {
            let j = (i + 1) % n
            out.append(Piece(a: p[i], c1: outH[i], c2: inH[j], b: p[j]))
            i += 1
        }
        return out
    }

    /// The path through `p` -- closed, or open -- as a CGPath.
    static func path(_ p: [CGPoint], smooth: [Bool],
                     closed: Bool) -> CGMutablePath {
        let path = CGMutablePath()
        guard let first = p.first else { return path }
        path.move(to: first)
        let ps = pieces(p, smooth: smooth, closed: closed)
        for (i, c) in ps.enumerated() {
            if c.straight {
                // The closing edge: the subpath's close draws it.
                if closed && i == ps.count - 1 { break }
                path.addLine(to: c.b)
            } else {
                path.addCurve(to: c.b, control1: c.c1, control2: c.c2)
            }
        }
        if closed { path.closeSubpath() }
        return path
    }

    /// A rectangle's, an ellipse's or a polygon's outline (the core's
    /// shape_path) -- a path through its smooth points, open or closed.
    static func shapePath(_ o: MarkupObject) -> CGPath {
        let path = CGMutablePath()
        switch o.kind {
        case .polygon:
            return Self.path(polygonPoints(o), smooth: o.smooth,
                             closed: !o.open)
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
        let t = transform(o)
        return ownGradientHandles(o).map { $0.applying(t) }
    }

    /// The same in its own pixels.
    private static func ownGradientHandles(_ o: MarkupObject) -> [CGPoint] {
        guard let g = o.gradient else { return [] }
        return [onCanvas(g.from, of: o), onCanvas(g.to, of: o)]
    }

    /// `o` with its gradient's handle `i` (0 from, 1 to) at `p`.
    static func movingGradient(_ o: MarkupObject, handle i: Int,
                               to p: CGPoint) -> MarkupObject {
        var o = o
        guard var g = o.gradient else { return o }
        let u = inBox(own(p, of: o), of: o)
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

    /// Where `o` lies, as Information gives it: its own box -- a line's
    /// ends', a text's lines' -- its stroke left out, before its turn
    /// and flips (Information's rotation and signs).
    static func frame(_ o: MarkupObject) -> CGRect {
        o.kind == .text && !o.box ? ownBounds(o) : box(o)
    }

    /// The frame round all of `objects`; nil for none.
    static func frame(_ objects: [MarkupObject]) -> CGRect? {
        objects.map(frame).reduce(nil) { $0?.union($1) ?? $1 }
    }

    /// The frame round all of `objects` as they are shown -- turned and
    /// flipped: what the guides' magnet lines up; nil for none.
    static func shownFrame(_ objects: [MarkupObject]) -> CGRect? {
        objects.map { frame($0).applying(transform($0)) }
            .reduce(nil) { $0?.union($1) ?? $1 }
    }

    /// `o` moved by (dx, dy).
    static func offset(_ o: MarkupObject, dx: Double,
                       dy: Double) -> MarkupObject {
        var o = o
        o.x0 += dx
        o.x1 += dx
        o.y0 += dy
        o.y1 += dy
        return o
    }

    /// `o` laid from frame `from` onto `to` -- moved and scaled, each axis
    /// on its own, its corners, ends, polygon and gradient with it. A text
    /// becomes a box (one from before: round what it showed); an axis a
    /// frame has none of (a level line's height) only moves.
    static func reframed(_ o: MarkupObject, from: CGRect,
                         to: CGRect) -> MarkupObject {
        var o = o
        if o.kind == .text && !o.box {
            let b = ownBounds(o)
            (o.x0, o.y0, o.x1, o.y1) = (b.minX, b.minY, b.maxX, b.maxY)
            o.box = true
        }
        func mapX(_ x: Double) -> Double {
            from.width > 1e-6
                ? to.minX + (x - from.minX) * to.width / from.width
                : x + to.minX - from.minX
        }
        func mapY(_ y: Double) -> Double {
            from.height > 1e-6
                ? to.minY + (y - from.minY) * to.height / from.height
                : y + to.minY - from.minY
        }
        (o.x0, o.x1) = (mapX(o.x0), mapX(o.x1))
        (o.y0, o.y1) = (mapY(o.y0), mapY(o.y1))
        return o
    }

    /// What `o` covers on the canvas, its stroke included: its own
    /// bounds turned and flipped, the box round them.
    static func bounds(_ o: MarkupObject) -> CGRect {
        ownBounds(o).applying(transform(o))
    }

    /// What `o` covers in its own pixels, its stroke included.
    static func ownBounds(_ o: MarkupObject) -> CGRect {
        switch o.kind {
        case .drawing:
            return box(o)
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
        case .polygon:
            // A curve bulges past its points' box.
            return box(o).union(shapePath(o).boundingBoxOfPath)
                .insetBy(dx: -o.width / 2, dy: -o.width / 2)
        default:
            return box(o).insetBy(dx: -o.width / 2, dy: -o.width / 2)
        }
    }

    /// Whether a point hits `o`, within `tolerance` canvas pixels: a
    /// filled shape anywhere inside, an outline or line on its stroke, a
    /// text in its box.
    static func hits(_ o: MarkupObject, _ p: CGPoint,
                     tolerance: Double) -> Bool {
        let p = own(p, of: o)
        let reach = o.width / 2 + tolerance
        switch o.kind {
        case .drawing:
            return box(o).insetBy(dx: -tolerance, dy: -tolerance).contains(p)
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
            // Inside (filled, closed), or near its outline.
            if o.fillable, o.fill.a > 0 || o.gradient != nil,
               shapePath(o).contains(p) {
                return true
            }
            return (nearestOwn(o, to: p)?.distance ?? .infinity) <= reach
        case .text:
            return ownBounds(o).insetBy(dx: -tolerance, dy: -tolerance)
                .contains(p)
        }
    }

    /// Where its handles are: a line's two ends, a box's four corners --
    /// (x0, y0), (x1, y0), (x1, y1), (x0, y1) -- a text's its box's (one
    /// from before: around what it shows); a polygon's corners first (one
    /// on its box's corner is the polygon's), then its box's.
    static func handles(_ o: MarkupObject) -> [CGPoint] {
        let t = transform(o)
        return ownHandles(o).map { $0.applying(t) }
    }

    /// The same in its own pixels.
    private static func ownHandles(_ o: MarkupObject) -> [CGPoint] {
        switch o.kind {
        case .drawing:
            return []  // moved whole, not reshaped
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
            let b = ownBounds(o)
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
        keepingPlace(o, movingOwn(o, handle: i, to: own(p, of: o)))
    }

    /// The same, `p` in its own pixels.
    private static func movingOwn(_ o: MarkupObject, handle i: Int,
                                  to p: CGPoint) -> MarkupObject {
        var o = o
        if o.kind == .text {
            let b = ownBounds(o)
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
                let ends = ownGradientHandles(o)
                setPolygon(&o, pts)
                keepGradient(&o, ends)
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
        let q = closest(p, a, b)
        return hypot(p.x - q.x, p.y - q.y)
    }

    /// The point of segment `a`-`b` nearest `p`.
    private static func closest(_ p: CGPoint, _ a: CGPoint,
                                _ b: CGPoint) -> CGPoint {
        let (dx, dy) = (b.x - a.x, b.y - a.y)
        let len2 = dx * dx + dy * dy
        let t = len2 > 0
            ? max(0, min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2))
            : 0
        return CGPoint(x: a.x + t * dx, y: a.y + t * dy)
    }

    // MARK: Paths, as the curvature pen edits them

    /// Where a path's outline comes nearest `p`: the piece (from its point
    /// `piece` to the next), the point on it, how far -- its curves
    /// followed in short straight steps.
    static func nearest(_ o: MarkupObject, to p: CGPoint)
        -> (piece: Int, at: CGPoint, distance: Double)?
    {
        guard let n = nearestOwn(o, to: own(p, of: o)) else { return nil }
        return (n.piece, n.at.applying(transform(o)), n.distance)
    }

    /// The same in its own pixels.
    private static func nearestOwn(_ o: MarkupObject, to p: CGPoint)
        -> (piece: Int, at: CGPoint, distance: Double)?
    {
        let ps = pieces(polygonPoints(o), smooth: o.smooth, closed: !o.open)
        var best: (piece: Int, at: CGPoint, distance: Double)?
        for (k, c) in ps.enumerated() {
            let steps = c.straight ? 1 : 48
            var from = c.a
            for s in 1...steps {
                let to = s == steps ? c.b
                    : c.at(CGFloat(s) / CGFloat(steps))
                let q = closest(p, from, to)
                let d = Double(hypot(p.x - q.x, p.y - q.y))
                if d < (best?.distance ?? .infinity) { best = (k, q, d) }
                from = to
            }
        }
        return best
    }

    /// The path `o` with a SMOOTH point at `p` (canvas pixels) after its
    /// point `k` -- on the piece from it to the next -- the curve made
    /// through it; its gradient's ends kept where they were.
    static func addingPoint(_ o: MarkupObject, after k: Int,
                            at p: CGPoint) -> MarkupObject {
        let before = o
        var o = o
        var pts = polygonPoints(o)
        var flags = pts.indices.map { o.isSmooth($0) }
        let at = min(k + 1, pts.count)
        pts.insert(own(p, of: o), at: at)
        flags.insert(true, at: at)
        let ends = ownGradientHandles(o)
        setPolygon(&o, pts)
        o.smooth = flags
        keepGradient(&o, ends)
        return keepingPlace(before, o)
    }

    /// The path `o` with its point `i` a corner if it was smooth, smooth
    /// if a corner.
    static func togglingPoint(_ o: MarkupObject, _ i: Int) -> MarkupObject {
        var o = o
        guard o.points.indices.contains(i) else { return o }
        var flags = o.points.indices.map { o.isSmooth($0) }
        flags[i].toggle()
        o.smooth = flags.contains(true) ? flags : []
        return o
    }

    /// A gradient's ends put back where they were on the canvas, its box
    /// made again.
    private static func keepGradient(_ o: inout MarkupObject,
                                     _ ends: [CGPoint]) {
        guard ends.count == 2, var g = o.gradient else { return }
        g.from = inBox(ends[0], of: o)
        g.to = inBox(ends[1], of: o)
        o.gradient = g
    }
}

/// Lifted drawings' pictures by their blob's hash -- read on the main
/// thread, drawn wherever an overlay draws.
final class DrawingImageCache: @unchecked Sendable {
    private let lock = NSLock()
    private var images: [String: CGImage] = [:]

    subscript(hash: String) -> CGImage? {
        get { lock.withLock { images[hash] } }
        set { lock.withLock { images[hash] = newValue } }
    }
}
