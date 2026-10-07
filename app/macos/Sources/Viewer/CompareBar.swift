import SwiftUI

/// The compare controls (vpipe compare-image's set), shown once there is
/// a B to compare against, in the title bar's style: glass capsules of at
/// most two buttons -- so no dividers -- with room between them; the mode
/// in use pressed in.
///
///   { ↩ }  { A, B }  { side by side, stacked }  { wipe ↔, wipe ↕ }
///   { line }  { |A−B| }  { swap }
///
/// The line button shows or hides the wipe's divider (pressed in while
/// it shows); it is for the wipes, so it is dimmed in the other modes.
/// Hidden, the divider still moves: the pointer still turns into the
/// resize cursor over it.
///
/// COMPARE: a picture from the inspector's Assets -- an asset, or the
/// picture one was made from, as the model got it -- dropped on A or B
/// takes that side. So A and B show as soon as there is a picture to
/// drop, B dimmed until it has one. The side showing the CURRENT state --
/// the picture as it is now -- has its letter bold and underlined; when
/// neither does, ↩ leads the row and puts the current state back in A.
///
/// (Zoom, and the zoom level, are in the title bar.)
struct CompareBar: View {
    let mode: CompareMode
    let hasB: Bool
    let wipeLine: Bool
    /// A picture can be dropped on A or B.
    var takesPictures = false
    /// The side showing the current state; nil while compared pictures
    /// hold both.
    var current: StageSlot? = .a
    let setMode: (CompareMode) -> Void
    let toggleWipeLine: () -> Void
    let swap: () -> Void
    var returnToCurrent: () -> Void = {}
    /// What was dropped on a side's button (an asset of the list, or a
    /// base's file): true when it took that side.
    var drop: (StageSlot, URL) -> Bool = { _, _ in false }
    @State private var targeted: StageSlot?

    private static let groups: [[CompareMode]] = [
        [.sideBySide, .stacked], [.wipe, .wipeHorizontal],
    ]

    var body: some View {
        if hasB || takesPictures {
            GlassEffectContainer(spacing: 16) {
                HStack(spacing: 16) {
                    if current == nil {
                        TitleBarButton(symbol: "arrow.uturn.backward",
                                       title: "Return to current",
                                       help: "Show the current picture in A again",
                                       action: returnToCurrent)
                            .titleBarCapsule()
                            .transition(.scale(scale: 0.6)
                                .combined(with: .opacity))
                    }
                    HStack(spacing: 2) {
                        sideButton(.a)
                        sideButton(.b)
                    }
                    .titleBarCapsule()
                    if hasB {
                        ForEach(Self.groups, id: \.self) { group in
                            HStack(spacing: 2) {
                                ForEach(group) { modeButton($0) }
                            }
                            .titleBarCapsule()
                        }
                        lineButton
                            .titleBarCapsule()
                        modeButton(.difference)
                            .titleBarCapsule()
                        TitleBarButton(symbol: "arrow.left.arrow.right",
                                       title: "Swap A and B",
                                       help: "Swap A and B", action: swap)
                            .titleBarCapsule()
                    }
                }
            }
            .transition(.opacity.combined(with: .scale(scale: 0.95)))
        }
    }

    private func modeButton(_ m: CompareMode) -> some View {
        TitleBarButton(symbol: m.symbol,
                       title: LocalizedStringKey(m.help),
                       help: LocalizedStringKey(m.help),
                       on: m == mode) {
            setMode(m)
        }
    }

    /// A or B: shows that side -- and takes a picture dropped on it.
    private func sideButton(_ s: StageSlot) -> some View {
        let m: CompareMode = s == .a ? .a : .b
        let empty = s == .b && !hasB
        return TitleBarButton(symbol: m.symbol,
                              title: LocalizedStringKey(m.help),
                              help: s == current
                                ? (s == .a ? "Show A, the current picture"
                                           : "Show B, the current picture")
                                : LocalizedStringKey(m.help),
                              on: m == mode && !empty, text: m.label,
                              emphasized: s == current) {
            if !empty { setMode(m) }
        }
        .opacity(empty ? 0.45 : 1)
        .overlay {
            if targeted == s {
                Circle().strokeBorder(Color.accentColor, lineWidth: 2)
                    .allowsHitTesting(false)
            }
        }
        .dropDestination(for: URL.self) { urls, _ in
            guard let url = urls.first else { return false }
            return drop(s, url)
        } isTargeted: { on in
            if on { targeted = s } else if targeted == s { targeted = nil }
        }
    }

    /// The divider, drawn across the wipe it belongs to.
    private var lineButton: some View {
        let wipes = mode == .wipe || mode == .wipeHorizontal
        return TitleBarButton(
            symbol: mode == .wipeHorizontal
                ? "arrow.up.and.line.horizontal.and.arrow.down"
                : "arrow.left.and.line.vertical.and.arrow.right",
            title: "Wipe line",
            help: wipeLine ? "Hide the wipe line" : "Show the wipe line",
            on: wipes && wipeLine, action: toggleWipeLine)
            .disabled(!wipes)
    }
}
