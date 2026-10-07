import AppKit
import SwiftUI

/// The markup toolbar (View › Show Markup Toolbar): a narrow row under
/// the title bar's hairline, as Preview's. Its tools -- select, brush,
/// eraser, line, rectangle, ellipse, text -- then what the tool (or the
/// selection) takes: a brush's size and softness, a line's width, a
/// text's font and the button opening its words' edit window
/// (MarkupTextPanel); the colour and the fill (sRGB with opacity);
/// what can be done with a selection; and which layer the markup goes on.
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
            selectionActions
            Divider().frame(height: 18)
            Text(verbatim: target)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
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
                .help(t.label)
                .accessibilityLabel(Text(verbatim: t.label))
            }
        }
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
            slider("Softness", value: Binding(get: { m.softness },
                                              set: { m.softness = $0 }),
                   range: 0...1,
                   text: m.softness.formatted(
                       .percent.precision(.fractionLength(0))),
                   width: 80)
        }
        if tool == .line || tool == .rect || tool == .ellipse
            || kinds.contains(.line) || kinds.contains(.rect)
            || kinds.contains(.ellipse) {
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
            Text(name).foregroundStyle(.secondary)
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
                }))
        }
    }

    // MARK: Colours

    private var colors: some View {
        let kinds = Set(m.selection.map(\.kind))
        let fills = m.tool == .rect || m.tool == .ellipse
            || kinds.contains(.rect) || kinds.contains(.ellipse)
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
                        (m.selection.first { $0.kind == .rect
                            || $0.kind == .ellipse }?.fill ?? m.fill).cgColor
                    },
                    set: { c in
                        m.fill = RGBA(cgColor: c)
                        model.editSelection {
                            if $0.kind == .rect || $0.kind == .ellipse {
                                $0.fill = m.fill
                            }
                        }
                    }), supportsOpacity: true)
                    .labelsHidden()
            }
            .disabled(!fills)
            .help("What rectangles and ellipses are filled with")
        }
    }

    // MARK: The selection

    @ViewBuilder
    private var selectionActions: some View {
        if !m.selection.isEmpty {
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

    /// Where markup goes: the selection's layer, the layer the next mark
    /// goes on, or a new one.
    private var target: String {
        guard model.markupReady else {
            return model.stagePicture == nil
                ? String(localized: "Markup goes on a picture on the stage")
                : String(localized: "Show the picture alone to mark it up")
        }
        if let l = m.selectionLayer,
           let layer = model.stagePicture?.layerStack.first(where: {
               $0.id == l }) {
            return String(localized: "On \(layer.title)")
        }
        if let layer = model.markupTargetPreview {
            return String(localized: "On \(layer.title)")
        }
        return String(localized: "On a new layer")
    }
}

/// The text font: a family (filtered as typed), a size in canvas pixels,
/// bold, italic, underline.
private struct FontPanel: View {
    @Binding var font: MarkupFont
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
        }
        .controlSize(.small)
        .padding(12)
    }
}
