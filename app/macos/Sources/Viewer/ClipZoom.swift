import AVFoundation
import AVKit

/// What the title bar's zoom group has asked of the stage so far
/// (ViewerState's counters): a player made later applies only what is
/// asked after it.
struct ZoomRequests: Equatable {
    var fit = 0
    var actual = 0
    var zoomIn = 0
    var zoomOut = 0
    var unfit = 0
}

/// A finished clip ZOOMED and panned as a picture's canvas is (the title
/// bar's zoom group, ⌘+ ⌘− ⌘0 ⌘9). AVKit's player has no zoom and lays
/// its picture out itself, so while the clip is not fitted this shows it
/// instead: a second layer of the same player -- its frames decoded, a
/// stack's drawn, once for both -- in the player's content overlay, over
/// AVKit's own picture and under its controls, on black. Fitted, it shows
/// nothing and takes no click: AVKit's picture, its clicks and its
/// trackpad scroll are as they were.
///
/// The viewport is the canvas's (CompareCanvas): `zoom` DEVICE pixels per
/// clip pixel, `center` the clip's pixel shown at the middle, 64 points of
/// the clip kept in sight on each axis. Zoomed, a drag pans, a trackpad's
/// scroll pans and a double-click fits again; fitted or not, a pinch, a
/// mouse wheel or a scroll with ⌘ zooms around the pointer -- over markup's
/// view on the clip too (ClipMarkupNSView), whose drags are the tool's.
@MainActor
final class ClipZoomView: NSView {
    /// The clip's frame in its pixels (a stack's canvas); nil, nothing to
    /// zoom: a sound, or a clip not read yet.
    var pixels: CGSize? {
        didSet {
            guard pixels != oldValue else { return }
            if fitting || oldValue == nil {
                fitNow()
            } else {
                keepInView()
                relayout()
            }
        }
    }

    var player: AVPlayer? {
        didSet { if player !== oldValue { videoLayer.player = player } }
    }

    var onViewport: ((CompareCanvas.Viewport) -> Void)?
    /// Where the clip's frame lies while zoomed, in points from the
    /// player's top left (the overlays over it follow); nil while fitted.
    var onShown: ((CGRect?) -> Void)?

    /// The view on show, for the snapshot hooks.
    static weak var shown: ClipZoomView?

    private var zoom: CGFloat = 1
    private var center = CGPoint.zero
    private(set) var fitting = true
    private let backdrop = CALayer()
    private let videoLayer = AVPlayerLayer()
    /// A press begun while zoomed is a pan's to its end.
    private var panning = false
    private var lastDrag = CGPoint.zero
    private var lastViewport: CompareCanvas.Viewport?
    private var lastShown: CGRect??
    private var monitor: Any?

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        backdrop.backgroundColor = NSColor.black.cgColor
        videoLayer.videoGravity = .resize
        for l in [backdrop, videoLayer] {
            l.isHidden = true
            layer?.addSublayer(l)
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override var isFlipped: Bool { false }

    /// Zoomed in or out, not fitted: this shows the clip.
    var zoomed: Bool { !fitting && pixels != nil }

    /// The clip's size on screen while zoomed, in points (a stack's
    /// frames are drawn at it); nil while fitted.
    var zoomedSize: CGSize? { zoomed ? frameRect.size : nil }

    override func setFrameSize(_ size: NSSize) {
        super.setFrameSize(size)
        refit()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        refit()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil {
            Self.shown = self
            watchGestures()
        } else if let monitor {
            NSEvent.removeMonitor(monitor)
            self.monitor = nil
        }
    }

    private func refit() {
        if fitting {
            fitNow()
        } else {
            keepInView()
            relayout()
        }
    }

    // MARK: - Commands

    func fitNow() {
        guard let p = pixels, p.width > 0, p.height > 0,
              bounds.width > 0, bounds.height > 0 else {
            fitting = true
            relayout()
            return
        }
        zoom = max(0.01, min(bounds.width / p.width,
                             bounds.height / p.height) * backing)
        center = CGPoint(x: p.width / 2, y: p.height / 2)
        fitting = true
        relayout()
    }

    /// Stop fitting, the zoom kept (the fit button released).
    func stopFitting() {
        guard fitting, pixels != nil else { return }
        fitting = false
        relayout()
    }

    /// One zoom step, around the middle.
    func zoom(by factor: CGFloat) {
        setZoom(zoom * factor, around: CGPoint(x: bounds.midX,
                                               y: bounds.midY))
    }

    /// The clip at its own pixels, 1:1.
    func actualSize() {
        setZoom(1, around: CGPoint(x: bounds.midX, y: bounds.midY))
    }

    /// A pan, as a drag makes it (snapshot runs drive it).
    func panBy(dx: CGFloat, dy: CGFloat) { pan(dx: dx, dy: dy) }

    // MARK: - Geometry

    private var backing: CGFloat { window?.backingScaleFactor ?? 2 }
    private var pointsPerPixel: CGFloat { zoom / backing }

    /// Where the clip's frame lies, in this view's points (y up).
    var frameRect: CGRect {
        guard let p = pixels else { return bounds }
        let ppp = pointsPerPixel
        return CGRect(x: bounds.midX - center.x * ppp,
                      y: bounds.midY - (p.height - center.y) * ppp,
                      width: p.width * ppp, height: p.height * ppp)
    }

    private func setZoom(_ z: CGFloat, around p: CGPoint) {
        guard pixels != nil else { return }
        let old = pointsPerPixel
        let ix = center.x + (p.x - bounds.midX) / old
        let iy = center.y - (p.y - bounds.midY) / old
        zoom = min(64, max(0.02, z))
        let new = pointsPerPixel
        center = CGPoint(x: ix - (p.x - bounds.midX) / new,
                         y: iy + (p.y - bounds.midY) / new)
        fitting = false
        keepInView()
        relayout()
    }

    private func pan(dx: CGFloat, dy: CGFloat) {
        guard pixels != nil else { return }
        let ppp = pointsPerPixel
        center.x -= dx / ppp
        center.y += dy / ppp
        fitting = false
        keepInView()
        relayout()
    }

    /// `center` held where the clip keeps CompareCanvas.keepInSight
    /// points in the view on each axis -- all of it, when it is shorter.
    private func keepInView() {
        guard let p = pixels, p.width > 0, p.height > 0 else { return }
        let ppp = pointsPerPixel
        guard ppp > 0 else { return }
        func held(_ c: CGFloat, _ e1: CGFloat, view v: CGFloat) -> CGFloat {
            let m = min(CompareCanvas.keepInSight, e1 * ppp, v)
            let slack = (v / 2 - m) / ppp
            let lo = -slack, hi = e1 + slack
            return lo <= hi ? min(hi, max(lo, c)) : e1 / 2
        }
        center = CGPoint(x: held(center.x, p.width, view: bounds.width),
                         y: held(center.y, p.height, view: bounds.height))
    }

    private func relayout() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }
        let on = zoomed
        backdrop.isHidden = !on
        videoLayer.isHidden = !on
        backdrop.frame = bounds
        let r = frameRect
        videoLayer.frame = r
        // In the player's points, from its top left: the card's.
        var shown: CGRect?
        if on {
            let host = enclosingPlayer ?? self
            let h = convert(r, to: host)
            shown = CGRect(x: h.minX,
                           y: host.isFlipped ? h.minY
                                             : host.bounds.height - h.maxY,
                           width: h.width, height: h.height)
        }
        if lastShown != .some(shown) {
            lastShown = .some(shown)
            onShown?(shown)
        }
        let v = CompareCanvas.Viewport(zoom: pixels == nil ? nil : zoom,
                                       fitting: fitting)
        if v != lastViewport {
            lastViewport = v
            onViewport?(v)
        }
    }

    private var enclosingPlayer: NSView? {
        var v = superview
        while let u = v, !(u is AVPlayerView) { v = u.superview }
        return v
    }

    // MARK: - Events

    /// Zoomed, the pointer over the clip is this view's; fitted, it goes
    /// past, to AVKit's views (and a sound's artwork) as before.
    override func hitTest(_ point: NSPoint) -> NSView? {
        zoomed ? super.hitTest(point) : nil
    }

    override func mouseDown(with event: NSEvent) {
        panning = zoomed
        guard panning else { return super.mouseDown(with: event) }
        if event.clickCount == 2 {
            fitNow()
            return
        }
        lastDrag = convert(event.locationInWindow, from: nil)
    }

    override func mouseDragged(with event: NSEvent) {
        guard panning else { return super.mouseDragged(with: event) }
        let p = convert(event.locationInWindow, from: nil)
        pan(dx: p.x - lastDrag.x, dy: p.y - lastDrag.y)
        lastDrag = p
    }

    override func mouseUp(with event: NSEvent) {
        guard panning else { return super.mouseUp(with: event) }
        panning = false
    }

    /// Pinches and scrolls over the clip, before AVKit's views have them:
    /// fitted, this view is not hit, and markup's view may lie over it.
    private func watchGestures() {
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(
            matching: [.magnify, .scrollWheel], handler: { [weak self] e in
                let taken = MainActor.assumeIsolated {
                    self?.take(e) ?? false
                }
                return taken ? nil : e
            })
    }

    /// A pinch or a scroll over the clip -- this view, AVKit's overlay
    /// (a sound's artwork) or markup's view, not AVKit's controls or what
    /// lies over the card -- zooms or pans it: true when it was taken.
    private func take(_ e: NSEvent) -> Bool {
        guard pixels != nil, let w = window, e.window === w,
              !isHiddenOrHasHiddenAncestor,
              let hit = w.contentView?.superview?.hitTest(e.locationInWindow),
              hit === self || hit is ClipMarkupNSView
                || superview.map({ hit.isDescendant(of: $0) }) == true
        else { return false }
        let p = convert(e.locationInWindow, from: nil)
        guard bounds.contains(p) else { return false }
        switch e.type {
        case .magnify:
            setZoom(zoom * (1 + e.magnification), around: p)
        case .scrollWheel:
            if e.hasPreciseScrollingDeltas
                && !e.modifierFlags.contains(.command) {
                // A trackpad's: a zoomed clip pans; a fitted one is
                // AVKit's to scroll.
                guard zoomed else { return false }
                pan(dx: e.scrollingDeltaX, dy: -e.scrollingDeltaY)
            } else {
                let dy = e.hasPreciseScrollingDeltas
                    ? e.scrollingDeltaY / 100 : e.scrollingDeltaY / 10
                guard dy != 0 else { return false }
                setZoom(zoom * pow(1.25, dy), around: p)
            }
        default:
            return false
        }
        return true
    }
}
