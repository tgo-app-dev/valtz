import AppKit
import CoreImage
import QuartzCore

/// Where the stage's card may grow as it is zoomed (DESIGN §10): `room`,
/// the most it takes -- the stage's space -- and `fit`, its fitted size,
/// the least.
struct StageGrowth: Equatable {
    var room: CGSize
    var fit: CGSize

    /// The card for what a zoom lays out (`content`, nil fitted): as
    /// large, between its fitted size and the room.
    func card(_ content: CGSize?) -> CGSize {
        guard let c = content else { return fit }
        return CGSize(width: min(room.width, max(fit.width, c.width)),
                      height: min(room.height, max(fit.height, c.height)))
    }
}

/// A pixel-accurate image viewer with A/B comparison, built on Core
/// Animation layers so the images are color-managed by the system and
/// HDR content keeps its headroom (`preferredDynamicRange = .high`).
///
/// Viewport model: the images lie on one CANVAS -- a pixel grid, top-left
/// origin -- each where its CanvasFit puts it. Most images are the
/// canvas; an edit's original is cropped and scaled to line up with the
/// result made from it. `zoom` is DEVICE pixels per canvas pixel, and
/// `center` the canvas point shown at the middle of each view region.
/// Every mode shares one viewport: side-by-side and stacked panes pan and
/// zoom together; the wipes and difference overlay B on A where their
/// fits put them (top-left aligned, as vpipe's compare stage does, when
/// both are the canvas). The zoom REPORTED is per pixel of the image
/// shown (A, or B in mode B), so 1:1 is that image's own pixels.
///
///   drag            pan (or move a wipe divider when grabbed)
///   scroll          pan (trackpad) / zoom (mouse wheel, or with ⌘)
///   pinch           zoom around the pointer
///   double-click    fit ↔ 1:1
///   space           flip between A and B
///   0 / f, 1, + / - fit, 1:1, zoom in/out
///
/// While FITTING (the title bar's fit button pressed in), the image is
/// fitted again whenever the view changes size, the image changes size
/// or the compare mode changes; zooming or panning ends it.
///
/// The picture stays IN SIGHT: a pan, a zoom or the view's new size leaves
/// at least 64 points of it in the view on each axis -- all of it, along
/// an axis it is shorter than that on screen (`keepInView`). Not while
/// a crop is adjusted: there a drag moves the picture on its canvas, and
/// it may go anywhere -- off the frame, as a keyframe's start or end.
///
/// A CROP (the Crop panel; core media/crop.h) makes A's canvas its own:
/// the picture lies on it where the core's placement puts it -- scaled,
/// turned, offset -- over the padding colour, cut to the canvas. While
/// it is being EDITED a drag moves the picture instead of the view, and
/// what lies outside the canvas shows faintly. GUIDES lay their lines
/// over the canvas (the thirds until set by hand), and where the canvas
/// lies is reported, for their marks beside the stage.
///
/// MARKUP (the markup toolbar): while it is active a drag on the picture
/// is the tool's -- reported in canvas pixels, top-left origin, with no
/// pan -- and its overlay (MarkupOverlay: the stroke being drawn, the
/// objects being edited, their handles) is drawn over the picture.
///
/// On the stage the view GROWS with its zoom (`growth`, DESIGN §10):
/// the card is as large as what the zoom lays out, between its fitted
/// size and the stage's room. Along an axis where all of it fits
/// the room it lies centred -- the card hugs it, so a pan or a zoom about
/// the pointer does not move it there -- and past the room it pans as
/// before.
@MainActor
final class CompareCanvas: NSView {
    /// The ½ point a laid-out size may run past the room and still be
    /// taken to fit it (the card's sizes are fractional).
    static let growSlack: CGFloat = 0.5

    /// What the title bar shows: the zoom (1.0 = 1:1; nil with no image)
    /// and whether the canvas is fitting -- and, for the stage's card,
    /// what the zoom lays out: the size in points a view would need to
    /// show all of it (nil with no image).
    struct Viewport: Equatable {
        var zoom: CGFloat?
        var fitting: Bool
        var content: CGSize? = nil
    }
    var onViewportChange: ((Viewport) -> Void)?
    private var lastViewport: Viewport?

    var imageA: CGImage? {
        didSet {
            guard imageA !== oldValue else { return }
            let resized = imageA?.width != oldValue?.width
                || imageA?.height != oldValue?.height
            applyImages()
            if resized && fitting { fitNow() } else { relayout() }
        }
    }

    var imageB: CGImage? {
        didSet {
            guard imageB !== oldValue else { return }
            applyImages()
            relayout()
        }
    }

    /// Each image's pixels as a GPU surface, when they were drawn into one
    /// (GPUPicture): the layers show it as it is -- a CGImage set as
    /// contents is copied into GPU memory first.
    var surfaceA: IOSurface? {
        didSet { if surfaceA !== oldValue { applyImages() } }
    }
    var surfaceB: IOSurface? {
        didSet { if surfaceB !== oldValue { applyImages() } }
    }

    /// What a layer showing A or B is given.
    private var contentsA: Any? { surfaceA ?? imageA }
    private var contentsB: Any? { surfaceB ?? imageB }

    /// Where each image lies on the canvas.
    var fitA = CanvasFit.identity {
        didSet {
            guard fitA != oldValue else { return }
            if fitting { fitNow() } else { relayout() }
        }
    }
    var fitB = CanvasFit.identity {
        didSet { if fitB != oldValue { relayout() } }
    }

    /// The wipe's divider is drawn. Hidden, it still moves: the resize
    /// cursor still shows where it is.
    var showsWipeLine = true {
        didSet { if showsWipeLine != oldValue { relayout() } }
    }

    /// Called when the space bar flips A ↔ B, so the owner's mode follows.
    var onModeFlip: ((CompareMode) -> Void)?

    /// A's crop: its canvas, and where A lies on it (content pixels ->
    /// canvas pixels, y up, as the core says). nil: A is the canvas.
    var cropA: CropPlacement? {
        didSet {
            guard cropA != oldValue else { return }
            let resized = cropA?.canvas != oldValue?.canvas
            applyImages()
            if resized && fitting { fitNow() } else { relayout() }
        }
    }
    /// The guides over what each side shows (View › Show Guides): A's
    /// canvas, B's picture -- where they are, the thirds unless set.
    var showsGuides = false {
        didSet { if showsGuides != oldValue { relayout() } }
    }
    var guideFractions = GuideFractions.thirds {
        didSet { if guideFractions != oldValue { relayout() } }
    }
    /// Where A's canvas lies in the view (points from its top left), each
    /// time it moves -- the guides' marks beside the stage follow it.
    var onCanvasRect: ((CGRect?) -> Void)?
    private var lastCanvasRect: CGRect??

    /// The markup toolbar has the pointer: drags are its tool's, not a
    /// pan (scrolling and pinching still move the view).
    var markupActive = false {
        didSet {
            if markupActive != oldValue {
                // The stage markup works on (the eyedropper samples it).
                if markupActive { Self.shown = self }
                window?.invalidateCursorRects(for: self)
                updateBrushRing()
            }
        }
    }
    /// The brush's (or eraser's) radius in canvas pixels while it is the
    /// tool -- 0 otherwise -- and its softness: its outline follows the
    /// pointer (BrushRingView), the size its stroke has at this zoom.
    var brushRadius: Double = 0 {
        didSet {
            if brushRadius != oldValue {
                if (brushRadius > 0) != (oldValue > 0) {
                    window?.invalidateCursorRects(for: self)
                }
                updateBrushRing()
            }
        }
    }
    var brushSoftness: Double = 0 {
        didSet { if brushSoftness != oldValue { updateBrushRing() } }
    }
    private let ringView = BrushRingView()
    /// Where the pointer is over the view (its points), while it is.
    private var pointerAt: CGPoint?
    private var tracking: NSTrackingArea?
    /// The stage on show, for the snapshot hooks.
    static weak var shown: CompareCanvas?
    /// No arrow: the brush's outline is the pointer.
    private static let noCursor = NSCursor(
        image: NSImage(size: NSSize(width: 1, height: 1)),
        hotSpot: .zero)
    /// The cursor its tool shows.
    var markupCursor: NSCursor = .crosshair {
        didSet {
            if markupCursor != oldValue {
                window?.invalidateCursorRects(for: self)
            }
        }
    }
    var onMarkupPointer: ((MarkupPointer) -> Void)?
    var onMarkupKey: ((MarkupKey) -> Void)?
    var canMarkupKey: ((MarkupKey) -> Bool)?
    var markupOverlay = MarkupOverlay() {
        didSet { overlayView.overlay = markupOverlay }
    }
    /// Where markup's own frame lies on A: the stack's frame on a canvas
    /// of its own size (Canvas Size) -- markup is drawn in that frame's
    /// pixels, so pointers come off it and the overlay goes on it.
    var markupOrigin = CGPoint.zero {
        didSet { if markupOrigin != oldValue { relayout() } }
    }
    private let overlayView = MarkupOverlayView()
    /// Where markup's in-place text editor goes (MarkupTextEditor).
    var markupOverlayHost: MarkupOverlayView { overlayView }

    /// Markup works on A, shown alone.
    private var markupHere: Bool {
        markupActive && imageA != nil && (imageB == nil || mode == .a)
    }

    /// Fraction of the view an image fills when fit (1 = edge to edge,
    /// for a card that already has the image's aspect ratio).
    var fitMargin: CGFloat = 0.98

    /// The stage's card, grown with the zoom; nil, the view's size is
    /// all there is.
    var growth: StageGrowth? {
        didSet {
            guard growth != oldValue else { return }
            if fitting {
                fitNow()
            } else {
                keepInView()
                relayout()
            }
        }
    }

    var canvasBackground: NSColor? = NSColor(white: 0.11, alpha: 1) {
        didSet { layer?.backgroundColor = canvasBackground?.cgColor }
    }

    var mode: CompareMode = .a {
        didSet {
            guard mode != oldValue else { return }
            applyImages()
            // Side by side or stacked halve the view: still in sight.
            if fitting {
                fitNow()
            } else {
                keepInView()
                relayout()
            }
        }
    }

    private var zoom: CGFloat = 1
    private var center = CGPoint.zero
    private var fitting = true
    /// Where a wipe's divider is, 0...1 (VALTZ_SNAPSHOT_SPLIT starts it
    /// elsewhere, for snapshots).
    private var split: CGFloat = ProcessInfo.processInfo
        .environment["VALTZ_SNAPSHOT_SPLIT"].flatMap(Double.init)
        .map { CGFloat(min(1, max(0, $0))) } ?? 0.5
    private var draggingDivider = false
    private var lastDrag = CGPoint.zero

    private let clipA = CALayer()
    private let clipB = CALayer()
    private let layerA = CALayer()
    private let layerB = CALayer()
    private let divider = CALayer()
    private let labelA = CATextLayer()
    private let labelB = CATextLayer()
    // A cropped: the canvas (checker under the padding) and the picture
    // on it in canvas pixels.
    private let cropBack = CALayer()
    private let cropSpace = CALayer()
    private let cropContent = CALayer()
    private lazy var guidesA = GuideLayers(in: clipA)
    private lazy var guidesB = GuideLayers(in: clipB)

    // Placed by relayout, which knows what the mode shows ("|A − B|"
    // in place of A's caption, say).
    var labelTextA = "" {
        didSet { if labelTextA != oldValue { relayout() } }
    }
    var labelTextB = "" {
        didSet { if labelTextB != oldValue { relayout() } }
    }

    override init(frame: NSRect) {
        super.init(frame: frame)
        wantsLayer = true
        layerUsesCoreImageFilters = true
        layer?.backgroundColor = canvasBackground?.cgColor

        let checker = Self.checkerColor()
        for (clip, content) in [(clipA, layerA), (clipB, layerB)] {
            clip.masksToBounds = true
            content.backgroundColor = checker
            content.contentsGravity = .resize
            content.minificationFilter = .trilinear
            content.preferredDynamicRange = .high
            clip.addSublayer(content)
            layer?.addSublayer(clip)
        }
        // Behind A's own layer in clipA: the canvas.
        cropBack.backgroundColor = checker
        cropSpace.masksToBounds = true
        cropSpace.addSublayer(cropContent)
        cropContent.anchorPoint = .zero
        cropContent.contentsGravity = .resize
        cropContent.minificationFilter = .trilinear
        cropContent.preferredDynamicRange = .high
        for l in [cropBack, cropSpace] {
            l.anchorPoint = .zero
            l.isHidden = true
            clipA.addSublayer(l)
        }
        // Over everything in each side: made after its layers.
        _ = guidesA
        _ = guidesB
        addSubview(overlayView)
        addSubview(ringView)
        divider.backgroundColor = NSColor.white.withAlphaComponent(0.9).cgColor
        divider.shadowOpacity = 0.6
        divider.shadowRadius = 2
        divider.shadowOffset = .zero
        layer?.addSublayer(divider)
        for label in [labelA, labelB] {
            label.fontSize = 11
            label.foregroundColor = NSColor.white.cgColor
            label.backgroundColor = NSColor.black.withAlphaComponent(0.55).cgColor
            label.cornerRadius = 4
            label.alignmentMode = .center
            label.truncationMode = .middle
            layer?.addSublayer(label)
        }
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override var acceptsFirstResponder: Bool { true }
    override var isFlipped: Bool { false }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        let s = window?.backingScaleFactor ?? 2
        for l in [labelA, labelB] { l.contentsScale = s }
        if fitting { fitNow() } else { relayout() }
    }

    override func layout() {
        super.layout()
        overlayView.frame = bounds
        ringView.frame = bounds
        if fitting {
            fitNow()
        } else {
            keepInView()
            relayout()
        }
    }

    // MARK: - Commands

    func fitNow() {
        guard imageA != nil else {
            relayout()
            return
        }
        // Fitted to the card's fitted size: grown by the zoom, the view
        // is that size again by the next layout.
        let box = growth.map {
            CGRect(x: 0, y: 0, width: min(bounds.width, $0.fit.width),
                   height: min(bounds.height, $0.fit.height))
        } ?? bounds
        let r = regions(in: box).first ?? box
        let e = extentA
        let fx = r.width / e.width
        let fy = r.height / e.height
        zoom = max(0.01, min(fx, fy) * fitMargin * backing)
        center = CGPoint(x: e.midX, y: e.midY)
        fitting = true
        relayout()
    }

    /// Stop fitting, keeping the zoom where it is (the fit button
    /// released).
    func stopFitting() {
        guard fitting else { return }
        fitting = false
        relayout()
    }

    /// One zoom step (the title bar's ⊕ / ⊖), around the centre.
    func zoom(by factor: CGFloat) {
        setZoom(zoom * factor, around: CGPoint(x: bounds.midX, y: bounds.midY))
    }

    /// The image shown at its own pixels, 1:1.
    func actualSize() {
        guard imageA != nil else { return }
        setZoom(1 / primaryFit.scale,
                around: CGPoint(x: bounds.midX, y: bounds.midY))
    }

    // MARK: - Geometry

    private var backing: CGFloat { window?.backingScaleFactor ?? 2 }
    /// View points per canvas pixel.
    private var pointsPerPixel: CGFloat { zoom / backing }

    /// The view regions the viewport is drawn into (two for side by side
    /// and stacked; A is left / top).
    private func regions() -> [CGRect] { regions(in: bounds) }

    private func regions(in box: CGRect) -> [CGRect] {
        guard imageB != nil else { return [box] }
        switch mode {
        case .sideBySide:
            let (l, r) = box.divided(atDistance: box.width / 2,
                                     from: .minXEdge)
            return [l.insetBy(dx: 1, dy: 0), r.insetBy(dx: 1, dy: 0)]
        case .stacked:
            let (t, b) = box.divided(atDistance: box.height / 2,
                                     from: .maxYEdge)
            return [t.insetBy(dx: 0, dy: 1), b.insetBy(dx: 0, dy: 1)]
        default:
            return [box]
        }
    }

    /// What the zoom lays out, in points: the size of a view that shows
    /// all of A's extent -- twice it, and the regions' gaps, side by side
    /// or stacked.
    private var laidOut: CGSize {
        let ppp = pointsPerPixel
        let e = extentA
        let w = e.width * ppp, h = e.height * ppp
        guard imageB != nil else { return CGSize(width: w, height: h) }
        switch mode {
        case .sideBySide: return CGSize(width: 2 * (w + 2), height: h)
        case .stacked: return CGSize(width: w, height: 2 * (h + 2))
        default: return CGSize(width: w, height: h)
        }
    }

    private var isWipe: Bool {
        (mode == .wipe || mode == .wipeHorizontal) && imageB != nil
    }

    private func region(containing p: CGPoint) -> CGRect {
        regions().first { $0.contains(p) } ?? bounds
    }

    /// What A covers on the canvas: its crop's canvas, when it has one.
    private var extentA: CGRect {
        if let c = cropA, mode != .b || imageB == nil {
            return CGRect(origin: .zero, size: c.canvas)
        }
        return extent(primary, primaryFit)
    }

    /// What an image covers on the canvas (canvas pixels, top-left
    /// origin).
    private func extent(_ img: CGImage?, _ fit: CanvasFit) -> CGRect {
        CGRect(x: fit.origin.x, y: fit.origin.y,
               width: CGFloat(img?.width ?? 0) * fit.scale,
               height: CGFloat(img?.height ?? 0) * fit.scale)
    }

    /// Where an image lands inside `region` (view points); `extent`
    /// overrides the image's own (a crop's canvas).
    private func imageRect(_ img: CGImage?, _ fit: CanvasFit,
                           in region: CGRect,
                           extent given: CGRect? = nil) -> CGRect {
        let ppp = pointsPerPixel
        let e = given ?? extent(img, fit)
        let x0 = region.midX + (e.minX - center.x) * ppp
        let top = region.midY + (center.y - e.minY) * ppp
        return CGRect(x: x0, y: top - e.height * ppp,
                      width: e.width * ppp, height: e.height * ppp)
    }

    /// The image the primary layer shows (B alone in mode B), and its fit.
    private var primary: CGImage? {
        mode == .b ? (imageB ?? imageA) : imageA
    }
    private var primaryFit: CanvasFit {
        mode == .b && imageB != nil ? fitB : fitA
    }

    /// A is drawn cropped: it has a crop, and the mode shows A.
    private var cropShown: Bool {
        cropA != nil && imageA != nil && !(mode == .b && imageB != nil)
    }

    private func applyImages() {
        cropContent.contents = contentsA
        layerA.contents = cropShown ? nil
            : mode == .b && imageB != nil ? contentsB : contentsA
        layerB.contents = mode == .a || mode == .b ? nil : contentsB
        clipB.compositingFilter = mode == .difference && imageB != nil
            ? CIFilter(name: "CIDifferenceBlendMode") : nil
    }

    private func relayout() {
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        defer { CATransaction.commit() }

        // Each image's own pixels set its filter: past 2x, nearest.
        func filter(_ fit: CanvasFit) -> CALayerContentsFilter {
            zoom * fit.scale >= 2 ? .nearest : .linear
        }
        layerA.magnificationFilter = filter(primaryFit)
        layerB.magnificationFilter = filter(fitB)

        let hasB = imageB != nil
        let rs = regions()
        let (a, fa) = (primary, primaryFit)
        let (b, fb) = (imageB, fitB)

        divider.isHidden = true
        labelB.isHidden = true
        // A wipe's side too narrow (or too short) for its label and
        // `labelRoom` around it shows no label; it comes back with room.
        var roomA = true
        var roomB = true
        clipB.isHidden = !hasB || mode == .a || mode == .b
        let top = bounds.maxY - 18

        switch mode {
        case .sideBySide where hasB, .stacked where hasB:
            clipA.frame = rs[0]
            clipB.frame = rs[1]
            layerA.frame = imageRect(a, fa, in: rs[0]).offsetBy(
                dx: -rs[0].minX, dy: -rs[0].minY)
            layerB.frame = imageRect(b, fb, in: rs[1]).offsetBy(
                dx: -rs[1].minX, dy: -rs[1].minY)
            place(labelA, labelTextA, at: CGPoint(x: rs[0].midX,
                                                  y: rs[0].maxY - 18))
            place(labelB, labelTextB, at: CGPoint(x: rs[1].midX,
                                                  y: rs[1].maxY - 18))
            labelB.isHidden = labelTextB.isEmpty
        case .wipe where hasB:
            let x = (bounds.width * split).rounded()
            clipA.frame = bounds
            layerA.frame = imageRect(a, fa, in: bounds)
            clipB.frame = CGRect(x: x, y: 0, width: bounds.width - x,
                                 height: bounds.height)
            layerB.frame = imageRect(b, fb, in: bounds).offsetBy(dx: -x, dy: 0)
            divider.isHidden = !showsWipeLine
            divider.frame = CGRect(x: x - 1, y: 0, width: 2,
                                   height: bounds.height)
            place(labelA, labelTextA, at: CGPoint(x: x / 2, y: top))
            place(labelB, labelTextB,
                  at: CGPoint(x: x + (bounds.width - x) / 2, y: top))
            roomA = x >= labelWidth(labelTextA) + labelRoom
            roomB = bounds.width - x >= labelWidth(labelTextB) + labelRoom
            labelB.isHidden = labelTextB.isEmpty || !roomB
        case .wipeHorizontal where hasB:
            // A above the divider, B below it.
            let y = (bounds.height * (1 - split)).rounded()
            clipA.frame = bounds
            layerA.frame = imageRect(a, fa, in: bounds)
            clipB.frame = CGRect(x: 0, y: 0, width: bounds.width, height: y)
            layerB.frame = imageRect(b, fb, in: bounds)
            divider.isHidden = !showsWipeLine
            divider.frame = CGRect(x: 0, y: y - 1, width: bounds.width,
                                   height: 2)
            place(labelA, labelTextA, at: CGPoint(x: bounds.midX, y: top))
            place(labelB, labelTextB, at: CGPoint(x: bounds.midX, y: 18))
            roomA = bounds.height - y >= labelHeight + labelRoom
            roomB = y >= labelHeight + labelRoom
            labelB.isHidden = labelTextB.isEmpty || !roomB
        case .difference where hasB:
            clipA.frame = bounds
            clipB.frame = bounds
            layerA.frame = imageRect(a, fa, in: bounds)
            layerB.frame = imageRect(b, fb, in: bounds)
            place(labelA, "|A − B|", at: CGPoint(x: bounds.midX, y: top))
        default:
            clipA.frame = bounds
            layerA.frame = imageRect(a, fa, in: bounds)
            let text = mode == .b && hasB ? labelTextB : labelTextA
            place(labelA, text, at: CGPoint(x: bounds.midX, y: top))
        }
        let canvas = layoutCrop(in: mode == .sideBySide || mode == .stacked
                                ? (hasB ? rs[0] : bounds) : bounds)
        // The guides over what each side shows: A's canvas -- its crop's,
        // laid out, else its picture -- and B, where it shows.
        let scale = window?.backingScaleFactor ?? 2
        guidesA.show(over: showsGuides && imageA != nil
                         ? canvas ?? layerA.frame : nil,
                     at: guideFractions,
                     in: clipA.bounds, scale: scale,
                     border: false)
        guidesB.show(over: showsGuides && hasB && !clipB.isHidden
                         ? layerB.frame : nil,
                     at: guideFractions,
                     in: clipB.bounds, scale: scale, border: false)
        let shown = imageA == nil ? nil
            : (canvas ?? layerA.frame).offsetBy(dx: clipA.frame.minX,
                                                dy: clipA.frame.minY)
        let topLeft = shown.map {
            CGRect(x: $0.minX, y: bounds.height - $0.maxY,
                   width: $0.width, height: $0.height)
        }
        if lastCanvasRect != .some(topLeft) {
            lastCanvasRect = .some(topLeft)
            onCanvasRect?(topLeft)
        }
        overlayView.toView = CGAffineTransform(
            translationX: markupOrigin.x, y: markupOrigin.y)
            .concatenating(canvasToView)
        // Zoomed or moved: the outline at its new size.
        updateBrushRing()
        labelA.isHidden = (labelA.string as? String ?? "").isEmpty || !roomA
        // The band where the pointer grabs the divider moves with the
        // split, and comes and goes with the mode.
        window?.invalidateCursorRects(for: self)
        let v = Viewport(zoom: imageA == nil ? nil : zoom * fa.scale,
                         fitting: fitting,
                         content: imageA == nil ? nil : laidOut)
        if v != lastViewport {
            lastViewport = v
            onViewportChange?(v)
        }
    }

    /// A's crop, laid out in `region` (A's, in view points): the canvas
    /// where A's extent is, the picture on it by the crop's placement --
    /// canvas pixels, scaled to points by the layer above -- and, while
    /// editing, the overhang. The canvas's rectangle in clipA, when there
    /// is one.
    @discardableResult
    private func layoutCrop(in region: CGRect) -> CGRect? {
        let shown = cropShown
        layerA.isHidden = shown
        cropBack.isHidden = !shown
        cropSpace.isHidden = !shown
        guard let c = cropA, let img = imageA, shown else { return nil }
        let ppp = pointsPerPixel
        // In clipA's coordinates (side by side: offset into its region).
        let origin = CGPoint(x: clipA.frame.minX, y: clipA.frame.minY)
        let r = imageRect(nil, .identity, in: region, extent: extentA)
            .offsetBy(dx: -origin.x, dy: -origin.y)
        cropBack.frame = r
        let content = CGRect(x: 0, y: 0, width: img.width, height: img.height)
        let filter: CALayerContentsFilter = zoom >= 2 ? .nearest : .linear
        cropSpace.bounds = CGRect(origin: .zero, size: c.canvas)
        cropSpace.position = r.origin
        cropSpace.transform = CATransform3DMakeScale(ppp, ppp, 1)
        cropContent.bounds = content
        cropContent.position = .zero
        cropContent.setAffineTransform(c.transform)
        cropContent.magnificationFilter = filter
        cropSpace.backgroundColor = c.pad
        return r
    }

    /// A's canvas pixels (top-left origin) -> the overlay's points (y
    /// down).
    private var canvasToView: CGAffineTransform {
        let ppp = pointsPerPixel
        let e = extentA
        let r = imageRect(primary, primaryFit, in: regions().first ?? bounds,
                          extent: e)
        return CGAffineTransform(a: ppp, b: 0, c: 0, d: ppp,
                                 tx: r.minX - e.minX * ppp,
                                 ty: bounds.height - r.maxY - e.minY * ppp)
    }

    /// Markup's canvas rect (pixels, y down) on the screen: where a window
    /// beside it goes (the text's edit window).
    func screenRect(canvas r: CGRect) -> CGRect? {
        guard let win = window else { return nil }
        let v = r.applying(CGAffineTransform(translationX: markupOrigin.x,
                                             y: markupOrigin.y)
                               .concatenating(canvasToView))
        let local = CGRect(x: v.minX, y: bounds.height - v.maxY,
                           width: v.width, height: v.height)
        return win.convertToScreen(convert(local, to: nil))
    }

    /// A view point (this view's, y up) on A's canvas, in pixels.
    private func canvasPoint(_ p: CGPoint) -> CGPoint {
        let c = CGPoint(x: p.x, y: bounds.height - p.y)
            .applying(canvasToView.inverted())
        return CGPoint(x: c.x - markupOrigin.x, y: c.y - markupOrigin.y)
    }

    private func markupPointer(_ phase: MarkupPointer.Phase,
                               _ event: NSEvent) {
        // The stage markup works on (the eyedropper samples it).
        Self.shown = self
        let p = convert(event.locationInWindow, from: nil)
        onMarkupPointer?(MarkupPointer(
            phase: phase, point: canvasPoint(p),
            shift: event.modifierFlags.contains(.shift),
            option: event.modifierFlags.contains(.option),
            command: event.modifierFlags.contains(.command),
            clickCount: MarkupPointer.clicks(event),
            pixelsPerPoint: Double(1 / max(0.0001, pointsPerPixel))))
    }

    /// The colour A shows at markup's canvas pixel `c` -- its picture as
    /// laid here: placed on its crop's canvas over the pad, or at its fit
    /// -- sRGB, straight alpha; nil off it. What the eyedropper takes.
    /// `other`: the same picture drawn otherwise -- at any size, A's
    /// pixels scaled to it (the Color panel's target tool reads A without
    /// its mixer so).
    func sampleColor(atCanvas c: CGPoint, from other: CGImage? = nil) -> RGBA? {
        guard let img = imageA else { return nil }
        let read = { (q: CGPoint) -> RGBA? in
            guard let other else { return Self.pixel(img, at: q) }
            return Self.pixel(other, at: CGPoint(
                x: q.x * CGFloat(other.width) / CGFloat(max(1, img.width)),
                y: q.y * CGFloat(other.height) / CGFloat(max(1, img.height))))
        }
        let p = CGPoint(x: c.x + markupOrigin.x, y: c.y + markupOrigin.y)
        if let crop = cropA {
            guard CGRect(origin: .zero, size: crop.canvas).contains(p) else {
                return nil
            }
            // Its content pixels from canvas pixels, both y up.
            let q = CGPoint(x: p.x, y: crop.canvas.height - p.y)
                .applying(crop.transform.inverted())
            guard q.x >= 0, q.y >= 0, q.x < CGFloat(img.width),
                  q.y < CGFloat(img.height) else {
                return RGBA(cgColor: crop.pad)
            }
            return read(CGPoint(x: q.x, y: CGFloat(img.height) - q.y))
        }
        let e = extent(img, fitA)
        guard e.contains(p), fitA.scale > 0 else { return nil }
        return read(CGPoint(x: (p.x - e.minX) / fitA.scale,
                            y: (p.y - e.minY) / fitA.scale))
    }

    /// `img`'s pixel at `q` (its pixels, from the top left) in sRGB,
    /// straight alpha; nil off it.
    static func pixel(_ img: CGImage, at q: CGPoint) -> RGBA? {
        let x = Int(q.x.rounded(.down)), y = Int(q.y.rounded(.down))
        guard x >= 0, y >= 0, x < img.width, y < img.height,
              let one = img.cropping(to: CGRect(x: x, y: y, width: 1,
                                                height: 1)) else {
            return nil
        }
        var px = [UInt8](repeating: 0, count: 4)
        let drawn = px.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = CGContext(
                data: buf.baseAddress, width: 1, height: 1,
                bitsPerComponent: 8, bytesPerRow: 4,
                space: CGColorSpace(name: CGColorSpace.sRGB)!,
                bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            ctx.draw(one, in: CGRect(x: 0, y: 0, width: 1, height: 1))
            return true
        }
        guard drawn else { return nil }
        let a = Double(px[3]) / 255
        guard a > 0 else { return .clear }
        func c(_ v: UInt8) -> Double { min(1, Double(v) / 255 / a) }
        return RGBA(r: c(px[0]), g: c(px[1]), b: c(px[2]), a: a)
    }

    private let labelHeight: CGFloat = 18
    /// Room a wipe's label needs beside it to show.
    private let labelRoom: CGFloat = 20

    private func labelWidth(_ text: String) -> CGFloat {
        min(max(80, CGFloat(text.count) * 6.6 + 16), bounds.width * 0.45)
    }

    private func place(_ label: CATextLayer, _ text: String, at p: CGPoint) {
        label.string = text
        let w = labelWidth(text)
        label.frame = CGRect(x: p.x - w / 2, y: p.y - labelHeight / 2,
                             width: w, height: labelHeight)
    }

    // MARK: - Viewport changes

    private func setZoom(_ z: CGFloat, around p: CGPoint) {
        guard imageA != nil else { return }
        let r = region(containing: p)
        let old = pointsPerPixel
        let ix = center.x + (p.x - r.midX) / old
        let iy = center.y - (p.y - r.midY) / old
        zoom = min(64, max(0.02, z))
        let new = pointsPerPixel
        center = CGPoint(x: ix - (p.x - r.midX) / new,
                         y: iy + (p.y - r.midY) / new)
        fitting = false
        keepInView()
        relayout()
    }

    private func pan(dx: CGFloat, dy: CGFloat) {
        let ppp = pointsPerPixel
        center.x -= dx / ppp
        center.y += dy / ppp
        fitting = false
        keepInView()
        relayout()
    }

    /// A pan, as a drag makes it (snapshot runs drive it).
    func panBy(dx: CGFloat, dy: CGFloat) { pan(dx: dx, dy: dy) }

    /// Where A lies in the view, in points (snapshot runs report it).
    var shownRectA: CGRect {
        imageRect(primary, primaryFit, in: regions().first ?? bounds,
                  extent: extentA)
    }

    /// How much of the picture a pan or zoom leaves in the view, on each
    /// axis, in view points: this much, or all of it when it is shorter.
    static let keepInSight: CGFloat = 64

    /// `center` held where the picture (A's extent: its crop's canvas,
    /// when it has one) keeps `keepInSight` points in the view -- or,
    /// shorter than that on screen, all of it. Along an axis the picture
    /// spans [e0, e1] canvas pixels and the view `v` points: its near
    /// edge may come to `m` points from the view's far side, and no
    /// further, so the centre stays within (v / 2 - m) / ppp of the
    /// picture. On the stage (`growth`), along an axis where it all fits
    /// the room, it is centred: the card grows to it.
    private func keepInView() {
        guard imageA != nil else { return }
        let r = regions().first ?? bounds
        let e = extentA
        let ppp = pointsPerPixel
        guard ppp > 0, e.width > 0, e.height > 0 else { return }
        let size = laidOut
        let fits = growth.map {
            (x: size.width <= $0.room.width + Self.growSlack,
             y: size.height <= $0.room.height + Self.growSlack)
        } ?? (x: false, y: false)
        func held(_ c: CGFloat, _ e0: CGFloat, _ e1: CGFloat,
                  view v: CGFloat, centred: Bool) -> CGFloat {
            if centred { return (e0 + e1) / 2 }
            let m = min(Self.keepInSight, (e1 - e0) * ppp, v)
            let slack = (v / 2 - m) / ppp
            let lo = e0 - slack, hi = e1 + slack
            return lo <= hi ? min(hi, max(lo, c)) : (e0 + e1) / 2
        }
        center = CGPoint(
            x: held(center.x, e.minX, e.maxX, view: r.width,
                    centred: fits.x),
            y: held(center.y, e.minY, e.maxY, view: r.height,
                    centred: fits.y))
    }

    // MARK: - Events

    override func magnify(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        setZoom(zoom * (1 + event.magnification), around: p)
    }

    override func scrollWheel(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        if event.hasPreciseScrollingDeltas
            && !event.modifierFlags.contains(.command) {
            pan(dx: event.scrollingDeltaX, dy: -event.scrollingDeltaY)
        } else {
            let dy = event.hasPreciseScrollingDeltas
                ? event.scrollingDeltaY / 100 : event.scrollingDeltaY / 10
            setZoom(zoom * pow(1.25, dy), around: p)
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(self)
        if markupHere {
            markingUp = true
            markupPointer(.down, event)
            return
        }
        markingUp = false
        let p = convert(event.locationInWindow, from: nil)
        if event.clickCount == 2 {
            if fitting {
                setZoom(1 / primaryFit.scale, around: p)
            } else {
                fitNow()
            }
            return
        }
        lastDrag = p
        draggingDivider = isWipe && (mode == .wipe
            ? abs(p.x - bounds.width * split) < 8
            : abs(p.y - bounds.height * (1 - split)) < 8)
    }

    override func mouseDragged(with event: NSEvent) {
        pointerAt = convert(event.locationInWindow, from: nil)
        updateBrushRing()
        if markingUp {
            markupPointer(.drag, event)
            return
        }
        let p = convert(event.locationInWindow, from: nil)
        if draggingDivider {
            split = mode == .wipe
                ? min(1, max(0, p.x / max(1, bounds.width)))
                : min(1, max(0, 1 - p.y / max(1, bounds.height)))
            relayout()
        } else {
            pan(dx: p.x - lastDrag.x, dy: p.y - lastDrag.y)
        }
        lastDrag = p
    }

    override func mouseUp(with event: NSEvent) {
        draggingDivider = false
        if markingUp {
            markingUp = false
            markupPointer(.up, event)
        }
    }

    /// A press that began with markup active is markup's to its end.
    private var markingUp = false

    // MARK: The brush's outline

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let tracking { removeTrackingArea(tracking) }
        let t = NSTrackingArea(rect: .zero,
                               options: [.mouseMoved, .mouseEnteredAndExited,
                                         .activeInKeyWindow, .inVisibleRect],
                               owner: self, userInfo: nil)
        addTrackingArea(t)
        tracking = t
    }

    override func mouseMoved(with event: NSEvent) {
        super.mouseMoved(with: event)
        pointerAt = convert(event.locationInWindow, from: nil)
        updateBrushRing()
        // Where the pointer is, for a tool that shows it (a polygon's next
        // edge).
        if markupHere { markupPointer(.hover, event) }
    }

    override func mouseEntered(with event: NSEvent) {
        super.mouseEntered(with: event)
        pointerAt = convert(event.locationInWindow, from: nil)
        updateBrushRing()
    }

    override func mouseExited(with event: NSEvent) {
        super.mouseExited(with: event)
        pointerAt = nil
        updateBrushRing()
        if markupHere { markupPointer(.exit, event) }
    }

    /// The outline where the pointer is, its radius the brush's canvas
    /// pixels at this zoom (points per canvas pixel), its core the part
    /// that is not soft; hidden away from the stage or another tool.
    private func updateBrushRing() {
        let ppp = pointsPerPixel
        let on = markupHere && brushRadius > 0
        if on { Self.shown = self }
        guard on, let p = pointerAt, bounds.contains(p) else {
            ringView.show(at: nil, radius: 0, inner: 0)
            return
        }
        let r = CGFloat(brushRadius) * ppp
        ringView.show(at: p, radius: r,
                      inner: r * CGFloat(1 - min(max(brushSoftness, 0), 1)))
    }

    /// The pointer put over canvas pixel `c` (a snapshot hook: a posted
    /// event moves no tracking area); its outline's radius in points.
    @discardableResult
    func hover(atCanvas c: CGPoint) -> CGFloat {
        let v = CGPoint(x: c.x + markupOrigin.x, y: c.y + markupOrigin.y)
            .applying(canvasToView)
        pointerAt = CGPoint(x: v.x, y: bounds.height - v.y)
        updateBrushRing()
        return CGFloat(brushRadius) * pointsPerPixel
    }

    override func resetCursorRects() {
        if markupHere {
            addCursorRect(bounds, cursor: brushRadius > 0 ? Self.noCursor
                                                          : markupCursor)
            return
        }
        guard isWipe else { return }
        if mode == .wipe {
            let x = bounds.width * split
            addCursorRect(CGRect(x: x - 8, y: 0, width: 16,
                                 height: bounds.height),
                          cursor: .resizeLeftRight)
        } else {
            let y = bounds.height * (1 - split)
            addCursorRect(CGRect(x: 0, y: y - 8, width: bounds.width,
                                 height: 16),
                          cursor: .resizeUpDown)
        }
    }

    // The Edit menu's Cut, Copy and Paste, while the stage has the focus:
    // the selected markup objects (a text's words are its edit window's).
    @objc func cut(_ sender: Any?) { onMarkupKey?(.cut) }
    @objc func copy(_ sender: Any?) { onMarkupKey?(.copy) }
    @objc func paste(_ sender: Any?) { onMarkupKey?(.paste) }

    override func keyDown(with event: NSEvent) {
        if markupHere {
            switch event.keyCode {
            case 51, 117:   // delete, forward delete
                onMarkupKey?(.delete)
                return
            case 53:        // escape
                onMarkupKey?(.escape)
                return
            case 36, 76 where canMarkupKey?(.enter) ?? false:  // return
                onMarkupKey?(.enter)
                return
            default:
                break
            }
        }
        switch event.charactersIgnoringModifiers {
        case " " where imageB != nil:
            onModeFlip?(mode == .b ? .a : .b)
        case "0", "f":
            fitNow()
        case "1":
            actualSize()
        case "+", "=":
            zoom(by: 1.5)
        case "-":
            zoom(by: 1 / 1.5)
        default:
            super.keyDown(with: event)
        }
    }

    // MARK: - Helpers

    private static func checkerColor() -> CGColor {
        let img = NSImage(size: NSSize(width: 16, height: 16), flipped: false) { r in
            NSColor(white: 0.30, alpha: 1).setFill()
            r.fill()
            NSColor(white: 0.40, alpha: 1).setFill()
            NSRect(x: 0, y: 0, width: 8, height: 8).fill()
            NSRect(x: 8, y: 8, width: 8, height: 8).fill()
            return true
        }
        return NSColor(patternImage: img).cgColor
    }
}

extension CompareCanvas: NSMenuItemValidation {
    func validateMenuItem(_ item: NSMenuItem) -> Bool {
        let key: MarkupKey? = switch item.action {
        case #selector(cut(_:)): .cut
        case #selector(copy(_:)): .copy
        case #selector(paste(_:)): .paste
        default: nil
        }
        guard let key else { return true }
        return markupHere && (canMarkupKey?(key) ?? false)
    }
}
