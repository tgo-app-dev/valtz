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
        case .rect, .ellipse:
            let r = box(o)
            if o.fill.a > 0 {
                ctx.setFillColor(o.fill.cgColor)
                o.kind == .ellipse ? ctx.fillEllipse(in: r) : ctx.fill(r)
            }
            if o.width > 0 {
                ctx.setLineJoin(.miter)
                ctx.setLineWidth(o.width)
                ctx.setStrokeColor(o.stroke.cgColor)
                o.kind == .ellipse ? ctx.strokeEllipse(in: r)
                                   : ctx.stroke(r)
            }
        case .text where o.box:
            // A text BOX: wrapped within its width, only the lines that
            // fit whole in its height (a framesetter sets whole lines
            // only) -- laid out y up from the box's bottom-left, as the
            // core does.
            let b = box(o)
            guard !o.text.isEmpty, b.width > 0, b.height > 0 else { return }
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
            let fs = CTFramesetterCreateWithAttributedString(
                NSAttributedString(string: o.text, attributes: attrs))
            let frame = CTFramesetterCreateFrame(
                fs, CFRange(location: 0, length: 0),
                CGPath(rect: CGRect(x: 0, y: 0, width: b.width,
                                    height: b.height), transform: nil),
                nil)
            ctx.translateBy(x: b.minX, y: b.maxY)
            ctx.scaleBy(x: 1, y: -1)
            ctx.textMatrix = .identity
            CTFrameDraw(frame, ctx)
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
            if o.fill.a > 0 && r.contains(p) { return true }
            let out = r.insetBy(dx: -reach, dy: -reach)
            let inner = r.insetBy(dx: reach, dy: reach)
            return out.contains(p) && (inner.isNull || !inner.contains(p))
        case .ellipse:
            let r = box(o)
            let (rx, ry) = (max(1, r.width / 2), max(1, r.height / 2))
            let nx = (p.x - r.midX) / rx, ny = (p.y - r.midY) / ry
            let d = (nx * nx + ny * ny).squareRoot()
            if o.fill.a > 0 && d <= 1 { return true }
            return abs(d - 1) * min(rx, ry) <= reach
        case .text:
            return bounds(o).insetBy(dx: -tolerance, dy: -tolerance)
                .contains(p)
        }
    }

    /// Where its handles are: a line's two ends, a box's four corners --
    /// (x0, y0), (x1, y0), (x1, y1), (x0, y1) -- a text's its box's (one
    /// from before: around what it shows).
    static func handles(_ o: MarkupObject) -> [CGPoint] {
        switch o.kind {
        case .line:
            return [CGPoint(x: o.x0, y: o.y0), CGPoint(x: o.x1, y: o.y1)]
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
