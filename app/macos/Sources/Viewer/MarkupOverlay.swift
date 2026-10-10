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

    var isEmpty: Bool {
        strokes.isEmpty && objects.isEmpty && drawingBounds == nil
    }
}

/// A pointer on the picture, for the markup toolbar: where (canvas
/// pixels, top-left origin), with which keys, and how many canvas pixels
/// a view point is (a hit's reach is measured on screen).
struct MarkupPointer {
    enum Phase { case down, drag, up }
    var phase: Phase
    var point: CGPoint
    var shift = false
    var clickCount = 1
    var pixelsPerPoint: Double = 1
}

/// What the stage hands markup from the keyboard -- and the Edit menu's
/// Cut, Copy and Paste, sent to it while it has the focus.
enum MarkupKey {
    case delete, escape, cut, copy, paste
    /// [ and ]: the brush smaller, larger; ⇧[ and ⇧]: softer, harder --
    /// Photoshop's keys.
    case smaller, larger, softer, harder
}

/// Draws a MarkupOverlay over CompareCanvas's picture; takes no clicks.
final class MarkupOverlayView: NSView {
    var overlay = MarkupOverlay() {
        didSet { if overlay != oldValue { needsDisplay = true } }
    }
    /// Canvas pixels -> this view's points (y down).
    var toView = CGAffineTransform.identity {
        didSet { if toView != oldValue { needsDisplay = true } }
    }

    override var isFlipped: Bool { true }
    override func hitTest(_ point: NSPoint) -> NSView? { nil }

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
            MarkupRender.draw(o, in: ctx)
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
        // Outlines and handles at screen size.
        for o in overlay.objects where overlay.selected.contains(o.id) {
            let b = MarkupRender.bounds(o).applying(toView)
            let outline = NSBezierPath(rect: b.insetBy(dx: -3, dy: -3))
            outline.lineWidth = 1
            outline.setLineDash([4, 3], count: 2, phase: 0)
            NSColor.controlAccentColor.setStroke()
            outline.stroke()
            for h in MarkupRender.handles(o) {
                let p = h.applying(toView)
                let r = NSRect(x: p.x - 4, y: p.y - 4, width: 8, height: 8)
                NSColor.white.setFill()
                NSBezierPath(rect: r).fill()
                NSColor.controlAccentColor.setStroke()
                NSBezierPath(rect: r).stroke()
            }
        }
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
