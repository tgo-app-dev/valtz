import AppKit

/// What the markup toolbar shows over the picture while it works: the
/// brush stroke being drawn (until the picture made with it comes back),
/// the objects being drawn or edited -- drawn here, live, and left out of
/// the picture the core makes meanwhile -- and the selected ones'
/// outlines and handles.
struct MarkupOverlay: Equatable {
    struct Stroke: Equatable {
        var points: [CGPoint]
        var radius: Double
        var softness: Double
        var color: RGBA
        var erase: Bool
    }
    /// The strokes let go but not yet in the picture, then the one being
    /// drawn.
    var strokes: [Stroke] = []
    var objects: [MarkupObject] = []
    var selected: Set<String> = []
    /// The selected objects drawn here too (while they change), not only
    /// outlined: otherwise the picture under has them in their place.
    var drawsSelection = true
    /// The selected DRAWING's pixels' bounds (canvas pixels), outlined.
    var drawingBounds: CGRect?
    /// A polygon being drawn: its corners (canvas pixels), the last where
    /// the pointer is -- drawn as a path, its first corner ringed. A
    /// curvature pen's (`pathCurved`): the curve through them, each point
    /// smooth or a corner (`pathSmooth`).
    var path: [CGPoint] = []
    var pathSmooth: [Bool] = []
    var pathCurved = false
    /// The pen is the tool: a selected path shows its points -- smooth
    /// round, corners square -- to drag and switch, not its box's handles.
    var editsPath = false
    /// The eyedropper: where it is (canvas pixels), the colour there and
    /// the one it would replace -- a ring round the pointer, the new
    /// above, the current below.
    var sampleAt: CGPoint?
    var sampleColor: RGBA?
    var sampleNow: RGBA?
    /// The object whose words the in-place editor shows (MarkupTextEditor):
    /// drawn without them, outlined without handles.
    var editing: String?
    /// CONNECTORS shown (canvas pixels): the selection's, or -- a line, a
    /// path or a point of one being placed -- those it could anchor to;
    /// `hotConnector` the one it would take, there.
    var connectors: [CGPoint] = []
    var hotConnector: CGPoint?

    var isEmpty: Bool {
        strokes.isEmpty && objects.isEmpty && drawingBounds == nil
            && path.isEmpty && sampleAt == nil && connectors.isEmpty
            && hotConnector == nil
    }
}

/// A pointer on the picture, for the markup toolbar: where (canvas
/// pixels, top-left origin), with which keys, and how many canvas pixels
/// a view point is (a hit's reach is measured on screen).
struct MarkupPointer {
    /// `hover`: the pointer moved with no button down; `exit`: it left
    /// the picture.
    enum Phase { case down, drag, up, hover, exit }
    var phase: Phase
    var point: CGPoint
    var shift = false
    /// ⌥ held: the pen's corner, the eyedropper's Fill.
    var option = false
    /// ⌘ held: the Color panel's target tool turns the hue.
    var command = false
    var clickCount = 1
    var pixelsPerPoint: Double = 1

    /// An event's click count -- 1 for one that has none: AppKit asserts
    /// (and Valtz aborted) reading it off an entered or exited event, the
    /// `exit` the eyedropper's ring is let go by.
    static func clicks(_ e: NSEvent) -> Int {
        e.type == .mouseEntered || e.type == .mouseExited ? 1 : e.clickCount
    }
}

/// What the stage hands markup from the keyboard -- and the Edit menu's
/// Cut, Copy and Paste, sent to it while it has the focus.
enum MarkupKey {
    case delete, escape, cut, copy, paste
    /// Return: a polygon being drawn made.
    case enter
    /// [ and ]: the brush smaller, larger; ⇧[ and ⇧]: softer, harder --
    /// Photoshop's keys.
    case smaller, larger, softer, harder
}

/// Draws a MarkupOverlay over CompareCanvas's picture; takes no clicks
/// but its in-place text editor's.
final class MarkupOverlayView: NSView {
    var overlay = MarkupOverlay() {
        didSet { if overlay != oldValue { needsDisplay = true } }
    }
    /// Canvas pixels -> this view's points (y down).
    var toView = CGAffineTransform.identity {
        didSet {
            if toView != oldValue {
                needsDisplay = true
                textEditor?.layout()
            }
        }
    }
    /// The in-place text editor it holds while a text is edited here: laid
    /// out again as the stage zooms and moves.
    weak var textEditor: MarkupTextEditor?

    override var isFlipped: Bool { true }

    override func hitTest(_ point: NSPoint) -> NSView? {
        let v = super.hitTest(point)
        return v === self ? nil : v
    }

    override func layout() {
        super.layout()
        textEditor?.layout()
    }

    /// How its draws went (the snapshot hooks' `scribble=` reads them):
    /// how many, their total and longest, in seconds.
    static var drawn = (count: 0, total: 0.0, longest: 0.0)

    override func draw(_ dirtyRect: NSRect) {
        let t0 = CFAbsoluteTimeGetCurrent()
        defer {
            let d = CFAbsoluteTimeGetCurrent() - t0
            Self.drawn.count += 1
            Self.drawn.total += d
            Self.drawn.longest = max(Self.drawn.longest, d)
        }
        guard !overlay.isEmpty,
              let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.saveGState()
        ctx.concatenate(toView)
        for s in overlay.strokes where !s.points.isEmpty {
            drawStroke(s, in: ctx)
        }
        for o in overlay.objects
        where overlay.drawsSelection || !overlay.selected.contains(o.id) {
            if o.id == overlay.editing {
                // Its words are the editor's.
                var bare = o
                bare.text = ""
                if bare.kind != .text { MarkupRender.draw(bare, in: ctx) }
            } else {
                MarkupRender.draw(o, in: ctx)
            }
        }
        ctx.restoreGState()
        // The selected drawing: its outline alone (it is not reshaped).
        if let d = overlay.drawingBounds {
            let outline = NSBezierPath(rect: d.applying(toView)
                .insetBy(dx: -3, dy: -3))
            outline.lineWidth = 1
            outline.setLineDash([4, 3], count: 2, phase: 0)
            NSColor.controlAccentColor.setStroke()
            outline.stroke()
        }
        // A polygon being drawn: its path, its corners, its first ringed
        // (a click there closes it). A pen's: its curve, its points by
        // their kind.
        if !overlay.path.isEmpty {
            let pts = overlay.path.map { $0.applying(toView) }
            ctx.saveGState()
            if overlay.pathCurved {
                ctx.addPath(MarkupRender.path(pts, smooth: overlay.pathSmooth,
                                              closed: false))
            } else {
                ctx.addLines(between: pts)
            }
            ctx.setLineWidth(1)
            ctx.setStrokeColor(NSColor.controlAccentColor.cgColor)
            ctx.strokePath()
            ctx.restoreGState()
            for (i, p) in pts.enumerated() {
                let r = i == 0 ? 6.0 : 3.5
                let smooth = !overlay.pathCurved
                    || (i < overlay.pathSmooth.count && overlay.pathSmooth[i])
                drawPoint(p, radius: r, smooth: smooth)
            }
        }
        // A selected object's gradient: its two ends -- a radial one's
        // centre and edge -- joined, each a round handle in its colour.
        if overlay.selected.count == 1,
           let o = overlay.objects.first(where: {
               overlay.selected.contains($0.id) }),
           let g = o.gradient {
            let ends = MarkupRender.gradientHandles(o).map {
                $0.applying(toView)
            }
            let join = NSBezierPath()
            join.move(to: ends[0])
            join.line(to: ends[1])
            join.lineWidth = 3
            NSColor.black.withAlphaComponent(0.5).setStroke()
            join.stroke()
            join.lineWidth = 1.5
            NSColor.white.setStroke()
            join.stroke()
            let colors = [g.stops.first?.color, g.stops.last?.color]
            for (i, p) in ends.enumerated() {
                let dot = NSBezierPath(ovalIn: NSRect(x: p.x - 6, y: p.y - 6,
                                                      width: 12, height: 12))
                NSColor(cgColor: (colors[i] ?? .clear).cgColor)?.setFill()
                dot.fill()
                dot.lineWidth = 2
                NSColor.white.setStroke()
                dot.stroke()
            }
        }
        // Outlines and handles at screen size.
        for o in overlay.objects where overlay.selected.contains(o.id) {
            // Round it as it is shown: a turned one's own box turned.
            let t = MarkupRender.transform(o)
            let outline: NSBezierPath
            if t.isIdentity {
                let b = MarkupRender.bounds(o).applying(toView)
                outline = NSBezierPath(rect: b.insetBy(dx: -3, dy: -3))
            } else {
                let k = 3 / max(1e-6, hypot(toView.a, toView.b))
                let b = MarkupRender.ownBounds(o).insetBy(dx: -k, dy: -k)
                let corners = [CGPoint(x: b.minX, y: b.minY),
                               CGPoint(x: b.maxX, y: b.minY),
                               CGPoint(x: b.maxX, y: b.maxY),
                               CGPoint(x: b.minX, y: b.maxY)]
                    .map { $0.applying(t).applying(toView) }
                outline = NSBezierPath()
                outline.move(to: corners[0])
                for c in corners.dropFirst() { outline.line(to: c) }
                outline.close()
            }
            outline.lineWidth = 1
            outline.setLineDash([4, 3], count: 2, phase: 0)
            NSColor.controlAccentColor.setStroke()
            outline.stroke()
            // Its words being typed: no handles under the editor.
            if o.id == overlay.editing { continue }
            // A path the pen edits: its points by their kind.
            if overlay.editsPath && o.kind == .polygon {
                for (i, h) in MarkupRender.shownPoints(o).enumerated() {
                    drawPoint(h.applying(toView), radius: 4,
                              smooth: o.isSmooth(i))
                }
                continue
            }
            for h in MarkupRender.handles(o) {
                let p = h.applying(toView)
                let r = NSRect(x: p.x - 4, y: p.y - 4, width: 8, height: 8)
                NSColor.white.setFill()
                NSBezierPath(rect: r).fill()
                NSColor.controlAccentColor.setStroke()
                NSBezierPath(rect: r).stroke()
            }
        }
        drawConnectors()
        drawSampleRing()
    }

    /// The connectors shown: small rings, white edged in the accent; the
    /// one a point would take, larger and filled.
    private func drawConnectors() {
        for c in overlay.connectors {
            let p = c.applying(toView)
            let ring = NSBezierPath(ovalIn: NSRect(x: p.x - 4, y: p.y - 4,
                                                   width: 8, height: 8))
            NSColor.white.withAlphaComponent(0.9).setFill()
            ring.fill()
            ring.lineWidth = 1.5
            NSColor.controlAccentColor.setStroke()
            ring.stroke()
        }
        if let c = overlay.hotConnector {
            let p = c.applying(toView)
            let dot = NSBezierPath(ovalIn: NSRect(x: p.x - 6.5, y: p.y - 6.5,
                                                  width: 13, height: 13))
            NSColor.controlAccentColor.setFill()
            dot.fill()
            dot.lineWidth = 2
            NSColor.white.setStroke()
            dot.stroke()
        }
    }

    /// A path's point at `p` (view points): a smooth one round, a corner
    /// square -- white, edged in the accent.
    private func drawPoint(_ p: CGPoint, radius r: CGFloat, smooth: Bool) {
        let rect = NSRect(x: p.x - r, y: p.y - r, width: 2 * r, height: 2 * r)
        let mark = smooth ? NSBezierPath(ovalIn: rect) : NSBezierPath(rect: rect)
        NSColor.white.setFill()
        mark.fill()
        NSColor.controlAccentColor.setStroke()
        mark.lineWidth = 1
        mark.stroke()
    }

    /// The eyedropper's ring round the pointer: the colour under it in the
    /// upper half, the one it would replace in the lower, edged grey to
    /// show on any picture; a colour with no alpha over a checkerboard.
    private func drawSampleRing() {
        guard let at = overlay.sampleAt, let c = overlay.sampleColor else {
            return
        }
        let p = at.applying(toView)
        let (outer, inner): (CGFloat, CGFloat) = (34, 22)
        func half(_ top: Bool) -> NSBezierPath {
            // y down here: the upper half runs from 180° to 360°.
            let path = NSBezierPath()
            let (a0, a1): (CGFloat, CGFloat) = top ? (180, 360) : (0, 180)
            path.appendArc(withCenter: p, radius: outer, startAngle: a0,
                           endAngle: a1)
            path.appendArc(withCenter: p, radius: inner, startAngle: a1,
                           endAngle: a0, clockwise: true)
            path.close()
            return path
        }
        for (top, color) in [(true, c), (false, overlay.sampleNow ?? c)] {
            let h = half(top)
            if color.a < 1 {
                NSColor(white: 0.85, alpha: 1).setFill()
                h.fill()
            }
            NSColor(cgColor: color.cgColor)?.setFill()
            h.fill()
        }
        let edge = NSBezierPath(ovalIn: NSRect(x: p.x - outer, y: p.y - outer,
                                               width: 2 * outer,
                                               height: 2 * outer))
        edge.append(NSBezierPath(ovalIn: NSRect(x: p.x - inner,
                                                y: p.y - inner,
                                                width: 2 * inner,
                                                height: 2 * inner)))
        edge.lineWidth = 1.5
        NSColor(white: 0.45, alpha: 0.9).setStroke()
        edge.stroke()
    }

    /// The stroke as the core will paint it, near enough to draw by: a
    /// round-capped path its width, its soft edge a blur of that width.
    /// Erasing shows where it goes, faintly.
    private func drawStroke(_ s: MarkupOverlay.Stroke, in ctx: CGContext) {
        let path = CGMutablePath()
        path.move(to: s.points[0])
        for p in s.points.dropFirst() { path.addLine(to: p) }
        if s.points.count == 1 { path.addLine(to: s.points[0]) }
        let color = s.erase
            ? CGColor(gray: 1, alpha: 0.45)
            : s.color.cgColor
        ctx.saveGState()
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)
        let soft = max(0, min(1, s.softness))
        ctx.setLineWidth(2 * s.radius * (1 - soft / 2))
        if soft > 0.01 {
            // A shadow blurs in view points, not canvas pixels.
            let blur = s.radius * soft * abs(toView.a)
            ctx.setShadow(offset: .zero, blur: blur, color: color)
        }
        ctx.setStrokeColor(color)
        ctx.beginTransparencyLayer(auxiliaryInfo: nil)
        ctx.addPath(path)
        ctx.strokePath()
        ctx.endTransparencyLayer()
        ctx.restoreGState()
    }
}

/// The brush's (or eraser's) OUTLINE under the pointer: the size its
/// stroke has on the picture at the view's zoom, and -- a soft brush --
/// its hard core dashed inside. Drawn here in Core Animation as the
/// pointer moves, so it keeps up with it; two-toned, so it shows on any
/// picture; a small crosshair when the ring would be too small to see.
/// Takes no clicks.
final class BrushRingView: NSView {
    private let dark = CAShapeLayer()
    private let light = CAShapeLayer()
    private let coreDark = CAShapeLayer()
    private let core = CAShapeLayer()

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        for l in [dark, light, coreDark, core] {
            l.fillColor = nil
            l.isHidden = true
            layer?.addSublayer(l)
        }
        dark.strokeColor = NSColor.black.withAlphaComponent(0.55).cgColor
        dark.lineWidth = 3
        light.strokeColor = NSColor.white.withAlphaComponent(0.95).cgColor
        light.lineWidth = 1
        coreDark.strokeColor = NSColor.black.withAlphaComponent(0.45).cgColor
        coreDark.lineWidth = 2.5
        coreDark.lineDashPattern = [3, 3]
        core.strokeColor = NSColor.white.withAlphaComponent(0.85).cgColor
        core.lineWidth = 1
        core.lineDashPattern = [3, 3]
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    /// At `center` (this view's points), `radius` points across, its hard
    /// core `inner`; nil hides it.
    func show(at center: CGPoint?, radius: CGFloat, inner: CGFloat) {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        guard let c = center else {
            for l in [dark, light, coreDark, core] { l.isHidden = true }
            return
        }
        let path: CGPath
        if radius < 3 {
            let m = CGMutablePath()
            let a: CGFloat = 6
            m.move(to: CGPoint(x: c.x - a, y: c.y))
            m.addLine(to: CGPoint(x: c.x + a, y: c.y))
            m.move(to: CGPoint(x: c.x, y: c.y - a))
            m.addLine(to: CGPoint(x: c.x, y: c.y + a))
            path = m
        } else {
            path = CGPath(ellipseIn: CGRect(x: c.x - radius, y: c.y - radius,
                                            width: 2 * radius,
                                            height: 2 * radius),
                          transform: nil)
        }
        dark.path = path
        light.path = path
        dark.isHidden = false
        light.isHidden = false
        let soft = radius >= 3 && inner >= 3 && inner < radius - 2
        core.isHidden = !soft
        coreDark.isHidden = !soft
        if soft {
            let p = CGPath(ellipseIn: CGRect(x: c.x - inner, y: c.y - inner,
                                             width: 2 * inner,
                                             height: 2 * inner),
                           transform: nil)
            core.path = p
            coreDark.path = p
        }
    }
}
