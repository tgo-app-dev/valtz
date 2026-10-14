import AppKit
import SwiftUI

/// The markup toolbar's tools.
enum MarkupTool: String, CaseIterable, Identifiable, Sendable {
    case select, brush, eraser, line, rect, ellipse, polygon, text
    var id: String { rawValue }

    var symbol: String {
        switch self {
        case .select: "cursorarrow"
        case .brush: "paintbrush.pointed"
        case .eraser: "eraser"
        case .line: "line.diagonal"
        case .rect: "rectangle"
        case .ellipse: "circle"
        case .polygon: "pentagon"
        case .text: "textformat"
        }
    }

    var label: String {
        switch self {
        case .select: String(localized: "Select")
        case .brush: String(localized: "Brush")
        case .eraser: String(localized: "Eraser")
        case .line: String(localized: "Line")
        case .rect: String(localized: "Rectangle")
        case .ellipse: String(localized: "Ellipse")
        case .polygon: String(localized: "Polygon")
        case .text: String(localized: "Text")
        }
    }

    /// Its key, as a painting app's: V select, B brush, E eraser, L line,
    /// R rectangle, C circle (the ellipse).
    var key: Character? {
        switch self {
        case .select: "v"
        case .brush: "b"
        case .eraser: "e"
        case .line: "l"
        case .rect: "r"
        case .ellipse: "c"
        default: nil
        }
    }

    /// Its key's code on a US keyboard: the tool by its place there when
    /// the layout types no Latin letter (Cyrillic, Greek).
    var keyCode: UInt16? {
        switch self {
        case .select: 9
        case .brush: 11
        case .eraser: 14
        case .line: 37
        case .rect: 15
        case .ellipse: 8
        default: nil
        }
    }

    /// It makes a vector object.
    var shape: MarkupObject.Kind? {
        switch self {
        case .line: .line
        case .rect: .rect
        case .ellipse: .ellipse
        case .text: .text
        default: nil
        }
    }

    var cursor: NSCursor {
        switch self {
        case .select: .arrow
        case .text: .iBeam
        default: .crosshair
        }
    }
}

/// What the toolbar's ARRANGE does with the selected objects (the core's
/// media::arrange_objects): their place in their layer's order, their
/// edges or middles lined up, three or more spaced evenly.
enum MarkupArrange: String, CaseIterable, Identifiable, Sendable {
    case toTop = "to-top", up, down, toBottom = "to-bottom"
    case left, center, right, top, middle, bottom
    case distributeH = "distribute-h", distributeV = "distribute-v"
    var id: String { rawValue }

    static let order: [MarkupArrange] = [.toTop, .up, .down, .toBottom]
    static let align: [MarkupArrange] = [.left, .center, .right,
                                         .top, .middle, .bottom]

    var symbol: String {
        switch self {
        case .toTop: "square.3.layers.3d.top.filled"
        case .up: "square.2.layers.3d.top.filled"
        case .down: "square.2.layers.3d.bottom.filled"
        case .toBottom: "square.3.layers.3d.bottom.filled"
        case .left: "align.horizontal.left"
        case .center: "align.horizontal.center"
        case .right: "align.horizontal.right"
        case .top: "align.vertical.top"
        case .middle: "align.vertical.center"
        case .bottom: "align.vertical.bottom"
        case .distributeH: "distribute.horizontal"
        case .distributeV: "distribute.vertical"
        }
    }

    /// The order's keys, as Photoshop's: ⌘] up, ⌘[ down, ⇧⌘] to the
    /// top, ⇧⌘[ to the bottom (the Arrange menu's). The shifted ones as
    /// the characters they type, "}" and "{": a "]" with ⇧ is never
    /// matched by a keyboard's ⇧⌘].
    var shortcut: KeyboardShortcut? {
        switch self {
        case .toTop: KeyboardShortcut("}", modifiers: .command)
        case .up: KeyboardShortcut("]", modifiers: .command)
        case .down: KeyboardShortcut("[", modifiers: .command)
        case .toBottom: KeyboardShortcut("{", modifiers: .command)
        default: nil
        }
    }

    /// The same, as a menu shows it.
    var shortcutText: String? {
        switch self {
        case .toTop: "⇧⌘]"
        case .up: "⌘]"
        case .down: "⌘["
        case .toBottom: "⇧⌘["
        default: nil
        }
    }

    var label: String {
        switch self {
        case .toTop: String(localized: "Move to Top")
        case .up: String(localized: "Move Up")
        case .down: String(localized: "Move Down")
        case .toBottom: String(localized: "Move to Bottom")
        case .left: String(localized: "Align Left Edges")
        case .center: String(localized: "Align Horizontal Centers")
        case .right: String(localized: "Align Right Edges")
        case .top: String(localized: "Align Top Edges")
        case .middle: String(localized: "Align Vertical Centers")
        case .bottom: String(localized: "Align Bottom Edges")
        case .distributeH: String(localized: "Distribute Horizontally")
        case .distributeV: String(localized: "Distribute Vertically")
        }
    }
}

/// The markup toolbar's state: its tool and settings, and the work in
/// hand -- the stroke being drawn, the shape being dragged out, the
/// objects selected (on one layer) and their current values, which the
/// stage draws live (MarkupOverlay) and the core's picture leaves out.
@MainActor @Observable
final class MarkupState {
    var tool: MarkupTool = .brush
    /// A brush's or eraser's radius (canvas pixels) and softness (0 hard
    /// .. 1 all fade).
    var radius: Double = 12
    var softness: Double = 0.3
    /// A line's or outline's width.
    var width: Double = 6
    /// The colour lines, outlines, text and the brush draw with; what
    /// rectangles and ellipses are filled with.
    var edge = RGBA.red
    var fill = RGBA.clear
    var font = MarkupFont()
    /// A text box's alignment ("left", "center", "right", "justified")
    /// and its lines' spacing, a multiple of the font's line.
    var align = "left"
    var lineSpacing = 1.0
    /// The effects new objects are drawn with (a shadow, a glow), and
    /// whether their panel is open.
    var effects: [MarkupEffect] = []
    var effectsOpen = false
    /// The gradient new shapes are filled with (none: their fill), and
    /// whether its editor is open.
    var gradient: MarkupGradient?
    var gradientOpen = false
    /// The Arrange panel (the selection's order, alignment, spacing).
    var arrangeOpen = false
    /// A POLYGON being drawn: its corners so far (canvas pixels), a click
    /// a corner, and where the pointer is -- the next corner's edge shown
    /// to it. Closed on its first corner, a double-click or Return.
    var polygon: [CGPoint] = []
    var polygonHover: CGPoint?

    /// The brush stroke being drawn.
    var stroke: [CGPoint] = []
    var strokeErase = false
    /// Strokes let go: painted by the core OFF the main thread, one after
    /// another in the order drawn (a long or wide one takes a while), and
    /// drawn over the picture until a picture composed after its paint is
    /// shown.
    var pending: [PendingStroke] = []
    /// How many strokes the core has painted (`PendingStroke.painted`).
    @ObservationIgnored var painted = 0
    @ObservationIgnored var paintedNext = 0
    /// The last paint queued: the next waits for it.
    @ObservationIgnored var painting: Task<Void, Never>?
    /// A shape being dragged out.
    var draft: MarkupObject?
    /// The selected objects, all on `selectionLayer`, as they are now.
    var selection: [MarkupObject] = []
    var selectionLayer: String?
    /// The picture they are on: a selection never outlives it.
    @ObservationIgnored var selectionAsset: String?
    /// The selected text's edit window.
    @ObservationIgnored let textPanel = MarkupTextPanel()
    /// Pastes of what was copied last (the pasteboard's change count
    /// then): each one a step further from the original.
    @ObservationIgnored var pastes = 0
    @ObservationIgnored var pasteboardChange = -1

    enum Drag {
        case none, stroke, shape, move, handle(String, Int), polygon
        /// A selected object's gradient's end (0 from, 1 to).
        case gradient(String, Int)
        /// A drawing pressed (where): dragged, it is lifted and moved.
        case lift(CGPoint)
    }
    /// The DRAWING selected on the raster (core media/markup.h: strokes
    /// that touch): a point of it, and its box -- canvas pixels.
    @ObservationIgnored var drawingAt: CGPoint?
    var drawingBox: CGRect?
    @ObservationIgnored var drag = Drag.none
    @ObservationIgnored var last = CGPoint.zero
    @ObservationIgnored var moved = false
    /// A move: where it was pressed, the selection as it was then -- a
    /// move is from there, so a snap to a guide holds no error.
    @ObservationIgnored var moveFrom = CGPoint.zero
    @ObservationIgnored var moveStart: [MarkupObject] = []

    var selectedIds: Set<String> { Set(selection.map(\.id)) }

    /// The selection being CHANGED -- moved, reshaped, restyled, typed:
    /// drawn live over the picture and left out of the core's, which
    /// would lag. Otherwise the core draws it, in its layer's place --
    /// over it, a selected object of a lower layer covered the layers
    /// above -- and the stage outlines it alone. `settling`: just kept,
    /// the live copy stays until the picture made with it is shown.
    var live = false
    var settling = false
    /// The selection layer's DRAWING -- the pixels painted on it --
    /// selected too (the select tool on them): copied, cut, deleted and
    /// pasted as pixels, onto a blank or markup layer.
    var drawing = false
    /// Each drawing read, by its raster's hash: its picture and where its
    /// pixels are (canvas pixels, y down) -- for a click and an outline.
    @ObservationIgnored var rasters: [String: (image: CGImage,
                                                bounds: CGRect)] = [:]

    /// A brush radius a step up or down from `r`, as Photoshop steps its
    /// brush: finer when small (its diameter's 1 / 10 / 25 / 50 / 100
    /// steps, as radii), held to the toolbar's 1...200.
    static func stepRadius(_ r: Double, up: Bool) -> Double {
        let at = up ? r : r - 0.001
        let step: Double = at < 5 ? 1 : at < 50 ? 5 : at < 100 ? 12.5
                         : at < 150 ? 25 : 50
        let next = up ? (r / step).rounded(.down) * step + step
                      : (r / step).rounded(.up) * step - step
        return min(200, max(1, next))
    }
    var selectedText: MarkupObject? {
        selection.count == 1 && selection[0].kind == .text
            ? selection[0] : nil
    }
}

extension AppModel {
    // MARK: Markup

    /// The markup toolbar has the stage's pointer: it is open over a
    /// picture of the project, shown alone, or a clip.
    var markupReady: Bool {
        markupOpen && markupAsset != nil && stage.mode == .a
            && !isGenerating
    }

    /// What markup draws on: the picture on the stage -- or the CLIP, a
    /// timeline drawn where the player is (DESIGN §10a Markup); one as
    /// imported or made through its edited copy, as a picture is.
    var markupAsset: AssetDTO? {
        if let pic = stagePicture { return pic }
        guard clipOnStage, let c = currentClip, c.kind == "video" else {
            return nil
        }
        return c
    }

    /// Markup on a clip: at the player's frame, drawn by the player.
    var markupOnClip: Bool { stagePicture == nil && markupAsset != nil }

    /// A layer markup takes now: on the page shown -- on a timeline, one
    /// showing at the player's frame (a markup or a blank runs from its
    /// start for its length, none: on to the end; core markup_layer).
    func markupShows(_ l: LayerDTO, in a: AssetDTO) -> Bool {
        if a.isPaged { return a.isOnPage(l, stagePage) }
        guard markupOnClip else { return true }
        let t = l.time ?? LayerTimeDTO()
        return t.offset <= videoFrame
            && (t.duration <= 0 || videoFrame < t.offset + t.duration)
    }

    /// What the stage draws over the picture for markup.
    var markupOverlay: MarkupOverlay {
        guard markupOpen else { return MarkupOverlay() }
        let m = markup
        var o = MarkupOverlay()
        o.strokes = m.pending.filter { $0.asset == markupAsset?.id }
            .map(\.stroke)
        if !m.stroke.isEmpty {
            o.strokes.append(MarkupOverlay.Stroke(
                points: m.stroke, radius: m.radius, softness: m.softness,
                color: m.edge, erase: m.strokeErase))
        }
        o.objects = m.selection + (m.draft.map { [$0] } ?? [])
        if let d = polygonDraft { o.objects.append(d) }
        o.path = m.polygon + (m.polygonHover.map { [$0] } ?? [])
        o.selected = m.selectedIds
        // Drawn by the core, in its layer's place, but while it changes.
        o.drawsSelection = m.live || m.settling
        if m.drawing, let layer = m.selectionLayer {
            o.drawingBounds = m.drawingBox ?? drawingBounds(layer)
        }
        // On a clip, the selection's layer away from the player's frame:
        // nothing of it shows there.
        if markupOnClip, let a = markupAsset, let id = m.selectionLayer,
           let l = a.layerStack.first(where: { $0.id == id }),
           !markupShows(l, in: a) {
            o.objects = m.draft.map { [$0] } ?? []
            o.selected = []
        }
        return o
    }

    /// The layer a markup action goes on (core: an empty selected layer,
    /// else the top markup layer, else a new one on top), made if need
    /// be, and selected in the layer editor.
    private func markupTarget() -> String? {
        guard let core, let projectId, let pic = markupAsset else {
            return nil
        }
        let selected = Array(selectedLayers.union([activeLayer]))
        var extra: [String: Any] = ["selected": selected]
        // A still with pages: drawn on the page shown; a timeline, at the
        // player's frame (a new layer from there, a second long).
        if pic.isPaged { extra["page"] = stagePage }
        if markupOnClip {
            extra["frame"] = videoFrame
            pauseForMarkup()
        }
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             "markup-target", extra: extra)
        guard r.ok, let layer = r["layer"] as? String else {
            note("error", r.message)
            return nil
        }
        reloadAssets()
        if activeLayer != layer || (stageStack?.layers ?? []).isEmpty {
            selectLayer(layer)
        }
        return layer
    }

    private func markupObjects(_ layer: String) -> [MarkupObject] {
        markupAsset?.layerStack.first { $0.id == layer }?.markup?.objects
            ?? []
    }

    /// Drawn on a clip: the player held on the frame it is drawn at.
    private func pauseForMarkup() {
        if videoRate != 0 { playVideo(rate: 0) }
    }

    /// A pointer on the picture, as the tool takes it.
    func markupPointer(_ e: MarkupPointer) {
        guard markupReady else { return }
        // A mark that would go on a LOCKED layer (the selected one): said
        // at the press, nothing begun.
        if e.phase == .down, markup.tool != .select,
           let t = markupTargetPreview, t.locked {
            flash(String(localized: "\(t.title) is locked: unlock it to change it."))
            return
        }
        // On a clip: drawn on the frame pressed on, the player held there.
        if e.phase == .down && markupOnClip { pauseForMarkup() }
        let m = markup
        let p = e.point
        switch (e.phase, m.tool) {
        case (.down, .brush), (.down, .eraser):
            commitSelection()
            m.stroke = [p]
            m.strokeErase = m.tool == .eraser
            m.drag = .stroke
        case (.drag, .brush), (.drag, .eraser):
            guard case .stroke = m.drag, let q = m.stroke.last else { return }
            if hypot(p.x - q.x, p.y - q.y) >= 0.5 { m.stroke.append(p) }
        case (.up, .brush), (.up, .eraser):
            guard case .stroke = m.drag else { return }
            m.drag = .none
            paintStroke()
        case (.down, .text):
            // On a text: edit it; else a new one there -- a BOX, a few
            // words wide and two lines tall, wrapping what is typed.
            if let hit = markupHit(p, e) , hit.object.kind == .text {
                select([hit.object], on: hit.layer)
                editMarkupText()
                return
            }
            commitSelection()
            var t = newObject(.text, at: p)
            t.text = String(localized: "Text")
            t.box = true
            t.x1 = p.x + max(160, t.font.size * 8)
            t.y1 = p.y + (2 * MarkupRender.lineHeight(t.font)).rounded(.up)
            addObject(t)
            editMarkupText()
        case (.down, _) where m.tool.shape != nil:
            commitSelection()
            m.draft = newObject(m.tool.shape!, at: p)
            m.drag = .shape
        case (.drag, _) where m.tool.shape != nil:
            guard case .shape = m.drag, var d = m.draft else { return }
            var q = p
            if e.shift { q = constrained(d, q) }
            (d.x1, d.y1) = (q.x, q.y)
            m.draft = d
        case (.up, _) where m.tool.shape != nil:
            guard case .shape = m.drag, let d = m.draft else { return }
            m.drag = .none
            m.draft = nil
            let big = d.kind == .line
                ? hypot(d.x1 - d.x0, d.y1 - d.y0) >= 2
                : abs(d.x1 - d.x0) >= 2 && abs(d.y1 - d.y0) >= 2
            if big { addObject(d) }
        case (.down, .polygon):
            polygonDown(e)
        case (.drag, .polygon):
            // The corner pressed follows the pointer until let go.
            guard case .polygon = m.drag, !m.polygon.isEmpty else { return }
            m.polygon[m.polygon.count - 1] = p
        case (.up, .polygon):
            m.drag = .none
        case (.hover, .polygon):
            if !m.polygon.isEmpty { m.polygonHover = p }
        case (.down, .select):
            selectDown(e)
        case (.drag, .select):
            selectDrag(e)
        case (.up, .select):
            if m.moved { commitSelection(keep: true) } else { settleSelection() }
            m.drag = .none
            m.moved = false
        default:
            break
        }
    }

    /// The selected text's words in their edit window (the toolbar's
    /// button, a new text, a double-click on one).
    func editMarkupText() {
        markup.textPanel.open(self)
    }

    func markupKey(_ k: MarkupKey) {
        switch k {
        case .delete: deleteSelectedObjects()
        case .escape:
            // A polygon being drawn goes; else the selection is let go.
            if !markup.polygon.isEmpty {
                markup.polygon = []
                markup.polygonHover = nil
            } else {
                commitSelection()
            }
        case .enter: finishPolygon()
        case .cut: copySelectedObjects(cut: true)
        case .copy: copySelectedObjects(cut: false)
        case .paste: pasteObjects()
        case .smaller, .larger:
            let m = markup
            m.radius = MarkupState.stepRadius(m.radius, up: k == .larger)
        case .softer, .harder:
            // Photoshop's hardness steps of a quarter, softness its
            // other side.
            let m = markup
            let s = m.softness + (k == .softer ? 0.25 : -0.25)
            m.softness = min(1, max(0, (s * 4).rounded() / 4))
        }
    }

    /// A tool's KEY (MarkupTool.key) -- V, B, E, L, R, C -- while the
    /// markup toolbar can be used, wherever the keyboard is in the window
    /// but text (a prompt, a field: a letter typed there is a letter) or a
    /// list (its letters find a row); no modifier held, held down once.
    /// Handled -- true -- or left to go on.
    func markupToolKey(_ e: NSEvent) -> Bool {
        guard markupOpen, markupReady, !e.isARepeat,
              let w = e.window, !(w is NSPanel),
              editorWindow == nil || w === editorWindow else { return false }
        let fr = w.firstResponder
        if fr is NSText || fr is NSTableView || fr is NSOutlineView
            || fr is NSCollectionView { return false }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
            .subtracting([.capsLock, .numericPad, .function])
        guard mods.isEmpty else { return false }
        // By the letter, else -- a layout of other letters -- by the key.
        let ch = (e.charactersIgnoringModifiers ?? "").lowercased()
        let latin = ch.count == 1 && ch.first.map {
            $0.isASCII && $0.isLetter } == true
        guard let tool = MarkupTool.allCases.first(where: { t in
            latin ? t.key.map { String($0) } == ch
                  : t.keyCode == e.keyCode
        }) else { return false }
        if markup.tool != tool {
            markup.tool = tool
            markupToolChanged()
        }
        return true
    }

    /// [ ] and ⇧[ ⇧] on the brush or the eraser, wherever the keyboard is
    /// but in text (a prompt, a field): handled -- true -- or left to go on.
    func brushKey(_ e: NSEvent) -> Bool {
        guard markupOpen, markupReady,
              markup.tool == .brush || markup.tool == .eraser,
              let w = e.window, !(w is NSPanel),
              editorWindow == nil || w === editorWindow,
              !(w.firstResponder is NSText) else { return false }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
        guard mods.subtracting([.shift, .capsLock]).isEmpty else {
            return false
        }
        // By the character, else by the key (another layout's).
        let ch = e.charactersIgnoringModifiers ?? ""
        let left = ch == "[" || ch == "{" || (ch.isEmpty && e.keyCode == 33)
        let right = ch == "]" || ch == "}" || (ch.isEmpty && e.keyCode == 30)
        guard left || right else { return false }
        let shift = mods.contains(.shift)
        markupKey(shift ? (left ? .softer : .harder)
                        : (left ? .smaller : .larger))
        return true
    }

    // MARK: Copy and paste

    /// Markup objects on the pasteboard: their JSON, the picture they
    /// came from, and whether they were cut. Pixels -- a stroke, objects
    /// made pixels -- are no objects: they stay where they are.
    static let markupPasteboardType =
        NSPasteboard.PasteboardType("com.tgous.valtz.markup")

    /// Whether the Edit menu's command has something to work on.
    func markupCan(_ k: MarkupKey) -> Bool {
        guard markupReady else { return false }
        switch k {
        case .cut, .copy, .delete:
            return !markup.selection.isEmpty || markup.drawing
        case .paste:
            return NSPasteboard.general.availableType(
                from: [Self.markupPasteboardType]) != nil
        case .escape, .smaller, .larger, .softer, .harder: return true
        case .enter: return !markup.polygon.isEmpty
        }
    }

    // MARK: Gradients

    /// The gradient the toolbar shows: the selected shape's or text's,
    /// else the one new shapes get.
    var markupGradient: MarkupGradient? {
        let m = markup
        if let o = m.selection.first(where: {
            $0.fillable || $0.kind == .text }) {
            return o.gradient
        }
        return m.gradient
    }

    /// The colour a gradient starts from: the selection's fill (or its
    /// colour), else the toolbar's.
    var markupGradientBase: RGBA {
        let m = markup
        if let o = m.selection.first(where: {
            $0.fillable || $0.kind == .text }) {
            return o.fillable && o.fill.a > 0 ? o.fill : o.stroke
        }
        return m.fill.a > 0 ? m.fill : m.edge
    }

    /// A new gradient of `kind` (none: nil) from `base`: it to a lighter
    /// one -- a darker one from a light colour.
    static func newGradient(_ kind: MarkupGradient.Kind,
                            from base: RGBA) -> MarkupGradient {
        let light = 0.299 * base.r + 0.587 * base.g + 0.114 * base.b > 0.6
        let t = light ? 0.0 : 1.0
        let other = RGBA(r: base.r + (t - base.r) * 0.7,
                         g: base.g + (t - base.g) * 0.7,
                         b: base.b + (t - base.b) * 0.7, a: base.a)
        return MarkupGradient(kind: kind, stops: [
            .init(at: 0, color: base), .init(at: 1, color: other)])
    }

    /// The gradient `g` (nil: none) on the selected shapes and texts, and
    /// the one new shapes get; kept at once, or -- a stop dragged -- when
    /// let go (`commit` false meanwhile).
    func setMarkupGradient(_ g: MarkupGradient?, commit: Bool = true) {
        markup.gradient = g
        editSelection(commit: commit) {
            if $0.fillable || $0.kind == .text { $0.gradient = g }
        }
    }

    /// The polygon being drawn, as an object with the toolbar's settings
    /// -- to the pointer too -- for the overlay; nil while there is none.
    private var polygonDraft: MarkupObject? {
        let m = markup
        let pts = m.polygon + (m.polygonHover.map { [$0] } ?? [])
        guard pts.count >= 2 else { return nil }
        var o = newObject(.polygon, at: pts[0])
        o.id = "polygon-draft"
        MarkupRender.setPolygon(&o, pts)
        return o
    }

    /// A click with the polygon tool: its first corner, the next one --
    /// or, on its first corner (three or more) or a double-click, the
    /// polygon made.
    private func polygonDown(_ e: MarkupPointer) {
        let m = markup
        let p = e.point
        if m.polygon.isEmpty {
            commitSelection()
            m.polygon = [p]
            m.polygonHover = nil
            m.drag = .polygon
            return
        }
        let first = m.polygon[0]
        let reach = 8 * e.pixelsPerPoint
        if (m.polygon.count >= 3
            && hypot(p.x - first.x, p.y - first.y) <= reach)
            || e.clickCount >= 2 {
            finishPolygon()
            return
        }
        m.polygon.append(p)
        m.drag = .polygon
    }

    /// The polygon drawn made an object -- three corners or more, the
    /// same corner twice running taken once -- and selected.
    func finishPolygon() {
        let m = markup
        var pts: [CGPoint] = []
        for p in m.polygon {
            if let q = pts.last, hypot(p.x - q.x, p.y - q.y) < 1 { continue }
            pts.append(p)
        }
        if let a = pts.first, let z = pts.last, pts.count > 1,
           hypot(a.x - z.x, a.y - z.y) < 1 {
            pts.removeLast()
        }
        m.polygon = []
        m.polygonHover = nil
        m.drag = .none
        guard pts.count >= 3 else { return }
        var o = newObject(.polygon, at: pts[0])
        MarkupRender.setPolygon(&o, pts)
        addObject(o)
    }

    /// The selected objects on the pasteboard -- the texts' words as
    /// plain text too, for the prompt and other apps -- and, cut, gone.
    func copySelectedObjects(cut: Bool) {
        let m = markup
        guard markupReady, !m.selection.isEmpty || m.drawing else { return }
        // The drawings -- the one selected on the raster, the lifted ones
        // -- as one PNG, the whole canvas, where they lie: held by the
        // pasteboard alone (the core's file, in the working copy, read and
        // removed -- never a file elsewhere).
        var png: Data?
        let lifted = m.selection.filter { $0.kind == .drawing }
        if m.drawing || !lifted.isEmpty, let layer = m.selectionLayer,
           let core, let projectId, let pic = markupAsset {
            var extra: [String: Any] = ["objects": lifted.map(\.id)]
            if m.drawing, let at = m.drawingAt {
                extra["x"] = at.x
                extra["y"] = at.y
            }
            let r = core.layerOp(project: projectId,
                                 asset: composedTarget(pic.id),
                                 "copy-drawings", layer: layer, extra: extra)
            if r.ok, let p = r["path"] as? String, !p.isEmpty {
                let url = URL(fileURLWithPath: p)
                png = try? Data(contentsOf: url)
                try? FileManager.default.removeItem(at: url)
            }
        }
        let doc: [String: Any] = [
            "objects": m.selection.filter { $0.kind != .drawing }
                .map(\.json),
            "asset": markupAsset?.id ?? "",
            "layer": m.selectionLayer ?? "",
            "drawing": png != nil,
            "cut": cut,
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: doc)
        else { return }
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setData(data, forType: Self.markupPasteboardType)
        if let png { pb.setData(png, forType: .png) }
        let words = m.selection.filter { $0.kind == .text }.map(\.text)
        if !words.isEmpty {
            pb.setString(words.joined(separator: "\n"), forType: .string)
        }
        m.pastes = 0
        m.pasteboardChange = pb.changeCount
        if cut { deleteSelectedObjects() }
    }

    /// The pasteboard's objects on the layer the next mark goes on,
    /// selected: a copy onto the picture it came from steps off the
    /// original (and off the paste before); a cut comes back where it
    /// was. Objects that would land off the picture are centred on it.
    func pasteObjects() {
        let m = markup
        let pb = NSPasteboard.general
        guard markupReady,
              let data = pb.data(forType: Self.markupPasteboardType),
              let doc = try? JSONSerialization.jsonObject(with: data)
                as? [String: Any] else { return }
        var objects = DTO.decode([MarkupObject].self, doc["objects"]) ?? []
        let drawing = doc["drawing"] as? Bool == true
            ? pb.data(forType: .png) : nil
        guard !objects.isEmpty || drawing != nil else { return }
        if pb.changeCount != m.pasteboardChange {
            m.pastes = 0
            m.pasteboardChange = pb.changeCount
        }
        commitSelection()
        // Onto the layer the next mark goes on: a blank one selected
        // takes a markup; a markup one selected is pasted on.
        guard let layer = markupTarget() else { return }
        // Onto the layer it came from, it steps off the original (and
        // off the paste before); onto another, where it was -- a cut
        // comes back where it was.
        let fromHere = (doc["asset"] as? String) == markupAsset?.id
            && (doc["layer"] as? String) == layer
        let cut = doc["cut"] as? Bool ?? false
        let step = Double(m.pastes + (fromHere && !cut ? 1 : 0)) * 20
        m.pastes += 1
        var dx = step, dy = step
        let union = objects.map(MarkupRender.bounds)
            .reduce(CGRect.null) { $0.union($1) }
        if !union.isNull, let f = markupFrame,
           !union.offsetBy(dx: dx, dy: dy).intersects(
               CGRect(origin: .zero, size: f)) {
            dx = f.width / 2 - union.midX
            dy = f.height / 2 - union.midY
        }
        for i in objects.indices {
            objects[i].id = "m" + UUID().uuidString.prefix(8).lowercased()
            objects[i].x0 += dx
            objects[i].x1 += dx
            objects[i].y0 += dy
            objects[i].y1 += dy
        }
        // A drawing FLOATS where it lands: a lifted one, selected with
        // the objects -- laid into the raster when they are let go.
        var floating: [MarkupObject] = []
        if let drawing, let core, let projectId, let pic = markupAsset {
            let r = core.layerOp(
                project: projectId, asset: composedTarget(pic.id),
                "paste-drawing", layer: layer,
                extra: ["png_base64": drawing.base64EncodedString(),
                        "dx": dx, "dy": dy])
            if !r.ok {
                note("error", r.message)
                return
            }
            reloadAssets()
            let id = r["object"] as? String
            floating = markupObjects(layer).filter { $0.id == id }
            markupChanged()
        }
        if !objects.isEmpty,
           !saveObjects(markupObjects(layer) + objects, on: layer) {
            return
        }
        if m.tool != .select && m.tool != .text { m.tool = .select }
        select(objects + floating, on: layer)
        m.drawing = false
    }

    // MARK: Drawings (a markup's painted pixels)

    /// The file a layer's drawing is in (the core's; nil: none painted).
    private func drawingPath(_ layer: String) -> URL? {
        guard let core, let projectId, let pic = markupAsset else {
            return nil
        }
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             "drawing", layer: layer)
        guard r.ok, let p = r["path"] as? String, !p.isEmpty else {
            return nil
        }
        return URL(fileURLWithPath: p)
    }

    /// A layer's drawing read (once a raster): its picture, and where its
    /// pixels are.
    private func drawingRaster(_ layer: String)
        -> (image: CGImage, bounds: CGRect)?
    {
        guard let l = markupAsset?.layerStack.first(where: { $0.id == layer }),
              let hash = l.markup?.raster, !hash.isEmpty else { return nil }
        let m = markup
        if let r = m.rasters[hash] { return r }
        guard let url = drawingPath(layer),
              let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let img = CGImageSourceCreateImageAtIndex(src, 0, nil)
        else { return nil }
        let r = (image: img, bounds: Self.paintedBounds(img))
        m.rasters[hash] = r
        return r
    }

    /// The drawing's pixels' bounds on the canvas, when it has been read.
    private func drawingBounds(_ layer: String) -> CGRect? {
        guard let l = markupAsset?.layerStack.first(where: { $0.id == layer }),
              let hash = l.markup?.raster,
              let r = markup.rasters[hash], !r.bounds.isNull else {
            return nil
        }
        return r.bounds.offsetBy(dx: rasterOffset(r.image).x,
                                 dy: rasterOffset(r.image).y)
    }

    /// Where a raster lies on the canvas: centred at its own size, as the
    /// core lays it.
    private func rasterOffset(_ img: CGImage) -> CGPoint {
        guard let f = markupFrame else { return .zero }
        return CGPoint(x: (f.width - CGFloat(img.width)) / 2,
                       y: (f.height - CGFloat(img.height)) / 2)
    }

    /// The layer whose drawing is under `p` -- painted there, within a
    /// few points -- the visible markup layers' showing here, from the
    /// top.
    private func drawingHit(_ p: CGPoint, _ e: MarkupPointer) -> String? {
        guard let a = markupAsset else { return nil }
        let reach = min(12, max(1, Int((4 * e.pixelsPerPoint).rounded())))
        // A locked layer's drawings are not picked up.
        for l in a.layerStack.reversed()
        where l.visible && !l.locked && markupShows(l, in: a)
            && !(l.markup?.raster.isEmpty ?? true) {
            guard let r = drawingRaster(l.id) else { continue }
            let o = rasterOffset(r.image)
            if Self.painted(r.image, at: CGPoint(x: p.x - o.x, y: p.y - o.y),
                            reach: reach) {
                return l.id
            }
        }
        return nil
    }

    /// Anything painted within `reach` pixels of `q` (y down)?
    private static func painted(_ img: CGImage, at q: CGPoint,
                                reach: Int) -> Bool {
        let n = 2 * reach + 1
        var px = [UInt8](repeating: 0, count: n * n * 4)
        let x = Int(q.x.rounded(.down)) - reach
        let y = Int(q.y.rounded(.down)) - reach
        let hit = px.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = Self.rgba(buf.baseAddress, n, n) else {
                return false
            }
            // Pixel (x, y) from the top at the context's origin.
            ctx.draw(img, in: CGRect(x: -x,
                                     y: -(img.height - n - y),
                                     width: img.width, height: img.height))
            return stride(from: 3, to: buf.count, by: 4).contains {
                buf[$0] > 8
            }
        }
        return hit
    }

    /// An 8-bit premultiplied RGBA context over `data`, `w` x `h`: its
    /// alphas read every fourth byte, rows from the top.
    private static func rgba(_ data: UnsafeMutableRawPointer?, _ w: Int,
                             _ h: Int) -> CGContext? {
        CGContext(data: data, width: w, height: h, bitsPerComponent: 8,
                  bytesPerRow: w * 4,
                  space: CGColorSpace(name: CGColorSpace.sRGB)!,
                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
    }

    /// Where a drawing's pixels are (y down); null for none.
    private static func paintedBounds(_ img: CGImage) -> CGRect {
        let w = img.width, h = img.height
        var a = [UInt8](repeating: 0, count: w * h * 4)
        let ok = a.withUnsafeMutableBytes { buf -> Bool in
            guard let ctx = Self.rgba(buf.baseAddress, w, h) else {
                return false
            }
            ctx.draw(img, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        guard ok else { return .null }
        var x0 = w, y0 = h, x1 = -1, y1 = -1
        for row in 0..<h {
            let base = row * w * 4 + 3
            for col in 0..<w where a[base + col * 4] > 8 {
                x0 = min(x0, col)
                x1 = max(x1, col)
                y0 = min(y0, row)
                y1 = max(y1, row)
            }
        }
        // The context's rows run from the top in memory.
        guard x1 >= 0 else { return .null }
        return CGRect(x: x0, y: y0, width: x1 - x0 + 1, height: y1 - y0 + 1)
    }

    /// The canvas a clip on the stage is drawn on, in pixels -- a canvas
    /// of its own size, else its frame -- and where markup's frame lies
    /// on it: what the clip's markup view fits, as the player does.
    var clipMarkupCanvas: CGSize {
        guard let c = markupAsset else { return .zero }
        if let cv = c.canvas, cv.resized {
            return CGSize(width: cv.w, height: cv.h)
        }
        if let f = markupFrame { return f }
        if let s = stackPlayback {
            return CGSize(width: s.width, height: s.height)
        }
        return .zero
    }

    var clipMarkupOrigin: CGPoint {
        guard let cv = markupAsset?.canvas, cv.resized else { return .zero }
        return CGPoint(x: cv.x, y: cv.y)
    }

    /// What markup is drawn on: the picture's frame, in its pixels.
    private var markupFrame: CGSize? {
        guard let pic = markupAsset else { return nil }
        if let c = pic.canvas, let w = c.fw, let h = c.fh, c.framed {
            return CGSize(width: w, height: h)
        }
        if let f = pic.ownFrame { return CGSize(width: f.w, height: f.h) }
        return stage.a.map { CGSize(width: $0.width, height: $0.height) }
    }

    /// A new object of `kind` at `p`, drawn with the toolbar's settings.
    private func newObject(_ kind: MarkupObject.Kind,
                           at p: CGPoint) -> MarkupObject {
        let m = markup
        var o = MarkupObject(
            id: "m" + UUID().uuidString.prefix(8).lowercased(),
            kind: kind, x0: p.x, y0: p.y, x1: p.x, y1: p.y)
        o.stroke = m.edge
        o.fill = kind == .rect || kind == .ellipse || kind == .polygon
            ? m.fill : .clear
        o.gradient = o.fillable ? m.gradient : nil
        o.width = m.width
        o.font = m.font
        if kind == .text {
            o.align = m.align == "left" ? nil : m.align
            o.lineSpacing = m.lineSpacing == 1 ? nil : m.lineSpacing
        }
        o.effects = m.effects
        return o
    }

    /// Shift: a line at a multiple of 45°, a square box.
    private func constrained(_ d: MarkupObject, _ p: CGPoint) -> CGPoint {
        let (dx, dy) = (p.x - d.x0, p.y - d.y0)
        if d.kind == .line {
            let a = (atan2(dy, dx) / (.pi / 4)).rounded() * (.pi / 4)
            let l = hypot(dx, dy)
            return CGPoint(x: d.x0 + l * cos(a), y: d.y0 + l * sin(a))
        }
        let s = max(abs(dx), abs(dy))
        return CGPoint(x: d.x0 + (dx < 0 ? -s : s),
                       y: d.y0 + (dy < 0 ? -s : s))
    }

    /// The stroke just drawn, painted into the target layer -- by the core
    /// off the main thread, after the strokes before it: the pointer and
    /// the window stay live while a long stroke is painted. Until the
    /// picture painted with it is shown, the overlay draws it.
    private func paintStroke() {
        let m = markup
        guard let core, let projectId, let pic = markupAsset,
              !m.stroke.isEmpty, let layer = markupTarget() else {
            m.stroke = []
            return
        }
        m.paintedNext += 1
        let entry = PendingStroke(
            id: m.paintedNext, asset: pic.id,
            stroke: MarkupOverlay.Stroke(
                points: m.stroke, radius: m.radius, softness: m.softness,
                color: m.edge, erase: m.strokeErase))
        m.pending.append(entry)
        m.stroke = []
        let asset = composedTarget(pic.id)
        let s = entry.stroke
        let previous = m.painting
        m.painting = Task { @MainActor [weak self] in
            await previous?.value
            let r = await Task.detached(priority: .userInitiated) {
                core.layerOp(project: projectId, asset: asset, "stroke",
                             layer: layer, extra: ["stroke": [
                                 "points": s.points.map {
                                     [Double($0.x), Double($0.y)]
                                 },
                                 "radius": s.radius, "softness": s.softness,
                                 "color": s.color.json, "erase": s.erase,
                             ] as [String: Any]])
            }.value
            guard let self else { return }
            let m = self.markup
            guard r.ok else {
                self.note("error", r.message)
                m.pending.removeAll { $0.id == entry.id }
                return
            }
            m.painted += 1
            if let i = m.pending.firstIndex(where: { $0.id == entry.id }) {
                m.pending[i].painted = m.painted
            }
            self.markupChanged()
        }
    }

    /// A new object on the target layer, selected.
    private func addObject(_ o: MarkupObject) {
        guard let layer = markupTarget() else { return }
        guard saveObjects(markupObjects(layer) + [o], on: layer) else {
            return
        }
        select([o], on: layer)
    }

    /// `objects` kept as `layer`'s.
    @discardableResult
    private func saveObjects(_ objects: [MarkupObject],
                             on layer: String) -> Bool {
        guard let core, let projectId, let pic = markupAsset else {
            return false
        }
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id), "objects",
                             layer: layer,
                             extra: ["objects": objects.map(\.json)])
        if !r.ok { note("error", r.message) }
        reloadAssets()
        return r.ok
    }

    /// The selection: `objects` on `layer`, drawn live and left out of
    /// the picture; the layer selected in the editor.
    private func select(_ objects: [MarkupObject], on layer: String) {
        let m = markup
        if m.selectionLayer != layer { m.drawing = false }
        loadDrawingImages(objects)
        if m.selectionLayer != layer || m.selectedIds != Set(objects.map(\.id)) {
            m.selection = objects
            m.selectionLayer = layer
            m.selectionAsset = markupAsset?.id
            if activeLayer != layer { selectLayer(layer) }
            markupChanged()
        }
    }

    /// The selection is being changed: drawn live, out of the core's
    /// picture, until it is kept (`settleSelection`).
    func liveSelection() {
        let m = markup
        guard !m.live, !m.selection.isEmpty else { return }
        m.live = true
        m.settling = false
        markupChanged()
    }

    /// Its change kept: the core draws it again in its layer's place; the
    /// live copy goes once that picture is shown (markupComposited).
    func settleSelection() {
        let m = markup
        guard m.live else { return }
        m.live = false
        m.settling = true
        markupChanged()
    }

    /// The selection's values kept on its layer; and, unless `keep`, the
    /// selection let go -- its objects drawn into the picture again.
    func commitSelection(keep: Bool = false) {
        defer { settleSelection() }
        let m = markup
        // Another picture on the stage now: the selection was its.
        if m.selectionAsset != nil && m.selectionAsset != markupAsset?.id {
            m.selection = []
            m.selectionLayer = nil
            m.selectionAsset = nil
            m.drawing = false
            return
        }
        guard let layer = m.selectionLayer, !m.selection.isEmpty else {
            if !keep {
                m.selection = []
                m.selectionLayer = nil
                m.drawing = false
            }
            return
        }
        let byId = Dictionary(uniqueKeysWithValues:
                                m.selection.map { ($0.id, $0) })
        let now = markupObjects(layer).map { byId[$0.id] ?? $0 }
        if now != markupObjects(layer) { saveObjects(now, on: layer) }
        if !keep {
            // A drawing lifted, let go: laid back into the raster where
            // it now lies -- over another, the two are one (a command of
            // its own, "Place Drawing").
            let lifted = m.selection.contains { $0.kind == .drawing }
            m.selection = []
            m.selectionLayer = nil
            m.drawing = false
            if lifted { placeDrawings(on: layer) }
            markupChanged()
        }
    }

    /// The lifted drawings on `layer` laid back into its raster.
    private func placeDrawings(on layer: String) {
        guard let core, let projectId, let pic = markupAsset else { return }
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(pic.id),
                             "place-drawings", layer: layer)
        if !r.ok { note("error", r.message) }
        reloadAssets()
    }

    /// The pictures of lifted drawings among `objects`, read once each
    /// (by their blob): what the overlay draws them from while they are
    /// selected.
    func loadDrawingImages(_ objects: [MarkupObject]) {
        guard let core, let projectId, let pic = markupAsset else { return }
        for o in objects where o.kind == .drawing {
            guard let hash = o.image?.hash,
                  MarkupRender.drawingImages[hash] == nil else { continue }
            let r = core.layerOp(project: projectId,
                                 asset: composedTarget(pic.id),
                                 "drawing-file", layer: "",
                                 extra: ["hash": hash])
            guard r.ok, let p = r["path"] as? String,
                  let src = CGImageSourceCreateWithURL(
                      URL(fileURLWithPath: p) as CFURL, nil),
                  let img = CGImageSourceCreateImageAtIndex(src, 0, nil)
            else { continue }
            MarkupRender.drawingImages[hash] = img
        }
    }

    /// The selection as the project now has it -- after an undo, a redo, a
    /// revert: its objects' values read again, those gone let go; and a
    /// lifted drawing back again (its placing undone) selected, so that
    /// letting it go places it again.
    func syncMarkupSelection() {
        let m = markup
        guard m.selectionAsset == nil || m.selectionAsset == markupAsset?.id
        else { return }
        m.live = false
        if let layer = m.selectionLayer {
            let now = markupObjects(layer)
            let ids = m.selectedIds
            m.selection = now.filter { ids.contains($0.id) }
            if m.selection.isEmpty && !m.drawing { m.selectionLayer = nil }
        }
        if m.selection.isEmpty, !m.drawing, let a = markupAsset,
           let l = a.layerStack.first(where: { l in
               l.visible && (l.markup?.objects ?? []).contains {
                   $0.kind == .drawing
               }
           }) {
            m.selection = markupObjects(l.id).filter { $0.kind == .drawing }
            m.selectionLayer = l.id
            m.selectionAsset = a.id
        }
        loadDrawingImages(m.selection)
    }

    /// Something selected to let go: objects, or a layer's drawing.
    var markupCanDeselect: Bool {
        markupReady && (!markup.selection.isEmpty || markup.drawing)
    }

    /// The selection let go, its values kept -- ⌘D (Edit › Deselect), the
    /// toolbar's button beside Select: Escape's, a polygon being drawn
    /// left as it is.
    func deselectMarkup() {
        guard markupCanDeselect else { return }
        commitSelection()
    }

    /// ⌘D in the editor's window while something is selected: here, in
    /// the window's key monitor, as well as Edit › Deselect's -- a menu
    /// item SwiftUI enables a beat late (in a window not in front,
    /// never), and a disabled one swallows its key. By the letter, else
    /// -- a layout of other letters -- by the key. Handled -- true -- or
    /// left to go on.
    func deselectKey(_ e: NSEvent) -> Bool {
        guard markupCanDeselect, !e.isARepeat,
              let w = e.window, !(w is NSPanel),
              editorWindow == nil || w === editorWindow else { return false }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
            .subtracting([.capsLock, .numericPad, .function])
        guard mods == .command else { return false }
        let ch = (e.charactersIgnoringModifiers ?? "").lowercased()
        let latin = ch.count == 1 && ch.first.map {
            $0.isASCII && $0.isLetter } == true
        guard latin ? ch == "d" : e.keyCode == 2 else { return false }
        deselectMarkup()
        return true
    }

    /// The selection's layer hidden -- its eye, its folder's, an undo, the
    /// agent -- and the selection let go: what is not seen is not worked
    /// on (its handles, Delete, the toolbar's settings would act on it
    /// unseen). Locked, likewise: nothing of it is changed.
    func deselectHiddenLayer() {
        let m = markup
        guard let id = m.selectionLayer,
              m.selectionAsset == markupAsset?.id,
              let l = markupAsset?.layerStack.first(where: { $0.id == id }),
              !l.visible || l.locked else { return }
        commitSelection()
    }

    /// The selected objects changed (a toolbar setting, the text): drawn
    /// at once, and kept -- or, while a slider moves or a word is typed,
    /// kept when that ends.
    func editSelection(commit: Bool = true,
                       _ change: (inout MarkupObject) -> Void) {
        let m = markup
        guard !m.selection.isEmpty else { return }
        if !commit { liveSelection() }
        for i in m.selection.indices { change(&m.selection[i]) }
        if commit { commitSelection(keep: true) }
    }

    /// Where markup's pixels start on the stage's (its canvas's origin):
    /// Information's and the guides' places are the stage's.
    var markupStageOrigin: CGPoint {
        markupOnClip ? clipMarkupOrigin
            : stagePicture?.canvas.map { CGPoint(x: $0.x, y: $0.y) } ?? .zero
    }

    /// The selected objects' frame on the stage (its pixels), as
    /// Information shows it; nil with none selected.
    var selectionFrame: CGRect? {
        MarkupRender.frame(markup.selection).map {
            $0.offsetBy(dx: markupStageOrigin.x, dy: markupStageOrigin.y)
        }
    }

    /// The selected objects laid in `frame` (the stage's pixels) --
    /// Information's position and size typed: moved, or scaled from their
    /// frame's top left, together. The Crop panel places the whole
    /// layer; this its objects alone. One command.
    func setSelectionFrame(_ frame: CGRect) {
        guard let f = MarkupRender.frame(markup.selection) else { return }
        let o = markupStageOrigin
        let to = frame.standardized.offsetBy(dx: -o.x, dy: -o.y)
        guard to != f else { return }
        // As a drag changes it: drawn live, then kept -- the core draws
        // it again where it now is.
        liveSelection()
        editSelection { $0 = MarkupRender.reframed($0, from: f, to: to) }
    }

    /// The layer the next mark goes on, as the core will choose it (an
    /// empty or markup layer among the selected, else a markup layer on
    /// top); nil: a new one.
    var markupTargetPreview: LayerDTO? {
        guard let pic = markupAsset, !(pic.layers ?? []).isEmpty else {
            return nil
        }
        // On a still with pages, the page shown's layers alone; on a
        // timeline, those showing at the player's frame.
        let stack = pic.layerStack.filter { markupShows($0, in: pic) }
        let sel = selectedLayers.union([activeLayer])
        if let top = stack.last(where: { sel.contains($0.id) }),
           top.isEmpty || top.isMarkup {
            return top
        }
        // A locked one is passed over for a new markup (the core's
        // choice too).
        if let last = stack.last, last.isMarkup, !last.locked { return last }
        return nil
    }

    func deleteSelectedObjects() {
        let m = markup
        // The drawing selected: its pixels cleared -- that drawing alone
        // (its objects stay but those selected).
        if m.drawing, let layer = m.selectionLayer, let core, let projectId,
           let pic = markupAsset {
            m.drawing = false
            var extra: [String: Any] = [:]
            if let at = m.drawingAt {
                extra = ["x": at.x, "y": at.y]
            }
            m.drawingAt = nil
            m.drawingBox = nil
            let r = core.layerOp(project: projectId,
                                 asset: composedTarget(pic.id),
                                 "clear-drawing", layer: layer,
                                 extra: extra)
            if !r.ok { note("error", r.message) }
            reloadAssets()
            markupChanged()
        }
        guard let layer = m.selectionLayer, !m.selection.isEmpty else {
            m.selectionLayer = m.selection.isEmpty ? nil : m.selectionLayer
            return
        }
        let gone = m.selectedIds
        m.selection = []
        m.selectionLayer = nil
        saveObjects(markupObjects(layer).filter { !gone.contains($0.id) },
                    on: layer)
        markupChanged()
    }

    /// The selected objects made pixels, on their layer.
    func materializeSelection() {
        let m = markup
        guard let core, let projectId, let pic = markupAsset,
              let layer = m.selectionLayer, !m.selection.isEmpty else {
            return
        }
        commitSelection(keep: true)
        let ids = m.selection.map(\.id)
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             "materialize", layer: layer,
                             extra: ["objects": ids])
        if !r.ok { note("error", r.message) }
        m.selection = []
        m.selectionLayer = nil
        reloadAssets()
        markupChanged()
    }

    /// Whether Arrange's `how` would change anything: up (down) the order
    /// unless the selection is its layer's top (bottom) already; lined
    /// up -- one object on the picture -- always; spaced, three or more.
    func markupCanArrange(_ how: MarkupArrange) -> Bool {
        let m = markup
        guard markupReady, let layer = m.selectionLayer,
              !m.selection.isEmpty else { return false }
        let order = markupObjects(layer).map(\.id)
        let sel = m.selectedIds
        switch how {
        case .toTop, .up: return Set(order.suffix(sel.count)) != sel
        case .down, .toBottom: return Set(order.prefix(sel.count)) != sel
        case .distributeH, .distributeV: return sel.count >= 3
        default: return true
        }
    }

    /// The selected objects ARRANGED by the core -- one command ("Undo
    /// Arrange", "Align", "Distribute") -- and still selected where they
    /// are now.
    func arrangeSelection(_ how: MarkupArrange) {
        let m = markup
        guard let core, let projectId, let pic = markupAsset,
              let layer = m.selectionLayer, !m.selection.isEmpty,
              markupCanArrange(how) else { return }
        commitSelection(keep: true)
        let ids = m.selectedIds
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(pic.id), "arrange",
                             layer: layer,
                             extra: ["objects": Array(ids),
                                     "how": how.rawValue])
        if !r.ok { note("error", r.message) }
        reloadAssets()
        // In the layer's order, as the stage draws them.
        m.selection = markupObjects(layer).filter { ids.contains($0.id) }
        markupChanged()
    }

    /// The object under `p`: the selection's first (its current values),
    /// then the visible markup layers' from the top.
    private func markupHit(_ p: CGPoint, _ e: MarkupPointer)
        -> (object: MarkupObject, layer: String)?
    {
        let m = markup
        let tol = 4 * e.pixelsPerPoint
        if let layer = m.selectionLayer,
           let o = m.selection.last(where: {
               MarkupRender.hits($0, p, tolerance: tol)
           }) {
            return (o, layer)
        }
        guard let a = markupAsset else { return nil }
        // A locked layer's objects are not picked up.
        for l in a.layerStack.reversed()
        where l.visible && !l.locked && markupShows(l, in: a) {
            for o in (l.markup?.objects ?? []).reversed()
            where MarkupRender.hits(o, p, tolerance: tol) {
                let live = m.selection.first { $0.id == o.id } ?? o
                return (live, l.id)
            }
        }
        return nil
    }

    private func selectDown(_ e: MarkupPointer) {
        let m = markup
        let p = e.point
        m.last = p
        m.moved = false
        // A handle of the selection -- its shape's, or the one object's
        // gradient's ends -- the nearest pressed: reshape it.
        let reach = 6 * e.pixelsPerPoint
        var best: (d: Double, drag: MarkupState.Drag)?
        func consider(_ h: CGPoint, _ drag: MarkupState.Drag) {
            let d = hypot(h.x - p.x, h.y - p.y)
            if d <= reach && d < (best?.d ?? .infinity) { best = (d, drag) }
        }
        if m.selection.count == 1, let o = m.selection.first {
            for (i, h) in MarkupRender.gradientHandles(o).enumerated() {
                consider(h, .gradient(o.id, i))
            }
        }
        for o in m.selection {
            for (i, h) in MarkupRender.handles(o).enumerated() {
                consider(h, .handle(o.id, i))
            }
        }
        if let best {
            m.drag = best.drag
            return
        }
        guard let hit = markupHit(p, e) else {
            // Painted pixels there: the layer's DRAWING selected (⇧: with
            // the objects selected on it).
            if let layer = drawingHit(p, e) {
                // The DRAWING there -- the strokes that touch -- selected,
                // outlined by its box; dragged, it is lifted and moved.
                if !(e.shift && m.selectionLayer == layer) {
                    commitSelection()
                    m.selection = []
                    m.selectionLayer = layer
                    m.selectionAsset = markupAsset?.id
                    if activeLayer != layer { selectLayer(layer) }
                }
                let reach = min(12, max(1, Int((4 * e.pixelsPerPoint)
                    .rounded())))
                m.drawingAt = p
                m.drawingBox = drawingBox(at: p, on: layer, reach: reach)
                m.drawing = true
                m.drag = .lift(p)
                return
            }
            commitSelection()
            m.drag = .none
            return
        }
        if e.clickCount >= 2 && hit.object.kind == .text {
            select([hit.object], on: hit.layer)
            editMarkupText()
            m.drag = .none
            return
        }
        if !m.selectedIds.contains(hit.object.id) {
            if e.shift && m.selectionLayer == hit.layer {
                select(m.selection + [hit.object], on: hit.layer)
            } else {
                commitSelection()
                select([hit.object], on: hit.layer)
            }
        }
        m.drag = .move
        m.moveFrom = p
        m.moveStart = m.selection
    }

    private func selectDrag(_ e: MarkupPointer) {
        let m = markup
        let p = e.point
        // Moved or reshaped: drawn live from its first step.
        switch m.drag {
        case .move, .handle, .gradient: liveSelection()
        default: break
        }
        // A drawing pressed, dragged: lifted -- an object where it lay,
        // selected with what was -- then moved as objects are.
        if case .lift(let from) = m.drag {
            guard hypot(p.x - from.x, p.y - from.y) >= 1,
                  let layer = m.selectionLayer,
                  let lifted = liftDrawing(at: from, on: layer, e) else {
                return
            }
            m.drawing = false
            m.drawingAt = nil
            m.drawingBox = nil
            select(m.selection + [lifted], on: layer)
            m.drag = .move
            m.moveFrom = from
            m.moveStart = m.selection
            liveSelection()
        }
        switch m.drag {
        case .move:
            var dx = p.x - m.moveFrom.x, dy = p.y - m.moveFrom.y
            // The magnet: the selection's left, middle or right -- top,
            // middle, bottom -- onto the nearest guide within 6 points.
            if let lines = markupSnapLines,
               let b = MarkupRender.frame(m.moveStart) {
                let reach = 6 * e.pixelsPerPoint
                dx += Self.snap([b.minX, b.midX, b.maxX].map { $0 + dx },
                                to: lines.xs, within: reach)
                dy += Self.snap([b.minY, b.midY, b.maxY].map { $0 + dy },
                                to: lines.ys, within: reach)
            }
            if m.selection.count == m.moveStart.count {
                m.selection = m.moveStart.map {
                    MarkupRender.offset($0, dx: dx, dy: dy)
                }
            }
            m.moved = true
        case .handle(let id, let i):
            if let k = m.selection.firstIndex(where: { $0.id == id }) {
                m.selection[k] = MarkupRender.moving(m.selection[k],
                                                     handle: i, to: p)
                m.moved = true
            }
        case .gradient(let id, let i):
            if let k = m.selection.firstIndex(where: { $0.id == id }) {
                m.selection[k] = MarkupRender.movingGradient(
                    m.selection[k], handle: i, to: p)
                m.moved = true
            }
        default:
            break
        }
        m.last = p
    }

    /// How far `edges` go to put the nearest of them on one of `lines`
    /// within `reach` -- 0 with none there.
    static func snap(_ edges: [Double], to lines: [Double],
                     within reach: Double) -> Double {
        var best: Double?
        for e in edges {
            for l in lines where abs(l - e) <= reach {
                if abs(l - e) < abs(best ?? .infinity) { best = l - e }
            }
        }
        return best ?? 0
    }

    /// The drawing painted near `p` on `layer`: its box (the core's
    /// media::drawing_at).
    private func drawingBox(at p: CGPoint, on layer: String,
                            reach: Int) -> CGRect? {
        guard let core, let projectId, let pic = markupAsset else {
            return nil
        }
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(pic.id), "drawing-at",
                             layer: layer,
                             extra: ["x": p.x, "y": p.y, "reach": reach])
        guard r.ok, let w = (r["w"] as? NSNumber)?.doubleValue, w > 0,
              let h = (r["h"] as? NSNumber)?.doubleValue,
              let x = (r["x"] as? NSNumber)?.doubleValue,
              let y = (r["y"] as? NSNumber)?.doubleValue else { return nil }
        return CGRect(x: x, y: y, width: w, height: h)
    }

    /// The drawing at `p` LIFTED off `layer`'s raster: the object it now
    /// is (one command with the move that follows at once).
    private func liftDrawing(at p: CGPoint, on layer: String,
                             _ e: MarkupPointer) -> MarkupObject? {
        guard let core, let projectId, let pic = markupAsset else {
            return nil
        }
        let reach = min(12, max(1, Int((4 * e.pixelsPerPoint).rounded())))
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(pic.id), "lift-drawing",
                             layer: layer,
                             extra: ["x": p.x, "y": p.y, "reach": reach])
        guard r.ok, let id = r["object"] as? String else {
            if !r.ok { note("error", r.message) }
            return nil
        }
        reloadAssets()
        return markupObjects(layer).first { $0.id == id }
    }

    /// The picture made again: the markup's layers changed, or which
    /// objects it draws itself.
    private func markupChanged() {
        if markupOnClip {
            refreshStackPlan()
        } else if stageComposed {
            recomposite()
        }
    }

    /// A picture composed once `painted` strokes were painted is shown:
    /// those strokes drawn over it can go (one painted later stays).
    func markupComposited(painted: Int) {
        let m = markup
        if m.settling { m.settling = false }
        m.pending.removeAll { ($0.painted ?? .max) <= painted }
    }

    /// The toolbar closed, or its tool changed: the selection let go.
    func markupToolChanged() {
        let m = markup
        // Another tool: a polygon being drawn is made (or let go).
        if m.tool != .polygon && !m.polygon.isEmpty { finishPolygon() }
        if m.tool != .select && m.tool != .text { commitSelection() }
        // The brush or the eraser takes the keyboard from the prompt (as
        // a painting app's tools do): [ ] are its size then, not words.
        if m.tool == .brush || m.tool == .eraser,
           let w = editorWindow, w.firstResponder is NSText {
            w.makeFirstResponder(nil)
        }
    }

    // MARK: Layers: merge and masks

    /// The two selected layers, lower first, when two are selected.
    var mergePair: (String, String)? {
        guard let stack = stagePicture?.layerStack,
              selectedLayers.count == 2 else { return nil }
        let ids = stack.map(\.id).filter { selectedLayers.contains($0) }
        return ids.count == 2 ? (ids[0], ids[1]) : nil
    }

    /// Why the selected two cannot be merged (a mask ties one of them to
    /// a layer outside the two), or nil when they can -- as the core
    /// decides.
    var mergeProblem: String? {
        if clipOnStage {
            return String(localized: "A clip's layers cannot be merged")
        }
        if pagedOnStage, let (lo, hi) = mergePair,
           let pic = stagePicture,
           let a = pic.layerStack.first(where: { $0.id == lo }),
           let b = pic.layerStack.first(where: { $0.id == hi }),
           pic.pageSpan(of: a) != pic.pageSpan(of: b)
            || layerLook(lo).keyed || layerLook(hi).keyed {
            return String(localized: "Layers on different pages, or changing from page to page, cannot be merged")
        }
        guard let (lo, hi) = mergePair,
              let stack = stageStack?.layerStack,
              let i = stack.firstIndex(where: { $0.id == lo }),
              let j = stack.firstIndex(where: { $0.id == hi }) else {
            return String(localized: "Select two layers to merge them")
        }
        func masksBelow(_ k: Int) -> Bool {
            k + 1 < stack.count && stack[k + 1].mask
        }
        let joined = stack[j].mask && j == i + 1
        if (stack[j].mask && !joined) || stack[i].mask
            || (masksBelow(i) && !joined) || masksBelow(j) {
            return String(localized: "A mask ties one of these layers to a layer outside the two")
        }
        return nil
    }

    /// The two selected layers made one, in the lower one's place.
    func mergeSelectedLayers() {
        guard let core, let projectId, let pic = stagePicture,
              let (lo, hi) = mergePair, mergeProblem == nil else { return }
        commitSelection()
        persistAdjustments()
        persistCrop()
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id), "merge",
                             layer: lo, extra: ["with": hi])
        if !r.ok { note("error", r.message) }
        reloadAssets()
        selectLayer(lo)
        recomposite()
    }

    /// A layer made the mask of the layer beneath it, or released.
    func setLayerMask(_ id: String, _ mask: Bool) {
        guard let core, let projectId, let pic = stageStack else { return }
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             mask ? "mask" : "unmask", layer: id)
        if !r.ok { note("error", r.message) }
        layersChanged()
    }

    /// A layer clicked with ⌘: in or out of the selection.
    func toggleLayerSelection(_ id: String) {
        if selectedLayers.contains(id) && selectedLayers.count > 1 {
            selectedLayers.remove(id)
            if activeLayer == id, let other = selectedLayers.first {
                let keep = selectedLayers
                selectLayer(other)
                selectedLayers = keep
            }
        } else {
            let keep = selectedLayers.union([id])
            selectLayer(id)
            selectedLayers = keep
        }
    }
}

/// A stroke let go: its points and brush, and -- once the core has
/// painted it -- the count of strokes painted by then.
struct PendingStroke: Equatable {
    var id: Int
    var asset: String   // the picture it is drawn on
    var stroke: MarkupOverlay.Stroke
    var painted: Int?
}
