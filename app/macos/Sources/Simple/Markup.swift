import AppKit
import SwiftUI

/// The markup toolbar's tools.
enum MarkupTool: String, CaseIterable, Identifiable, Sendable {
    case select, brush, eraser, line, rect, ellipse, text
    var id: String { rawValue }

    var symbol: String {
        switch self {
        case .select: "cursorarrow"
        case .brush: "paintbrush.pointed"
        case .eraser: "eraser"
        case .line: "line.diagonal"
        case .rect: "rectangle"
        case .ellipse: "circle"
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
        case .text: String(localized: "Text")
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

    enum Drag { case none, stroke, shape, move, handle(String, Int) }
    @ObservationIgnored var drag = Drag.none
    @ObservationIgnored var last = CGPoint.zero
    @ObservationIgnored var moved = false

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
        o.selected = m.selectedIds
        // Drawn by the core, in its layer's place, but while it changes.
        o.drawsSelection = m.live || m.settling
        if m.drawing, let layer = m.selectionLayer {
            o.drawingBounds = drawingBounds(layer)
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
        case .escape: commitSelection()
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
        }
    }

    /// The selected objects on the pasteboard -- the texts' words as
    /// plain text too, for the prompt and other apps -- and, cut, gone.
    func copySelectedObjects(cut: Bool) {
        let m = markup
        guard markupReady, !m.selection.isEmpty || m.drawing else { return }
        // The drawing as its PNG -- the whole canvas, where it lies --
        // held by the pasteboard alone (never a file elsewhere: an
        // anonymous session's stays in its working copy).
        var png: Data?
        if m.drawing, let layer = m.selectionLayer,
           let path = drawingPath(layer) {
            png = try? Data(contentsOf: path)
        }
        let doc: [String: Any] = [
            "objects": m.selection.map(\.json),
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
            markupChanged()
        }
        if !objects.isEmpty,
           !saveObjects(markupObjects(layer) + objects, on: layer) {
            return
        }
        if m.tool != .select && m.tool != .text { m.tool = .select }
        select(objects, on: layer)
        m.drawing = drawing != nil
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
        for l in a.layerStack.reversed()
        where l.visible && markupShows(l, in: a)
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
        o.fill = kind == .rect || kind == .ellipse ? m.fill : .clear
        o.width = m.width
        o.font = m.font
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
            m.selection = []
            m.selectionLayer = nil
            m.drawing = false
            markupChanged()
        }
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
        if let last = stack.last, last.isMarkup { return last }
        return nil
    }

    func deleteSelectedObjects() {
        let m = markup
        // The drawing selected: its pixels cleared (its objects stay but
        // those selected).
        if m.drawing, let layer = m.selectionLayer, let core, let projectId,
           let pic = markupAsset {
            m.drawing = false
            let r = core.layerOp(project: projectId,
                                 asset: composedTarget(pic.id),
                                 "clear-drawing", layer: layer)
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
        for l in a.layerStack.reversed()
        where l.visible && markupShows(l, in: a) {
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
        // A handle of the selection: reshape it.
        let reach = 6 * e.pixelsPerPoint
        for o in m.selection {
            for (i, h) in MarkupRender.handles(o).enumerated()
            where hypot(h.x - p.x, h.y - p.y) <= reach {
                m.drag = .handle(o.id, i)
                return
            }
        }
        guard let hit = markupHit(p, e) else {
            // Painted pixels there: the layer's DRAWING selected (⇧: with
            // the objects selected on it).
            if let layer = drawingHit(p, e) {
                if !(e.shift && m.selectionLayer == layer) {
                    commitSelection()
                    m.selection = []
                    m.selectionLayer = layer
                    m.selectionAsset = markupAsset?.id
                    if activeLayer != layer { selectLayer(layer) }
                }
                m.drawing = true
                m.drag = .none
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
    }

    private func selectDrag(_ e: MarkupPointer) {
        let m = markup
        let p = e.point
        // Moved or reshaped: drawn live from its first step.
        switch m.drag {
        case .move, .handle: liveSelection()
        default: break
        }
        switch m.drag {
        case .move:
            let (dx, dy) = (p.x - m.last.x, p.y - m.last.y)
            for i in m.selection.indices {
                m.selection[i].x0 += dx
                m.selection[i].x1 += dx
                m.selection[i].y0 += dy
                m.selection[i].y1 += dy
            }
            m.moved = true
        case .handle(let id, let i):
            if let k = m.selection.firstIndex(where: { $0.id == id }) {
                m.selection[k] = MarkupRender.moving(m.selection[k],
                                                     handle: i, to: p)
                m.moved = true
            }
        default:
            break
        }
        m.last = p
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
