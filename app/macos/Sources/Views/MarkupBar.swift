import AppKit
import SwiftUI

/// The markup toolbar (View › Show Markup Toolbar): a narrow row under
/// the title bar's hairline, as Preview's. Its tools -- select, brush,
/// eraser, line, rectangle, ellipse, text -- then what the tool (or the
/// selection) takes: a brush's size and softness, a line's width, a
/// text's font (and line spacing), its alignment and the button opening
/// its words' edit window (MarkupTextPanel); the colour and the fill
/// (sRGB with opacity); an object's EFFECTS -- a shadow, a glow; what can
/// be done with a selection -- ARRANGE it (its order, alignment,
/// spacing), make it pixels, delete it; and which layer the markup goes
/// on.
struct MarkupBar: View {
    @Bindable var model: AppModel
    @State private var fontPanel = false

    private var m: MarkupState { model.markup }

    var body: some View {
        // Centred in the bar, as Preview's markup toolbar is.
        HStack(spacing: 12) {
            tools
            Divider().frame(height: 18)
            settings
            colors
            effectsButton
            selectionActions
            Divider().frame(height: 18)
            // Where the markup goes: the first to give way.
            Text(verbatim: target)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .layoutPriority(-1)
        }
        .disabled(!model.markupReady)
        .controlSize(.small)
        .padding(.horizontal, 12)
        .frame(height: 34)
        .frame(maxWidth: .infinity, alignment: .center)
        .background(.bar)
        .overlay(alignment: .bottom) { Divider() }
        .onChange(of: m.tool) { _, _ in model.markupToolChanged() }
        // The text's edit window follows the selection, and closes with
        // the toolbar.
        .onChange(of: m.selectedText) { _, _ in m.textPanel.follow(model) }
        .onChange(of: model.markupReady) { _, _ in
            m.textPanel.follow(model)
        }
        .onDisappear { m.textPanel.close() }
    }

    // MARK: Tools

    private var tools: some View {
        HStack(spacing: 2) {
            ForEach(MarkupTool.allCases) { t in
                Button {
                    m.tool = t
                } label: {
                    Image(systemName: t.symbol)
                        .frame(width: 26, height: 22)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.borderless)
                .background(RoundedRectangle(cornerRadius: 5)
                    .fill(m.tool == t ? Color.accentColor.opacity(0.22)
                                      : .clear))
                .foregroundStyle(m.tool == t ? Color.accentColor
                                             : Color.primary)
                // Its key beside its name: "Brush (B)".
                .help(t.key.map { "\(t.label) (\(String($0).uppercased()))" }
                      ?? t.label)
                .accessibilityLabel(Text(verbatim: t.label))
                .shortcutHint(t.key.map { String($0).uppercased() } ?? "")
                if t == .select {
                    deselectButton
                    magnetButton
                }
            }
        }
    }

    /// Beside Select: the selection let go, as ⌘D (Edit › Deselect) --
    /// greyed while nothing is selected.
    private var deselectButton: some View {
        Button {
            model.deselectMarkup()
        } label: {
            Image(systemName: "cursorarrow.slash")
                .frame(width: 26, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .shortcutHint("⌘D")
        .disabled(!model.markupCanDeselect)
        .help("Deselect (⌘D)")
        .accessibilityLabel(Text("Deselect"))
    }

    /// Beside them: moves snap to the guides while it is on (and they
    /// show) -- an object's edges or middle onto a line within reach.
    private var magnetButton: some View {
        let on = model.snapsToGuides
        return Button {
            model.snapsToGuides.toggle()
        } label: {
            ZStack {
                MagnetShape()
                    .fill(on ? Color.accentColor : Color.primary)
                MagnetPoles()
                    .fill(Color(nsColor: .controlBackgroundColor)
                        .opacity(0.85))
            }
            .frame(width: 10, height: 13)
            // Its poles up to the right, as a magnet is drawn.
            .rotationEffect(.degrees(-135))
            .frame(width: 26, height: 22)
            .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .background(RoundedRectangle(cornerRadius: 5)
            .fill(on ? Color.accentColor.opacity(0.22) : .clear))
        .help(on ? "Snap to Guides: on — moved objects stop on the guides"
                 : "Snap to Guides: moved objects stop on the guides (View › Show Guides)")
        .accessibilityLabel(Text("Snap to Guides"))
        .accessibilityAddTraits(on ? .isSelected : [])
    }

    // MARK: Settings

    /// What the tool -- or, with the select tool, the selection -- takes.
    @ViewBuilder
    private var settings: some View {
        let kinds = Set(m.selection.map(\.kind))
        let tool = m.tool
        if tool == .brush || tool == .eraser {
            slider("Size", value: Binding(get: { m.radius },
                                          set: { m.radius = $0 }),
                   range: 1...200, text: String(Int(m.radius.rounded())),
                   width: 110)
                .shortcutHint("[  ]")
            slider("Softness", value: Binding(get: { m.softness },
                                              set: { m.softness = $0 }),
                   range: 0...1,
                   text: m.softness.formatted(
                       .percent.precision(.fractionLength(0))),
                   width: 80)
                .shortcutHint("⇧[  ⇧]")
        }
        if tool == .line || tool == .rect || tool == .ellipse
            || tool == .polygon
            || kinds.contains(.line) || kinds.contains(.rect)
            || kinds.contains(.ellipse) || kinds.contains(.polygon) {
            slider("Width", value: Binding(
                       get: { m.selection.first { $0.kind != .text }?.width
                              ?? m.width },
                       set: { w in
                           m.width = w
                           model.editSelection(commit: false) {
                               if $0.kind != .text { $0.width = w }
                           }
                       }),
                   range: 0...60, text: String(Int(m.width.rounded())),
                   width: 90)
        }
        if tool == .text || kinds.contains(.text) {
            fontButton
            alignment
        }
        if m.selectedText != nil {
            Button {
                model.editMarkupText()
            } label: {
                Image(systemName: "character.cursor.ibeam")
                    .frame(width: 26, height: 22)
                    .contentShape(Rectangle())
            }
            .buttonStyle(.borderless)
            .help("Edit the text in its own window")
            .accessibilityLabel(Text("Edit Text"))
        }
    }

    /// "Size ▬▬○ 24": a slider with its label and value; it keeps the
    /// selection's values when let go.
    private func slider(_ name: LocalizedStringKey,
                        value: Binding<Double>,
                        range: ClosedRange<Double>, text: String,
                        width: CGFloat) -> some View {
        HStack(spacing: 5) {
            // Whole: a narrow bar wrapped it ("Siz / e").
            Text(name).foregroundStyle(.secondary)
                .fixedSize()
            Slider(value: value, in: range) { editing in
                if !editing { model.commitSelection(keep: true) }
            }
            .frame(width: width)
            Text(verbatim: text)
                .monospacedDigit()
                .frame(minWidth: 28, alignment: .leading)
        }
    }

    private var fontButton: some View {
        let f = m.selectedText?.font ?? m.font
        let size = String(Int(f.size.rounded()))
        return Button {
            fontPanel = true
        } label: {
            HStack(spacing: 4) {
                Image(systemName: "textformat.size")
                Text(verbatim: "\(f.family) \(size)")
                    .lineLimit(1)
            }
        }
        .help("Font")
        .popover(isPresented: $fontPanel, arrowEdge: .bottom) {
            FontPanel(font: Binding(
                get: { m.selectedText?.font ?? m.font },
                set: { nf in
                    m.font = nf
                    model.editSelection {
                        if $0.kind == .text { $0.font = nf }
                    }
                }),
                      spacing: Binding(
                get: {
                    m.selection.first { $0.kind == .text }
                        .map { $0.lineSpacing ?? 1 } ?? m.lineSpacing
                },
                set: { v in
                    let v = min(3, max(1, v))
                    m.lineSpacing = v
                    model.editSelection {
                        if $0.kind == .text {
                            $0.lineSpacing = v == 1 ? nil : v
                        }
                    }
                }))
        }
    }

    /// A text box's alignment: its lines left, centred, right or
    /// justified in the box -- the selected texts', or the next one's.
    private var alignment: some View {
        Picker("Alignment", selection: Binding(
            get: {
                m.selection.first { $0.kind == .text }
                    .map { $0.align ?? "left" } ?? m.align
            },
            set: { a in
                m.align = a
                model.editSelection {
                    if $0.kind == .text { $0.align = a == "left" ? nil : a }
                }
            })) {
            Label("Align Left", systemImage: "text.alignleft")
                .tag("left")
            Label("Center", systemImage: "text.aligncenter")
                .tag("center")
            Label("Align Right", systemImage: "text.alignright")
                .tag("right")
            Label("Justify", systemImage: "text.justify")
                .tag("justified")
        }
        .pickerStyle(.segmented)
        .labelStyle(.iconOnly)
        .labelsHidden()
        .fixedSize()
        .help("How the text's lines lie in its box")
    }

    // MARK: Effects

    /// A shadow and a glow: the selected objects', or the next ones'. A
    /// dot on the button while any is on.
    @ViewBuilder
    private var effectsButton: some View {
        if m.tool.shape != nil || m.tool == .text || m.tool == .polygon
            || !m.selection.isEmpty {
            let fx = m.selection.first?.effects ?? m.effects
            Button {
                m.effectsOpen = true
            } label: {
                Image(systemName: fx.isEmpty ? "sparkles"
                                             : "sparkles.rectangle.stack")
                    .frame(width: 26, height: 22)
                    .contentShape(Rectangle())
                    .foregroundStyle(fx.isEmpty ? Color.primary
                                                : Color.accentColor)
            }
            .buttonStyle(.borderless)
            .help("Effects: a shadow, a glow")
            .accessibilityLabel(Text("Effects"))
            .popover(isPresented: Binding(get: { m.effectsOpen },
                                          set: { m.effectsOpen = $0 }),
                     arrowEdge: .bottom) {
                EffectsPanel(
                    effects: m.selection.first?.effects ?? m.effects,
                    size: m.selectedText?.font.size
                        ?? (m.tool == .text ? m.font.size : 40),
                    color: m.selection.first?.stroke ?? m.edge,
                    change: { fx, commit in
                        m.effects = fx
                        model.editSelection(commit: commit) {
                            $0.effects = fx
                        }
                    },
                    ended: { model.commitSelection(keep: true) })
            }
        }
    }

    // MARK: Colours

    private var colors: some View {
        let kinds = Set(m.selection.map(\.kind))
        let fills = m.tool == .rect || m.tool == .ellipse
            || m.tool == .polygon
            || kinds.contains(.rect) || kinds.contains(.ellipse)
            || kinds.contains(.polygon)
        return HStack(spacing: 8) {
            HStack(spacing: 3) {
                Text("Color").foregroundStyle(.secondary)
                ColorPicker("Color", selection: Binding(
                    get: { (m.selection.first?.stroke ?? m.edge).cgColor },
                    set: { c in
                        m.edge = RGBA(cgColor: c)
                        model.editSelection { $0.stroke = m.edge }
                    }), supportsOpacity: true)
                    .labelsHidden()
            }
            .help("The colour lines, outlines, text and the brush draw with")
            HStack(spacing: 3) {
                Text("Fill").foregroundStyle(.secondary)
                ColorPicker("Fill", selection: Binding(
                    get: {
                        (m.selection.first { $0.fillable }?.fill
                            ?? m.fill).cgColor
                    },
                    set: { c in
                        m.fill = RGBA(cgColor: c)
                        model.editSelection {
                            if $0.fillable { $0.fill = m.fill }
                        }
                    }), supportsOpacity: true)
                    .labelsHidden()
            }
            .disabled(!fills)
            .help("What rectangles, ellipses and polygons are filled with")
            if fills || kinds.contains(.text) {
                gradientButton
            }
        }
    }

    /// The gradient: the selected shapes' insides or texts' words, or the
    /// next shapes'. Lit while there is one.
    private var gradientButton: some View {
        let on = model.markupGradient != nil
        return Button {
            m.gradientOpen = true
        } label: {
            Image(systemName: "circle.lefthalf.filled")
                .frame(width: 26, height: 22)
                .contentShape(Rectangle())
                .foregroundStyle(on ? Color.accentColor : Color.primary)
        }
        .buttonStyle(.borderless)
        .help("Gradient: linear or radial, in place of the fill")
        .accessibilityLabel(Text("Gradient"))
        .popover(isPresented: Binding(get: { m.gradientOpen },
                                      set: { m.gradientOpen = $0 }),
                 arrowEdge: .bottom) {
            GradientPanel(
                gradient: model.markupGradient,
                base: model.markupGradientBase,
                change: { g, commit in
                    model.setMarkupGradient(g, commit: commit)
                },
                ended: { model.commitSelection(keep: true) })
        }
    }

    // MARK: The selection

    @ViewBuilder
    private var selectionActions: some View {
        if !m.selection.isEmpty {
            arrangeButton
            Button {
                model.materializeSelection()
            } label: {
                Label("Make Pixels", systemImage: "square.grid.3x3.fill")
            }
            .help("Draw the selected objects into the layer's pixels: they are no longer editable as shapes or text")
            Button {
                model.deleteSelectedObjects()
            } label: {
                Image(systemName: "trash")
            }
            .buttonStyle(.borderless)
            .help("Delete the selected objects")
        }
    }

    /// The selection's ARRANGEMENT: its place in its layer's order, its
    /// alignment, its spacing.
    private var arrangeButton: some View {
        Button {
            m.arrangeOpen = true
        } label: {
            Image(systemName: "square.3.layers.3d")
                .frame(width: 26, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .help("Arrange: the order, alignment and spacing of the selected objects")
        .accessibilityLabel(Text("Arrange"))
        .popover(isPresented: Binding(get: { m.arrangeOpen },
                                      set: { m.arrangeOpen = $0 }),
                 arrowEdge: .bottom) {
            ArrangePanel(model: model)
        }
    }

    /// Where markup goes: the selection's layer, the layer the next mark
    /// goes on, or a new one.
    private var target: String {
        guard model.markupReady else {
            return model.markupAsset == nil
                ? String(localized: "Markup goes on a picture or a clip on the stage")
                : String(localized: "Show the picture alone to mark it up")
        }
        if let l = m.selectionLayer,
           let layer = model.markupAsset?.layerStack.first(where: {
               $0.id == l }) {
            return String(localized: "On \(layer.title)")
        }
        if let layer = model.markupTargetPreview {
            // Locked: nothing goes on it -- said before the press is.
            return layer.locked
                ? String(localized: "\(layer.title) is locked")
                : String(localized: "On \(layer.title)")
        }
        return String(localized: "On a new layer")
    }
}

/// ARRANGE: the selected objects moved up or down their layer's order
/// (drawn over or under the others), their edges or middles lined up --
/// one object alone on the picture -- and three or more spaced evenly.
/// Open while it is used: one change after another. The order's keys --
/// Photoshop's -- are the Arrange menu's, shown beside each.
private struct ArrangePanel: View {
    let model: AppModel

    private var count: Int { model.markup.selection.count }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            section("Order")
            Grid(horizontalSpacing: 6, verticalSpacing: 6) {
                GridRow {
                    labeled(.toTop)
                    labeled(.up)
                }
                GridRow {
                    labeled(.toBottom)
                    labeled(.down)
                }
            }
            Divider()
            HStack(alignment: .firstTextBaseline) {
                section("Align")
                Spacer()
                Text(count > 1 ? "To each other" : "On the picture")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            HStack(spacing: 4) {
                ForEach(MarkupArrange.align.prefix(3)) { icon($0) }
                Spacer().frame(width: 10)
                ForEach(MarkupArrange.align.suffix(3)) { icon($0) }
            }
            Divider()
            HStack(alignment: .firstTextBaseline) {
                section("Distribute")
                Spacer()
                if count < 3 {
                    Text("Three or more objects")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            HStack(spacing: 6) {
                labeled(.distributeH, String(localized: "Horizontally"))
                labeled(.distributeV, String(localized: "Vertically"))
            }
        }
        .controlSize(.small)
        .padding(12)
        .frame(width: 330)
    }

    private func section(_ title: LocalizedStringKey) -> some View {
        Text(title).font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
    }

    /// A button with its name -- the order's, the spacing's -- and its
    /// key (the Arrange menu's).
    private func labeled(_ how: MarkupArrange,
                         _ title: String? = nil) -> some View {
        Button {
            model.arrangeSelection(how)
        } label: {
            HStack(spacing: 4) {
                Label {
                    Text(verbatim: title ?? how.label)
                } icon: {
                    Image(systemName: how.symbol)
                }
                Spacer(minLength: 4)
                if let key = how.shortcutText {
                    Text(verbatim: key).foregroundStyle(.secondary)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .shortcutHint(how.shortcutText ?? "")
        .disabled(!model.markupCanArrange(how))
        .help(how.shortcutText.map { "\(how.label) (\($0))" } ?? how.label)
    }

    /// An alignment's button: its picture, its name its tooltip.
    private func icon(_ how: MarkupArrange) -> some View {
        Button {
            model.arrangeSelection(how)
        } label: {
            Image(systemName: how.symbol)
                .frame(width: 30, height: 22)
                .contentShape(Rectangle())
        }
        .disabled(!model.markupCanArrange(how))
        .help(how.label)
        .accessibilityLabel(Text(verbatim: how.label))
    }
}

/// A GRADIENT's editor: none, linear or radial; its colours along a bar
/// -- a stop a marker under it, dragged along to move, down off the bar
/// to remove (two stay), a click on the bar adding one there in the
/// colour the gradient has there -- the chosen stop's colour and place,
/// and the whole reversed. Its ends are dragged on the picture (a radial
/// one's centre and edge). A stop dragged is kept when let go (`ended`);
/// the rest at once.
private struct GradientPanel: View {
    let gradient: MarkupGradient?
    let base: RGBA
    let change: (MarkupGradient?, Bool) -> Void
    let ended: () -> Void
    @State private var chosen = 0

    private static let width: CGFloat = 260
    private static let barHeight: CGFloat = 22
    /// A marker dragged this far below the bar is let go of.
    private static let away: CGFloat = 40

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("Gradient", selection: Binding(
                get: { gradient?.kind.rawValue ?? "none" },
                set: { k in
                    guard let kind = MarkupGradient.Kind(rawValue: k) else {
                        change(nil, true)
                        return
                    }
                    // Another kind keeps the colours, its ends its own.
                    let g = gradient.map {
                        MarkupGradient(kind: kind, stops: $0.stops)
                    } ?? AppModel.newGradient(kind, from: base)
                    change(g, true)
                })) {
                Text("None").tag("none")
                Text("Linear").tag("linear")
                Text("Radial").tag("radial")
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            if let g = gradient {
                bar(g)
                stopRow(g)
                Text("Drag its ends on the picture")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .controlSize(.small)
        .padding(12)
        .frame(width: Self.width + 24)
    }

    /// The colours along a bar, the stops as markers under it.
    private func bar(_ g: MarkupGradient) -> some View {
        let w = Self.width, h = Self.barHeight
        let sorted = g.stops.sorted { $0.at < $1.at }
        return ZStack(alignment: .topLeading) {
            RoundedRectangle(cornerRadius: 4)
                .fill(LinearGradient(
                    stops: sorted.map {
                        .init(color: Color(cgColor: $0.color.cgColor),
                              location: $0.at)
                    },
                    startPoint: .leading, endPoint: .trailing))
                .overlay(RoundedRectangle(cornerRadius: 4)
                    .stroke(Color.secondary.opacity(0.5)))
                .frame(width: w, height: h)
                .contentShape(Rectangle())
                // A click on the bar: a stop there, in the colour there.
                .onTapGesture(coordinateSpace: .local) { at in
                    let t = min(1, max(0, at.x / w))
                    var n = g
                    n.stops.append(.init(at: t, color: g.color(at: t)))
                    chosen = n.stops.count - 1
                    change(n, true)
                }
            ForEach(g.stops.indices, id: \.self) { i in
                let s = g.stops[i]
                Circle()
                    .fill(Color(cgColor: s.color.cgColor))
                    .frame(width: 14, height: 14)
                    .overlay(Circle().stroke(
                        i == chosen ? Color.accentColor : Color.white,
                        lineWidth: 2))
                    .shadow(radius: 1)
                    .position(x: s.at * w, y: h + 11)
                    .gesture(
                        DragGesture(minimumDistance: 0,
                                    coordinateSpace: .named("gradient-bar"))
                            .onChanged { v in
                                chosen = i
                                var n = g
                                n.stops[i].at = min(1, max(0,
                                                           v.location.x / w))
                                change(n, false)
                            }
                            .onEnded { v in
                                if v.location.y > h + Self.away,
                                   g.stops.count > 2 {
                                    var n = g
                                    n.stops.remove(at: i)
                                    chosen = 0
                                    change(n, true)
                                } else {
                                    ended()
                                }
                            })
                    .help("Drag along to move, down off the bar to remove")
            }
        }
        .frame(width: w, height: h + 22, alignment: .topLeading)
        .coordinateSpace(name: "gradient-bar")
    }

    /// The chosen stop's colour and place; removing it; reversing all.
    private func stopRow(_ g: MarkupGradient) -> some View {
        let i = min(chosen, g.stops.count - 1)
        return HStack(spacing: 8) {
            ColorPicker("Color", selection: Binding(
                get: { g.stops[i].color.cgColor },
                set: { c in
                    var n = g
                    n.stops[i].color = RGBA(cgColor: c)
                    change(n, true)
                }), supportsOpacity: true)
                .labelsHidden()
            TextField("Location", value: Binding(
                get: { (g.stops[i].at * 100).rounded() },
                set: { v in
                    var n = g
                    n.stops[i].at = min(1, max(0, v / 100))
                    change(n, true)
                }), format: .number.precision(.fractionLength(0)))
                .textFieldStyle(.roundedBorder)
                .frame(width: 44)
            Text(verbatim: "%").foregroundStyle(.secondary)
            Button {
                var n = g
                n.stops.remove(at: i)
                chosen = 0
                change(n, true)
            } label: {
                Image(systemName: "minus")
            }
            .disabled(g.stops.count <= 2)
            .help("Remove this colour")
            Spacer()
            Button {
                var n = g
                n.stops = n.stops.map { .init(at: 1 - $0.at, color: $0.color) }
                change(n, true)
            } label: {
                Image(systemName: "arrow.left.arrow.right")
            }
            .help("Reverse the gradient")
        }
    }
}

/// A markup object's EFFECTS: a drop shadow -- its colour, blur, distance
/// and angle (as Keynote's: 315° falls down and right) -- and a glow, its
/// colour and radius; canvas pixels. Switched on, each starts from values
/// for the object's size (`size`: a text's font size). A slider keeps its
/// value when let go (`ended`); a switch or a colour at once.
private struct EffectsPanel: View {
    let effects: [MarkupEffect]
    let size: Double
    let color: RGBA
    let change: ([MarkupEffect], Bool) -> Void
    let ended: () -> Void

    private func effect(_ k: MarkupEffect.Kind) -> MarkupEffect? {
        effects.first { $0.kind == k }
    }

    /// `e` in place of its kind's (nil: none), kept in the order the core
    /// draws them -- the glow inside, the shadow of it all outside.
    private func with(_ k: MarkupEffect.Kind,
                      _ e: MarkupEffect?) -> [MarkupEffect] {
        var out = effects.filter { $0.kind != k }
        if let e { out.append(e) }
        return out.filter { $0.kind == .glow }
            + out.filter { $0.kind != .glow }
    }

    /// A shadow's offset as a distance and an angle: degrees, 0 to the
    /// right, counter-clockwise (315 down and right).
    private static func distance(_ e: MarkupEffect) -> Double {
        (e.dx * e.dx + e.dy * e.dy).squareRoot()
    }

    private static func angle(_ e: MarkupEffect) -> Double {
        let a = atan2(-e.dy, e.dx) * 180 / .pi
        return a < 0 ? a + 360 : a
    }

    private var shadowDefault: MarkupEffect {
        let d = max(2, (size * 0.1).rounded())
        return MarkupEffect(kind: .shadow, color: RGBA(r: 0, g: 0, b: 0,
                                                       a: 0.5),
                            blur: max(3, (size * 0.2).rounded()),
                            dx: (d * 0.7071).rounded(),
                            dy: (d * 0.7071).rounded())
    }

    private var glowDefault: MarkupEffect {
        var c = color
        c.a = max(c.a, 0.8)
        return MarkupEffect(kind: .glow, color: c,
                            radius: max(4, (size * 0.35).rounded()))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle("Shadow", isOn: Binding(
                get: { effect(.shadow) != nil },
                set: { change(with(.shadow, $0 ? shadowDefault : nil), true) }))
                .toggleStyle(.checkbox)
            if let sh = effect(.shadow) {
                Grid(alignment: .leading, horizontalSpacing: 8,
                     verticalSpacing: 6) {
                    colorRow(sh)
                    slider("Blur", sh.blur, 0...200) { v in
                        var e = sh
                        e.blur = v
                        return e
                    }
                    let dist = Self.distance(sh)
                    let angle = Self.angle(sh)
                    slider("Distance", dist, 0...200) { v in
                        var e = sh
                        let a = (dist > 0 ? angle : 315) * .pi / 180
                        e.dx = v * cos(a)
                        e.dy = -v * sin(a)
                        return e
                    }
                    slider("Angle", angle, 0...360, unit: "°") { v in
                        var e = sh
                        let a = v * .pi / 180
                        e.dx = dist * cos(a)
                        e.dy = -dist * sin(a)
                        return e
                    }
                }
                .padding(.leading, 18)
            }
            Divider()
            Toggle("Glow", isOn: Binding(
                get: { effect(.glow) != nil },
                set: { change(with(.glow, $0 ? glowDefault : nil), true) }))
                .toggleStyle(.checkbox)
            if let g = effect(.glow) {
                Grid(alignment: .leading, horizontalSpacing: 8,
                     verticalSpacing: 6) {
                    colorRow(g)
                    slider("Radius", g.radius, 0...200) { v in
                        var e = g
                        e.radius = v
                        return e
                    }
                }
                .padding(.leading, 18)
            }
        }
        .controlSize(.small)
        .padding(12)
        .frame(width: 280)
    }

    private func colorRow(_ e: MarkupEffect) -> some View {
        GridRow {
            Text("Color").foregroundStyle(.secondary)
                .frame(width: 64, alignment: .leading)
            ColorPicker("Color", selection: Binding(
                get: { e.color.cgColor },
                set: { c in
                    var n = e
                    n.color = RGBA(cgColor: c)
                    change(with(e.kind, n), true)
                }), supportsOpacity: true)
                .labelsHidden()
        }
    }

    /// "Blur ▬▬○ 12": a value of `e`, changed live, kept when let go.
    private func slider(_ name: LocalizedStringKey, _ value: Double,
                        _ range: ClosedRange<Double>, unit: String = "",
                        _ set: @escaping (Double) -> MarkupEffect)
        -> some View {
        GridRow {
            Text(name).foregroundStyle(.secondary)
                .frame(width: 64, alignment: .leading)
            Slider(value: Binding(
                get: { min(range.upperBound, max(range.lowerBound, value)) },
                set: { v in
                    let e = set(v.rounded())
                    change(with(e.kind, e), false)
                }), in: range) { editing in
                if !editing { ended() }
            }
            .frame(width: 140)
            Text(verbatim: "\(Int(value.rounded()))\(unit)")
                .monospacedDigit()
                .frame(minWidth: 34, alignment: .trailing)
        }
    }
}

/// The text font: a family (filtered as typed), a size in canvas pixels,
/// bold, italic, underline; and a text box's line spacing.
private struct FontPanel: View {
    @Binding var font: MarkupFont
    @Binding var spacing: Double
    @State private var filter = ""
    private static let families = NSFontManager.shared.availableFontFamilies

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            TextField("Typeface", text: $filter)
                .textFieldStyle(.roundedBorder)
            List(selection: Binding(
                get: { font.family },
                set: { if let f = $0 { font.family = f } })) {
                ForEach(Self.families.filter {
                    filter.isEmpty || $0.localizedCaseInsensitiveContains(filter)
                }, id: \.self) { f in
                    Text(verbatim: f)
                        .font(.custom(f, size: 13))
                        .tag(f)
                }
            }
            .frame(width: 240, height: 200)
            HStack(spacing: 8) {
                Text("Size").foregroundStyle(.secondary)
                TextField("Size", value: Binding(
                    get: { font.size },
                    set: { font.size = min(1000, max(1, $0)) }),
                          format: .number.precision(.fractionLength(0)))
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 56)
                Stepper("Size", value: Binding(
                    get: { font.size },
                    set: { font.size = min(1000, max(1, $0)) }),
                        step: 2)
                    .labelsHidden()
                Spacer()
                Toggle(isOn: $font.bold) { Image(systemName: "bold") }
                    .help("Bold")
                Toggle(isOn: $font.italic) { Image(systemName: "italic") }
                    .help("Italic")
                Toggle(isOn: $font.underline) {
                    Image(systemName: "underline")
                }
                .help("Underline")
            }
            .toggleStyle(.button)
            HStack(spacing: 8) {
                Text("Line Spacing").foregroundStyle(.secondary)
                TextField("Line Spacing", value: $spacing,
                          format: .number.precision(.fractionLength(1)))
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 56)
                Stepper("Line Spacing", value: $spacing, in: 1...3,
                        step: 0.1)
                    .labelsHidden()
            }
        }
        .controlSize(.small)
        .padding(12)
    }
}
