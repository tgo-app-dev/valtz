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

    /// The brush stroke being drawn; kept after it is painted until the
    /// picture made with it is shown.
    var stroke: [CGPoint] = []
    var strokeErase = false
    @ObservationIgnored var strokePainted = false
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
    var selectedText: MarkupObject? {
        selection.count == 1 && selection[0].kind == .text
            ? selection[0] : nil
    }
}

extension AppModel {
    // MARK: Markup

    /// The markup toolbar has the stage's pointer: it is open over a
    /// picture of the project, shown alone.
    var markupReady: Bool {
        markupOpen && stagePicture != nil && stage.mode == .a
            && !isGenerating
    }

    /// What the stage draws over the picture for markup.
    var markupOverlay: MarkupOverlay {
        guard markupOpen else { return MarkupOverlay() }
        let m = markup
        var o = MarkupOverlay()
        if !m.stroke.isEmpty {
            o.stroke = MarkupOverlay.Stroke(
                points: m.stroke, radius: m.radius, softness: m.softness,
                color: m.edge, erase: m.strokeErase)
        }
        o.objects = m.selection + (m.draft.map { [$0] } ?? [])
        o.selected = m.selectedIds
        return o
    }

    /// The layer a markup action goes on (core: an empty selected layer,
    /// else the top markup layer, else a new one on top), made if need
    /// be, and selected in the layer editor.
    private func markupTarget() -> String? {
        guard let core, let projectId, let pic = stagePicture else {
            return nil
        }
        let selected = Array(selectedLayers.union([activeLayer]))
        var extra: [String: Any] = ["selected": selected]
        // A still with pages: drawn on the page shown.
        if pic.isPaged { extra["page"] = stagePage }
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             "markup-target", extra: extra)
        guard r.ok, let layer = r["layer"] as? String else {
            note("error", r.message)
            return nil
        }
        reloadAssets()
        if activeLayer != layer || !stageLayered { selectLayer(layer) }
        return layer
    }

    private func markupObjects(_ layer: String) -> [MarkupObject] {
        stagePicture?.layerStack.first { $0.id == layer }?.markup?.objects
            ?? []
    }

    /// A pointer on the picture, as the tool takes it.
    func markupPointer(_ e: MarkupPointer) {
        guard markupReady else { return }
        let m = markup
        let p = e.point
        switch (e.phase, m.tool) {
        case (.down, .brush), (.down, .eraser):
            commitSelection()
            m.stroke = [p]
            m.strokeErase = m.tool == .eraser
            m.strokePainted = false
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
            if m.moved { commitSelection(keep: true) }
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
        }
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
        case .cut, .copy, .delete: return !markup.selection.isEmpty
        case .paste:
            return NSPasteboard.general.availableType(
                from: [Self.markupPasteboardType]) != nil
        case .escape: return true
        }
    }

    /// The selected objects on the pasteboard -- the texts' words as
    /// plain text too, for the prompt and other apps -- and, cut, gone.
    func copySelectedObjects(cut: Bool) {
        let m = markup
        guard markupReady, !m.selection.isEmpty else { return }
        let doc: [String: Any] = [
            "objects": m.selection.map(\.json),
            "asset": stagePicture?.id ?? "",
            "cut": cut,
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: doc)
        else { return }
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setData(data, forType: Self.markupPasteboardType)
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
                as? [String: Any],
              var objects = DTO.decode([MarkupObject].self, doc["objects"]),
              !objects.isEmpty else { return }
        if pb.changeCount != m.pasteboardChange {
            m.pastes = 0
            m.pasteboardChange = pb.changeCount
        }
        let here = stagePicture?.id ?? ""
        let fromHere = (doc["asset"] as? String) == here
        let cut = doc["cut"] as? Bool ?? false
        let step = Double(m.pastes + (fromHere && !cut ? 1 : 0)) * 20
        m.pastes += 1
        var dx = step, dy = step
        let union = objects.map(MarkupRender.bounds)
            .reduce(CGRect.null) { $0.union($1) }
        if let f = markupFrame,
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
        commitSelection()
        guard let layer = markupTarget(),
              saveObjects(markupObjects(layer) + objects, on: layer)
        else { return }
        if m.tool != .select && m.tool != .text { m.tool = .select }
        select(objects, on: layer)
    }

    /// What markup is drawn on: the picture's frame, in its pixels.
    private var markupFrame: CGSize? {
        guard let pic = stagePicture else { return nil }
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

    /// The stroke just drawn, painted into the target layer.
    private func paintStroke() {
        let m = markup
        guard let core, let projectId, let pic = stagePicture,
              !m.stroke.isEmpty, let layer = markupTarget() else {
            m.stroke = []
            return
        }
        let s: [String: Any] = [
            "points": m.stroke.map { [Double($0.x), Double($0.y)] },
            "radius": m.radius, "softness": m.softness,
            "color": m.edge.json, "erase": m.strokeErase,
        ]
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id), "stroke",
                             layer: layer, extra: ["stroke": s])
        if !r.ok {
            note("error", r.message)
            m.stroke = []
            return
        }
        // Shown until the picture painted with it is.
        m.strokePainted = true
        markupChanged()
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
        guard let core, let projectId, let pic = stagePicture else {
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
        if m.selectionLayer != layer || m.selectedIds != Set(objects.map(\.id)) {
            m.selection = objects
            m.selectionLayer = layer
            m.selectionAsset = stagePicture?.id
            if activeLayer != layer { selectLayer(layer) }
            markupChanged()
        }
    }

    /// The selection's values kept on its layer; and, unless `keep`, the
    /// selection let go -- its objects drawn into the picture again.
    func commitSelection(keep: Bool = false) {
        let m = markup
        // Another picture on the stage now: the selection was its.
        if m.selectionAsset != nil && m.selectionAsset != stagePicture?.id {
            m.selection = []
            m.selectionLayer = nil
            m.selectionAsset = nil
            return
        }
        guard let layer = m.selectionLayer, !m.selection.isEmpty else {
            if !keep { m.selection = []; m.selectionLayer = nil }
            return
        }
        let byId = Dictionary(uniqueKeysWithValues:
                                m.selection.map { ($0.id, $0) })
        let now = markupObjects(layer).map { byId[$0.id] ?? $0 }
        if now != markupObjects(layer) { saveObjects(now, on: layer) }
        if !keep {
            m.selection = []
            m.selectionLayer = nil
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
        for i in m.selection.indices { change(&m.selection[i]) }
        if commit { commitSelection(keep: true) }
    }

    /// The layer the next mark goes on, as the core will choose it (an
    /// empty or markup layer among the selected, else a markup layer on
    /// top); nil: a new one.
    var markupTargetPreview: LayerDTO? {
        guard let pic = stagePicture, !(pic.layers ?? []).isEmpty else {
            return nil
        }
        // On a still with pages, the page shown's layers alone.
        let stack = pic.layerStack.filter { pic.isOnPage($0, stagePage) }
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
        guard let layer = m.selectionLayer, !m.selection.isEmpty else {
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
        guard let core, let projectId, let pic = stagePicture,
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
        for l in (stagePicture?.layerStack ?? []).reversed()
        where l.visible {
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
        if stageComposed { recomposite() }
    }

    /// The picture made with a painted stroke is shown: the stroke drawn
    /// over it can go.
    func markupComposited() {
        let m = markup
        if m.strokePainted {
            m.strokePainted = false
            m.stroke = []
        }
    }

    /// The toolbar closed, or its tool changed: the selection let go.
    func markupToolChanged() {
        let m = markup
        if m.tool != .select && m.tool != .text { commitSelection() }
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
