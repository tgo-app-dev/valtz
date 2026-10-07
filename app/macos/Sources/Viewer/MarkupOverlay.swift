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
    var stroke: Stroke?
    var objects: [MarkupObject] = []
    var selected: Set<String> = []

    var isEmpty: Bool { stroke == nil && objects.isEmpty }
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
enum MarkupKey { case delete, escape, cut, copy, paste }

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

    override func draw(_ dirtyRect: NSRect) {
        guard !overlay.isEmpty,
              let ctx = NSGraphicsContext.current?.cgContext else { return }
        ctx.saveGState()
        ctx.concatenate(toView)
        if let s = overlay.stroke, !s.points.isEmpty {
            drawStroke(s, in: ctx)
        }
        for o in overlay.objects {
            MarkupRender.draw(o, in: ctx)
        }
        ctx.restoreGState()
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
