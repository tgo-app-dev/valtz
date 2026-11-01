import SwiftUI

/// The compare's controls (vpipe compare-image's set), in the stage
/// column's contextual row (ContextBar) while the compare is on: bordered
/// groups, the mode in use pressed in.
///
///   Compare  { ↩ }  { A  B }  { side by side, stacked }
///            { wipe ↔, wipe ↕, line }  { |A−B| }  { swap }
///
/// The line button shows or hides the wipe's divider (pressed in while
/// it shows); it is for the wipes, so it is dimmed in the other modes.
/// Hidden, the divider still moves: the pointer still turns into the
/// resize cursor over it.
///
/// A picture from the sidebar's Assets -- an asset, or the picture one
/// was made from, as the model got it -- dropped on A or B takes that
/// side; B is dimmed until it has one. The side showing the CURRENT
/// state -- the picture as it is now -- has its letter bold and
/// underlined; when neither does, ↩ leads the row and puts the current
/// state back in A. With markup's row up, the same controls fold into a
/// menu at its end (CompareMenu).
///
/// (Zoom, and the zoom level, are in the toolbar.)
struct CompareBar: View {
    @Bindable var model: AppModel
    @State private var targeted: StageSlot?

    private var mode: CompareMode { model.stage.mode }
    private var hasB: Bool { model.stage.b != nil }
    private var current: StageSlot? { model.currentSlot }

    var body: some View {
        ContextBar {
            Text("Compare")
                .font(.callout)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .layoutPriority(-1)
            if current == nil {
                ContextGroup {
                    ContextButton(symbol: "arrow.uturn.backward",
                                  title: "Return to current",
                                  help: "Show the current picture in A again",
                                  action: model.returnToCurrent)
                }
                .transition(.scale(scale: 0.6).combined(with: .opacity))
            }
            ContextGroup {
                sideButton(.a)
                sideButton(.b)
            }
            if hasB {
                ContextGroup {
                    modeButton(.sideBySide)
                    modeButton(.stacked)
                }
                ContextGroup {
                    modeButton(.wipe)
                    modeButton(.wipeHorizontal)
                    lineButton
                }
                ContextGroup { modeButton(.difference) }
                ContextGroup {
                    ContextButton(symbol: "arrow.left.arrow.right",
                                  title: "Swap A and B",
                                  help: "Swap A and B",
                                  action: model.swapStage)
                }
            }
        }
        .animation(AppModel.motion, value: hasB)
        .animation(AppModel.motion, value: current == nil)
    }

    private func modeButton(_ m: CompareMode) -> some View {
        ContextButton(symbol: m.symbol,
                      title: LocalizedStringKey(m.help),
                      help: LocalizedStringKey(m.help),
                      on: m == mode) {
            model.setStageMode(m)
        }
    }

    /// A or B: shows that side -- and takes a picture dropped on it.
    private func sideButton(_ s: StageSlot) -> some View {
        let m: CompareMode = s == .a ? .a : .b
        let empty = s == .b && !hasB
        return ContextButton(symbol: m.symbol,
                             title: LocalizedStringKey(m.help),
                             help: s == current
                               ? (s == .a ? "Show A, the current picture"
                                          : "Show B, the current picture")
                               : empty
                               ? "Drop a picture from Assets here to compare it as B"
                               : LocalizedStringKey(m.help),
                             on: m == mode && !empty, text: m.label,
                             emphasized: s == current) {
            if !empty { model.setStageMode(m) }
        }
        .opacity(empty ? 0.45 : 1)
        .overlay {
            if targeted == s {
                RoundedRectangle(cornerRadius: 5, style: .continuous)
                    .strokeBorder(Color.accentColor, lineWidth: 2)
                    .allowsHitTesting(false)
            }
        }
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            return model.dropOnCompare(url, in: s)
        } isTargeted: { on in
            if on { targeted = s } else if targeted == s { targeted = nil }
        }
    }

    /// The divider, drawn across the wipe it belongs to.
    private var lineButton: some View {
        let wipes = mode == .wipe || mode == .wipeHorizontal
        return ContextButton(
            symbol: mode == .wipeHorizontal
                ? "arrow.up.and.line.horizontal.and.arrow.down"
                : "arrow.left.and.line.vertical.and.arrow.right",
            title: "Wipe line",
            help: model.stage.wipeLine ? "Hide the wipe line"
                                       : "Show the wipe line",
            on: wipes && model.stage.wipeLine,
            action: model.toggleStageWipeLine)
            .disabled(!wipes)
    }
}

/// The compare's controls folded into a menu, at the end of markup's row
/// while both are on (the row is markup's): which side or layout shows,
/// the wipe's line, the swap, the current state back in A.
struct CompareMenu: View {
    @Bindable var model: AppModel

    var body: some View {
        let hasB = model.stage.b != nil
        let wipes = model.stage.mode == .wipe
            || model.stage.mode == .wipeHorizontal
        Menu {
            if model.currentSlot == nil {
                Button("Return to current", action: model.returnToCurrent)
                Divider()
            }
            Picker("Compare", selection: Binding(
                get: { model.stage.mode },
                set: { model.setStageMode($0) })) {
                ForEach(CompareMode.allCases) { m in
                    Label(m.help, systemImage: m.symbol)
                        .tag(m)
                        .disabled(m.needsB && !hasB)
                }
            }
            .pickerStyle(.inline)
            Divider()
            Toggle("Wipe line", isOn: Binding(
                get: { model.stage.wipeLine },
                set: { _ in model.toggleStageWipeLine() }))
                .disabled(!wipes)
            Button("Swap A and B", action: model.swapStage)
                .disabled(!hasB)
        } label: {
            Label("Compare", systemImage: "square.split.2x1")
        }
        .menuIndicator(.visible)
        .fixedSize()
        .help("The compare's controls: markup's row is up, so they wait in this menu")
    }
}
