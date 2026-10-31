import SwiftUI

/// A button floating over the stage -- a clip's Loop and Grab Frame, a
/// still's pages, the compare's A and B and its modes, the Atlas's
/// player: a symbol that highlights on hover and, `on`, shows pressed in
/// (a mode chosen, a side shown). Grouped on the stage's glass by
/// `stageCapsule()`. The window's own toolbar uses the system's buttons.
struct StageButton: View {
    let symbol: String
    let title: LocalizedStringKey
    let help: LocalizedStringKey
    var on = false
    /// A short text in place of the symbol ("A", "B").
    var text: String?
    /// The text bold and underlined: the compare side showing the
    /// current state.
    var emphasized = false
    let action: () -> Void
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        Button(action: action) {
            label
                .frame(width: VZ.Size.toolbarButton,
                       height: VZ.Size.toolbarButton)
                .background {
                    Circle().fill(Color.primary.opacity(
                        on ? 0.14 : hovering && enabled ? 0.06 : 0))
                }
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(enabled ? .primary : .tertiary)
        .accessibilityLabel(Text(title))
        .accessibilityValue(emphasized ? Text("Current") : Text(verbatim: ""))
        .accessibilityAddTraits(on ? .isSelected : [])
        .help(help)
        .onHover { hovering = $0 }
        .animation(VZ.Motion.quick, value: hovering)
        .animation(VZ.Motion.quick, value: on)
    }

    @ViewBuilder
    private var label: some View {
        if let text {
            Text(verbatim: text)
                .font(.system(size: 14,
                              weight: emphasized ? .heavy : .regular))
                .underline(emphasized)
        } else {
            Image(systemName: symbol).font(.system(size: VZ.Size.toolbarSymbol))
        }
    }
}

extension View {
    /// A group of stage buttons on one capsule of the stage's glass.
    func stageCapsule() -> some View {
        padding(.horizontal, VZ.Space.xs)
            .frame(height: 36)
            .vzStageOverlay()
    }
}
