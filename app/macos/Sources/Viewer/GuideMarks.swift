import AppKit
import SwiftUI

/// A guide: horizontal (a line across, its place down from the top) or
/// vertical, by its place in its list.
struct GuideRef: Equatable {
    var horizontal: Bool
    var index: Int
}

// MARK: - The guides, in the model

extension AppModel {
    /// What the guides measure, in pixels: what the stage shows -- a
    /// clip's or a timeline's canvas, a crop's, a picture's. None while it
    /// shows nothing of the project's (a preview, a picture about to be
    /// edited) or a sound: the lines are the thirds there, unmarked.
    var guideCanvas: CGSize? {
        guard stageAssetId != nil, stage.clip == nil else { return nil }
        if stage.video != nil {
            guard !stageIsAudio else { return nil }
            if let s = stackPlayback, s.width > 0, s.height > 0 {
                return CGSize(width: s.width, height: s.height)
            }
            if let f = currentClip?.info?.frame, f.w > 0, f.h > 0 {
                return CGSize(width: f.w, height: f.h)
            }
            return nil
        }
        if let c = stageCropPlacement { return c.canvas }
        return stage.a.map { CGSize(width: $0.width, height: $0.height) }
    }

    /// The stage's guides, in its pixels: set by hand, else the thirds.
    var stageGuides: GuideSet? {
        guard let s = guideCanvas else { return nil }
        return stageAssetId.flatMap { guideSets[$0] } ?? .thirds(s)
    }

    /// Where the lines go, as fractions of what they go over.
    var stageGuideFractions: GuideFractions {
        guard let s = guideCanvas, let g = stageGuides else { return .thirds }
        return g.fractions(s)
    }

    /// The stage's guides are set by hand: View › Reset Guides has
    /// something to do.
    var guidesSetByHand: Bool {
        stageAssetId.map { guideSets[$0] != nil } ?? false
    }

    /// A guide at `px` -- its pixels down, or across -- kept on what it
    /// measures. `keep` false: shown there, not yet written into the
    /// project (a drag's steps; its end writes it).
    func moveGuide(_ r: GuideRef, to px: Double, keep: Bool = true) {
        changeGuides(keep: keep) { g, s in
            let v = min(max(0, px), r.horizontal ? s.height : s.width)
            if r.horizontal, g.h.indices.contains(r.index) {
                g.h[r.index] = v
            } else if !r.horizontal, g.v.indices.contains(r.index) {
                g.v[r.index] = v
            }
        }
    }

    /// A new guide at `px`: which it is.
    @discardableResult
    func addGuide(horizontal: Bool, at px: Double) -> GuideRef? {
        var made: GuideRef?
        changeGuides { g, s in
            let v = min(max(0, px), horizontal ? s.height : s.width)
            if horizontal {
                g.h.append(v)
                made = GuideRef(horizontal: true, index: g.h.count - 1)
            } else {
                g.v.append(v)
                made = GuideRef(horizontal: false, index: g.v.count - 1)
            }
        }
        return made
    }

    func removeGuide(_ r: GuideRef) {
        guideEditing = nil
        changeGuides { g, _ in
            if r.horizontal, g.h.indices.contains(r.index) {
                g.h.remove(at: r.index)
            } else if !r.horizontal, g.v.indices.contains(r.index) {
                g.v.remove(at: r.index)
            }
        }
    }

    /// View › Reset Guides: the thirds again.
    func resetGuides() {
        guard let id = stageAssetId, guideSets[id] != nil else { return }
        guideEditing = nil
        guideSets[id] = nil
        keepGuides()
    }

    /// The stage's guides changed -- set by hand from then on.
    private func changeGuides(keep: Bool = true,
                              _ change: (inout GuideSet, CGSize) -> Void) {
        guard let id = stageAssetId, let s = guideCanvas,
              var g = stageGuides else { return }
        let was = guideSets[id]
        change(&g, s)
        if g != stageGuides { guideSets[id] = g }
        // Kept: written when it differs from what the project holds.
        if keep, was != g || keptGuides(id) != g { keepGuides() }
    }

    /// The asset's guides as the project's view state holds them.
    private func keptGuides(_ id: String) -> GuideSet? {
        guard let core, let projectId,
              let view = core.viewState(projectId)["view"] as? [String: Any]
        else { return nil }
        return GuideSet(json: (view["guides"] as? [String: Any])?[id])
    }

    /// The guides as the project keeps them (its view state's "guides":
    /// {asset: {"h", "v"}}).
    func loadGuides() {
        guideEditing = nil
        guard let core, let projectId,
              let view = core.viewState(projectId)["view"] as? [String: Any],
              let all = view["guides"] as? [String: Any] else {
            guideSets = [:]
            return
        }
        var out: [String: GuideSet] = [:]
        for (id, j) in all {
            if let g = GuideSet(json: j) { out[id] = g }
        }
        guideSets = out
    }

    /// Into the project: a change to it, saved with it (not undone).
    private func keepGuides() {
        noteView(["guides": guideSets.isEmpty ? nil
                      : guideSets.mapValues(\.json)])
        refreshDocument()
    }

    /// The guides in MARKUP's pixels (its canvas lies at an origin on the
    /// stage's): what a moved object snaps to, while the magnet is on and
    /// they show.
    var markupSnapLines: (xs: [Double], ys: [Double])? {
        guard snapsToGuides, showsGuides, let g = stageGuides else {
            return nil
        }
        let o = markupStageOrigin
        return (g.v.map { $0 - o.x }, g.h.map { $0 - o.y })
    }
}

// MARK: - The marks beside the stage

/// The guides' MARKS (View › Show Guides), beside the stage's card, never
/// on it: a horizontal guide's triangle at its right edge, a vertical
/// one's under its bottom edge, each where its line is -- moving with it
/// as the picture is zoomed and panned, gone while its line is out of
/// view -- its place in pixels after it, in Georgia. A click on a number
/// types over it (Return moves the guide there, Escape leaves it, ↑ / ↓ a
/// pixel up or down -- ⇧ ten); meanwhile its triangle and number are
/// black, the triangle is DRAGGED along the edge (locked otherwise) and
/// a "−" beside it removes it. The "+" at the corner, in line with both,
/// adds one.
struct GuideMarks: View {
    @Bindable var model: AppModel
    /// The card's size; where what the guides measure lies on it.
    let size: CGSize
    let rect: CGRect
    /// Its pixels, and the guides in them.
    let canvas: CGSize
    let guides: GuideSet

    /// A triangle dragged: its guide's place as it was pressed.
    @State private var dragFrom: Double?

    /// Room beside the card, right and below.
    static let gutter = CGSize(width: 56, height: 26)
    /// A triangle's depth and its gap from the card.
    private static let tri: CGFloat = 7
    private static let gap: CGFloat = 2

    var body: some View {
        ZStack(alignment: .topLeading) {
            Color.clear.allowsHitTesting(false)
            ForEach(Array(guides.h.enumerated()), id: \.offset) { i, px in
                let y = rect.minY + CGFloat(px / max(1, canvas.height))
                    * rect.height
                if y >= -0.5, y <= size.height + 0.5 {
                    mark(GuideRef(horizontal: true, index: i), px, at: y)
                }
            }
            ForEach(Array(guides.v.enumerated()), id: \.offset) { i, px in
                let x = rect.minX + CGFloat(px / max(1, canvas.width))
                    * rect.width
                if x >= -0.5, x <= size.width + 0.5 {
                    mark(GuideRef(horizontal: false, index: i), px, at: x)
                }
            }
            addMenu
                .position(x: size.width + Self.gap + Self.tri / 2,
                          y: size.height + Self.gap + Self.tri / 2)
        }
        .frame(width: size.width, height: size.height, alignment: .topLeading)
        // Another asset on the stage: its own guides, none typed over.
        .onChange(of: model.stageAssetId) { _, _ in
            model.guideEditing = nil
        }
    }

    /// The column of the horizontal guides' triangles, the row of the
    /// vertical ones'.
    private var column: CGFloat { size.width + Self.gap + Self.tri / 2 }
    private var row: CGFloat { size.height + Self.gap + Self.tri / 2 }

    @ViewBuilder
    private func mark(_ r: GuideRef, _ px: Double, at: CGFloat)
        -> some View {
        let editing = model.guideEditing == r
        let h = r.horizontal
        GuideTriangle(horizontal: h)
            .fill(editing ? Color.primary : Color.secondary)
            .frame(width: h ? Self.tri : Self.tri * 1.3,
                   height: h ? Self.tri * 1.3 : Self.tri)
            .position(x: h ? column : at, y: h ? at : row)
            .allowsHitTesting(false)
        // Its number open: the triangle a grip, dragged along the edge.
        if editing {
            GuideGrip(horizontal: h) { phase, d in drag(r, px, phase, d) }
                .frame(width: 16, height: 16)
                .position(x: h ? column : at, y: h ? at : row)
        }
        Group {
            if editing {
                GuideField(model: model, ref: r, value: px)
            } else {
                Button {
                    model.guideEditing = r
                } label: {
                    Text(verbatim: Self.label(px))
                        .font(.custom("Georgia", size: 10.5))
                        .foregroundStyle(.secondary)
                        .fixedSize()
                        .padding(.horizontal, 2)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help(h ? "Horizontal guide: click to type where it goes"
                        : "Vertical guide: click to type where it goes")
            }
        }
        .frame(width: 46, height: 14, alignment: h ? .leading : .top)
        .position(x: h ? column + Self.tri / 2 + 3 + 23 : at,
                  y: h ? at : row + Self.tri / 2 + 1 + 7)
        // While its number is typed over: a "−" beside its triangle
        // (under a horizontal one's, right of a vertical one's).
        if editing {
            Button {
                model.removeGuide(r)
            } label: {
                GuideGlyph(symbol: "minus")
            }
            .buttonStyle(.plain)
            .help("Remove this guide")
            .accessibilityLabel(Text("Remove Guide"))
            .position(x: h ? column : at + Self.tri + 6,
                      y: h ? at + Self.tri + 6 : row)
        }
    }

    /// The corner's "+": a horizontal or a vertical guide, in the middle
    /// of what is in view, its number open to type.
    private var addMenu: some View {
        Menu {
            Button("Add Horizontal Guide") { add(horizontal: true) }
            Button("Add Vertical Guide") { add(horizontal: false) }
        } label: {
            GuideGlyph(symbol: "plus")
        }
        // Its label as drawn, as the "−"'s.
        .menuStyle(.button)
        .buttonStyle(.plain)
        .menuIndicator(.hidden)
        .fixedSize()
        .frame(width: 14, height: 14)
        .help("Add a guide")
        .accessibilityLabel(Text("Add Guide"))
    }

    private func add(horizontal: Bool) {
        let shown = rect.intersection(CGRect(origin: .zero, size: size))
        let mid = shown.isNull ? rect : shown
        let px = horizontal
            ? Double((mid.midY - rect.minY) / max(1, rect.height)
                     * canvas.height)
            : Double((mid.midX - rect.minX) / max(1, rect.width)
                     * canvas.width)
        if let r = model.addGuide(horizontal: horizontal,
                                  at: px.rounded()) {
            model.guideEditing = r
        }
    }

    /// The triangle dragged `d` points along its edge (down, right): the
    /// guide from where it was pressed, by whole pixels, written as it is
    /// let go.
    private func drag(_ r: GuideRef, _ px: Double, _ phase: GuideGrip.Phase,
                      _ d: CGFloat) {
        switch phase {
        case .began:
            dragFrom = px
            model.guideDragging = r
        case .changed, .ended:
            let perPixel = r.horizontal ? rect.height / max(1, canvas.height)
                                        : rect.width / max(1, canvas.width)
            let v = (dragFrom ?? px) + Double(d / max(1e-6, perPixel))
            model.moveGuide(r, to: v.rounded(), keep: phase == .ended)
            if phase == .ended {
                dragFrom = nil
                model.guideDragging = nil
            }
        }
    }

    /// "213", "213.3": whole pixels as they are, a third to a tenth.
    static func label(_ px: Double) -> String {
        let t = (px * 10).rounded() / 10
        return t == t.rounded() ? String(Int(t))
                                : String(format: "%.1f", t)
    }
}

/// A guide's mark: a small triangle pointing at its line on the card --
/// left, beside a horizontal one; up, under a vertical one.
private struct GuideTriangle: Shape {
    let horizontal: Bool

    func path(in r: CGRect) -> Path {
        var p = Path()
        if horizontal {
            p.move(to: CGPoint(x: r.minX, y: r.midY))
            p.addLine(to: CGPoint(x: r.maxX, y: r.minY))
            p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        } else {
            p.move(to: CGPoint(x: r.midX, y: r.minY))
            p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
            p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        }
        p.closeSubpath()
        return p
    }
}

/// A guide's number typed over: Return moves it there, Escape -- or the
/// pointer going elsewhere -- leaves it where it was.
private struct GuideField: View {
    @Bindable var model: AppModel
    let ref: GuideRef
    let value: Double
    @State private var text = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("", text: $text)
            .textFieldStyle(.plain)
            .font(.custom("Georgia", size: 10.5))
            // Under a vertical guide's triangle, centred on it.
            .multilineTextAlignment(ref.horizontal ? .leading : .center)
            .padding(.horizontal, 2)
            .background(RoundedRectangle(cornerRadius: 3)
                .fill(Color(nsColor: .textBackgroundColor)))
            .overlay(RoundedRectangle(cornerRadius: 3)
                .strokeBorder(Color.accentColor, lineWidth: 1))
            .frame(width: 46)
            .focused($focused)
            .onAppear {
                text = GuideMarks.label(value)
                focused = true
            }
            // Moved by its triangle, or a step: the number with it.
            .onChange(of: value) { _, v in text = GuideMarks.label(v) }
            // ↑ up (or left), ↓ down (right): a pixel, ⇧ ten -- from what
            // is typed.
            .onKeyPress(keys: [.upArrow, .downArrow]) { press in
                let by = press.modifiers.contains(.shift) ? 10.0 : 1.0
                let from = Double(text.trimmingCharacters(in: .whitespaces)
                    .replacingOccurrences(of: ",", with: ".")) ?? value
                model.moveGuide(ref, to: press.key == .upArrow ? from - by
                                                               : from + by)
                selectNumber()
                return .handled
            }
            .onSubmit {
                if let v = Double(text.trimmingCharacters(in: .whitespaces)
                    .replacingOccurrences(of: ",", with: ".")) {
                    model.moveGuide(ref, to: v)
                }
                model.guideEditing = nil
            }
            .onExitCommand { model.guideEditing = nil }
            // Let go elsewhere: left as it was -- after a moment, so the
            // "−" beside it is pressed first.
            .onChange(of: focused) { _, now in
                guard !now else { return }
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) {
                    if model.guideEditing == ref, !focused,
                       model.guideDragging == nil {
                        model.guideEditing = nil
                    }
                }
            }
            // Its triangle let go: the keyboard back in the number.
            .onChange(of: model.guideDragging) { _, now in
                guard now == nil, model.guideEditing == ref else { return }
                focused = true
                selectNumber()
            }
            .accessibilityLabel(Text(ref.horizontal ? "Horizontal guide"
                                                    : "Vertical guide"))
    }

    /// The number selected whole once it has moved -- by a step, a drag
    /// -- so what is typed next replaces it (its new text lands a moment
    /// after, the caret at its end).
    private func selectNumber() {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.08) {
            guard model.guideEditing == ref, focused,
                  let editor = model.editorWindow?.firstResponder as? NSText
            else { return }
            editor.selectAll(nil)
        }
    }
}

/// The "+" and the "−" beside the stage: one glyph, drawn alike.
private struct GuideGlyph: View {
    let symbol: String

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: 10, weight: .medium))
            .foregroundStyle(.primary.opacity(0.75))
            .frame(width: 14, height: 14)
            .contentShape(Rectangle())
    }
}

/// A guide's triangle as a GRIP while its number is open: an AppKit view
/// over it (the pointer's own path -- the resize cursor, a press that
/// leaves the number its keyboard), handing over how far it has been
/// dragged along its edge, in points: down for a horizontal guide's,
/// right for a vertical one's.
struct GuideGrip: NSViewRepresentable {
    enum Phase { case began, changed, ended }
    let horizontal: Bool
    let onDrag: (Phase, CGFloat) -> Void

    func makeNSView(context: Context) -> GripView { GripView() }

    func updateNSView(_ v: GripView, context: Context) {
        if v.horizontal != horizontal {
            v.horizontal = horizontal
            v.window?.invalidateCursorRects(for: v)
        }
        v.onDrag = onDrag
    }

    final class GripView: NSView {
        var horizontal = true
        var onDrag: (Phase, CGFloat) -> Void = { _, _ in }
        private var downAt: CGPoint?

        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        override func resetCursorRects() {
            addCursorRect(bounds, cursor: horizontal ? .resizeUpDown
                                                     : .resizeLeftRight)
        }

        /// How far from the press, along the edge (the window's y is up).
        private func along(_ e: NSEvent) -> CGFloat {
            guard let a = downAt else { return 0 }
            let p = e.locationInWindow
            return horizontal ? a.y - p.y : p.x - a.x
        }

        override func mouseDown(with e: NSEvent) {
            downAt = e.locationInWindow
            onDrag(.began, 0)
        }

        override func mouseDragged(with e: NSEvent) {
            guard downAt != nil else { return }
            onDrag(.changed, along(e))
        }

        override func mouseUp(with e: NSEvent) {
            guard downAt != nil else { return }
            onDrag(.ended, along(e))
            downAt = nil
        }
    }
}

/// The markup toolbar's magnet: a horseshoe, its poles down.
struct MagnetShape: Shape {
    func path(in r: CGRect) -> Path {
        let w = r.width
        let arm = w * 0.34
        let outer = w / 2, inner = outer - arm
        let cy = r.minY + outer
        var p = Path()
        // The bend over the top, then down each arm to its pole.
        p.move(to: CGPoint(x: r.minX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.minX, y: cy))
        p.addArc(center: CGPoint(x: r.midX, y: cy), radius: outer,
                 startAngle: .degrees(180), endAngle: .degrees(0),
                 clockwise: false)
        p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        p.addLine(to: CGPoint(x: r.maxX - arm, y: r.maxY))
        p.addLine(to: CGPoint(x: r.maxX - arm, y: cy))
        p.addArc(center: CGPoint(x: r.midX, y: cy), radius: inner,
                 startAngle: .degrees(0), endAngle: .degrees(180),
                 clockwise: true)
        p.addLine(to: CGPoint(x: r.minX + arm, y: r.maxY))
        p.closeSubpath()
        return p
    }
}

/// The poles' tips, light against the magnet.
struct MagnetPoles: Shape {
    func path(in r: CGRect) -> Path {
        let arm = r.width * 0.34
        let tip = r.height * 0.26
        var p = Path()
        p.addRect(CGRect(x: r.minX, y: r.maxY - tip, width: arm, height: tip))
        p.addRect(CGRect(x: r.maxX - arm, y: r.maxY - tip, width: arm,
                         height: tip))
        return p
    }
}
