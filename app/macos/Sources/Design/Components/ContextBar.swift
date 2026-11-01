import SwiftUI

/// The one contextual row above the stage: the markup toolbar's tools,
/// or the compare's A/B controls (markup's when both are on, the
/// compare's folded into a menu at its end). One height, one
/// background (the bar's material), a hairline under it; what fills it is
/// the mode's, centred, at the small control size -- scrolling sideways
/// where the column is too narrow for it, never widening the window.
struct ContextBar<Content: View, Trailing: View>: View {
    @ViewBuilder var content: () -> Content
    /// Pinned at the row's end, out of its scroll: the compare's menu on
    /// markup's row.
    @ViewBuilder var trailing: () -> Trailing
    /// The tools' room: what they are centred in.
    @State private var width: CGFloat = 0

    var body: some View {
        HStack(spacing: 0) {
            // Wider than the column -- markup's tools at 820 points --
            // the tools scroll sideways rather than widen the window.
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: VZ.Space.s) {
                    content()
                }
                .padding(.horizontal, VZ.Space.m)
                .frame(minWidth: width, maxHeight: .infinity)
            }
            .scrollBounceBehavior(.basedOnSize)
            .onGeometryChange(for: CGFloat.self) { $0.size.width } action: {
                width = $0
            }
            trailing()
        }
        .controlSize(.small)
        .frame(height: VZ.Size.contextBar)
        .frame(maxWidth: .infinity)
        .background(.bar)
        .overlay(alignment: .bottom) { Divider() }
    }
}

extension ContextBar where Trailing == EmptyView {
    init(@ViewBuilder content: @escaping () -> Content) {
        self.content = content
        self.trailing = { EmptyView() }
    }
}

/// A group of the row's buttons in one bordered box: the compare's sides,
/// its layouts, its wipes.
struct ContextGroup<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: VZ.Radius.control,
                                     style: .continuous)
        HStack(spacing: 1) { content() }
            .padding(1)
            .background(shape.fill(Color.primary.opacity(0.035)))
            .overlay(shape.strokeBorder(Color.primary.opacity(0.14),
                                        lineWidth: 0.5))
    }
}

/// One of the row's buttons, as markup's tools are: a symbol -- or a
/// short text ("A", "B") -- pressed in with the accent while `on`.
struct ContextButton: View {
    let symbol: String
    let title: LocalizedStringKey
    let help: LocalizedStringKey
    var on = false
    /// A short text in place of the symbol.
    var text: String?
    /// The text bold and underlined: the compare side showing the
    /// current state.
    var emphasized = false
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Group {
                if let text {
                    Text(verbatim: text)
                        .fontWeight(emphasized ? .heavy : .regular)
                        .underline(emphasized)
                } else {
                    Image(systemName: symbol)
                }
            }
            .frame(width: 26, height: 22)
            .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .background(RoundedRectangle(cornerRadius: 5, style: .continuous)
            .fill(on ? Color.accentColor.opacity(0.22) : .clear))
        .foregroundStyle(on ? Color.accentColor : Color.primary)
        .help(help)
        .accessibilityLabel(Text(title))
        .accessibilityValue(emphasized ? Text("Current") : Text(verbatim: ""))
        .accessibilityAddTraits(on ? .isSelected : [])
    }
}
