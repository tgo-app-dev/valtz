import AppKit
import SwiftUI

/// Markup over a CLIP on the stage (DESIGN §10a Markup): the player shows
/// a timeline's frame fitted in the card -- AVKit's aspect fit -- and
/// this, over it while the markup toolbar works, takes the pointer there
/// in the canvas's pixels (as the picture's canvas hands them), draws
/// what markup draws live, and the brush's outline. The player has no
/// zoom: its frame is fitted, so markup is too.
struct ClipMarkupView: NSViewRepresentable {
    /// The canvas the player draws, in pixels, and where markup's own
    /// frame lies on it (a canvas of its own size: Canvas Size).
    var canvas: CGSize
    var origin: CGPoint
    var overlay: MarkupOverlay
    var cursor: NSCursor
    var brushRadius: Double
    var brushSoftness: Double
    var onPointer: (MarkupPointer) -> Void
    var onKey: (MarkupKey) -> Void
    var canKey: (MarkupKey) -> Bool

    func makeNSView(context: Context) -> ClipMarkupNSView {
        ClipMarkupNSView()
    }

    func updateNSView(_ v: ClipMarkupNSView, context: Context) {
        v.onPointer = onPointer
        v.onKey = onKey
        v.canKey = canKey
        v.canvas = canvas
        v.origin = origin
        v.overlay = overlay
        v.cursor = cursor
        v.brushRadius = brushRadius
        v.brushSoftness = brushSoftness
    }
}

@MainActor
final class ClipMarkupNSView: NSView {
    var canvas = CGSize(width: 1, height: 1) {
        didSet { if canvas != oldValue { relayout() } }
    }
    var origin = CGPoint.zero {
        didSet { if origin != oldValue { relayout() } }
    }
    var overlay = MarkupOverlay() {
        didSet { overlayView.overlay = overlay }
    }
    var cursor: NSCursor = .crosshair {
        didSet {
            if cursor != oldValue { window?.invalidateCursorRects(for: self) }
        }
    }
    var brushRadius: Double = 0 {
        didSet {
            if brushRadius != oldValue {
                window?.invalidateCursorRects(for: self)
                updateRing()
            }
        }
    }
    var brushSoftness: Double = 0 {
        didSet { if brushSoftness != oldValue { updateRing() } }
    }
    var onPointer: ((MarkupPointer) -> Void)?
    var onKey: ((MarkupKey) -> Void)?
    var canKey: ((MarkupKey) -> Bool)?

    private let overlayView = MarkupOverlayView()
    private let ringView = BrushRingView()
    private var pointerAt: CGPoint?
    private var pressed = false
    /// No arrow: the brush's outline is the pointer.
    private static let noCursor = NSCursor(
        image: NSImage(size: NSSize(width: 1, height: 1)), hotSpot: .zero)
    /// The view on show, for the snapshot hooks.
    static weak var shown: ClipMarkupNSView?

    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(overlayView)
        addSubview(ringView)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { Self.shown = self }
    }

    override func layout() {
        super.layout()
        relayout()
    }

    /// The frame as the player fits it (this view's points, y up).
    private var fitted: CGRect {
        let w = max(1, canvas.width), h = max(1, canvas.height)
        let s = min(bounds.width / w, bounds.height / h)
        let size = CGSize(width: w * s, height: h * s)
        return CGRect(x: bounds.midX - size.width / 2,
                      y: bounds.midY - size.height / 2,
                      width: size.width, height: size.height)
    }

    /// Points a canvas pixel.
    private var pointsPerPixel: CGFloat {
        fitted.width / max(1, canvas.width)
    }

    private func relayout() {
        overlayView.frame = bounds
        ringView.frame = bounds
        let f = fitted, ppp = pointsPerPixel
        // Canvas pixels (y down) to the overlay's points (y down).
        overlayView.toView = CGAffineTransform(translationX: origin.x,
                                               y: origin.y)
            .concatenating(CGAffineTransform(
                a: ppp, b: 0, c: 0, d: ppp,
                tx: f.minX, ty: bounds.height - f.maxY))
        updateRing()
    }

    /// A view point (y up) as markup's pixels.
    func canvasPoint(_ p: CGPoint) -> CGPoint {
        let f = fitted, ppp = max(0.0001, pointsPerPixel)
        return CGPoint(x: (p.x - f.minX) / ppp - origin.x,
                       y: (f.maxY - p.y) / ppp - origin.y)
    }

    /// Markup's pixel `c` as a point of the window, from its top left
    /// (the snapshot hooks post events there).
    func windowPoint(canvas c: CGPoint) -> CGPoint? {
        guard let win = window else { return nil }
        let f = fitted, ppp = pointsPerPixel
        let local = CGPoint(x: f.minX + (c.x + origin.x) * ppp,
                            y: f.maxY - (c.y + origin.y) * ppp)
        let w = convert(local, to: nil)
        return CGPoint(x: w.x, y: win.frame.height - w.y)
    }

    private func send(_ phase: MarkupPointer.Phase, _ e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        onPointer?(MarkupPointer(
            phase: phase, point: canvasPoint(p),
            shift: e.modifierFlags.contains(.shift),
            clickCount: e.clickCount,
            pixelsPerPoint: Double(1 / max(0.0001, pointsPerPixel))))
    }

    override func mouseDown(with e: NSEvent) {
        window?.makeFirstResponder(self)
        pressed = true
        send(.down, e)
    }

    override func mouseDragged(with e: NSEvent) {
        pointerAt = convert(e.locationInWindow, from: nil)
        updateRing()
        if pressed { send(.drag, e) }
    }

    override func mouseUp(with e: NSEvent) {
        guard pressed else { return }
        pressed = false
        send(.up, e)
    }

    // MARK: The brush's outline

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .mouseEnteredAndExited,
                      .activeInKeyWindow, .inVisibleRect],
            owner: self))
    }

    override func mouseMoved(with e: NSEvent) {
        pointerAt = convert(e.locationInWindow, from: nil)
        updateRing()
    }

    override func mouseEntered(with e: NSEvent) {
        pointerAt = convert(e.locationInWindow, from: nil)
        updateRing()
    }

    override func mouseExited(with e: NSEvent) {
        pointerAt = nil
        updateRing()
    }

    private func updateRing() {
        guard brushRadius > 0, let p = pointerAt, bounds.contains(p) else {
            ringView.show(at: nil, radius: 0, inner: 0)
            return
        }
        let r = CGFloat(brushRadius) * pointsPerPixel
        ringView.show(at: p, radius: r,
                      inner: r * CGFloat(1 - min(max(brushSoftness, 0), 1)))
    }

    /// The pointer put over markup's pixel `c` (a snapshot hook: a posted
    /// event moves no tracking area); its outline's radius in points.
    @discardableResult
    func hover(atCanvas c: CGPoint) -> CGFloat {
        let f = fitted, ppp = pointsPerPixel
        pointerAt = CGPoint(x: f.minX + (c.x + origin.x) * ppp,
                            y: f.maxY - (c.y + origin.y) * ppp)
        updateRing()
        return CGFloat(brushRadius) * ppp
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: brushRadius > 0 ? Self.noCursor
                                                      : cursor)
    }

    // MARK: Keys and the Edit menu

    // The Edit menu's Cut, Copy and Paste while it has the focus: the
    // selected markup objects.
    @objc func cut(_ sender: Any?) { onKey?(.cut) }
    @objc func copy(_ sender: Any?) { onKey?(.copy) }
    @objc func paste(_ sender: Any?) { onKey?(.paste) }

    override func keyDown(with e: NSEvent) {
        switch e.keyCode {
        case 51, 117:   // delete, forward delete
            onKey?(.delete)
        case 53:        // escape
            onKey?(.escape)
        default:
            super.keyDown(with: e)
        }
    }
}

extension ClipMarkupNSView: NSMenuItemValidation {
    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        let key: MarkupKey? = switch item.action {
        case #selector(cut(_:)): .cut
        case #selector(copy(_:)): .copy
        case #selector(paste(_:)): .paste
        default: nil
        }
        guard let key else { return true }
        return canKey?(key) ?? false
    }
}
