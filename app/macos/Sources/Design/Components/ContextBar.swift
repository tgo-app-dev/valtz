import SwiftUI

/// The one contextual row above the stage: the markup toolbar's tools,
/// or the compare's A/B controls, whichever mode is on. One height, one
/// background (the bar's material), a hairline under it; what fills it is
/// the mode's, centred, at the small control size.
struct ContextBar<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        HStack(spacing: VZ.Space.s) {
            Spacer(minLength: 0)
            content()
            Spacer(minLength: 0)
        }
        .controlSize(.small)
        .padding(.horizontal, VZ.Space.m)
        .frame(height: VZ.Size.contextBar)
        .frame(maxWidth: .infinity)
        .background(.bar)
        .overlay(alignment: .bottom) { Divider() }
    }
}
