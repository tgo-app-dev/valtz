import SwiftUI

/// A card of the content layer: the composer's, the timeline's. It sits
/// on the window's text background with a hairline edge and the dock's
/// shadow, its corners concentric with the dock around it (the dock sets
/// the container shape). No glass: glass is for what floats over it.
struct DockCard<Content: View>: View {
    @ViewBuilder var content: () -> Content

    var body: some View {
        content()
            .background {
                VZ.Radius.concentric()
                    .fill(Color(nsColor: .textBackgroundColor))
                    .overlay {
                        // Inset by half the line: a stroke straddles the
                        // edge, and the shape has no strokeBorder.
                        VZ.Radius.concentric()
                            .stroke(Color.vzHairline, lineWidth: 1)
                            .padding(0.5)
                    }
                    .vzShadow(VZ.Elevation.dock)
            }
    }
}
