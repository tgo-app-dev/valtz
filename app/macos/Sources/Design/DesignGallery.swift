import SwiftUI

#if DEBUG
/// Every component of the design system on one page, in the current
/// appearance (Help › Design Gallery, debug builds): a place to see a
/// change to a token on everything it touches.
struct DesignGallery: View {
    @State private var chipOpen = false
    @State private var folded: Set<String> = []
    @State private var weights: [String: Double] = [:]

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: VZ.Space.xl) {
                // The gallery's own copy is sample data, not UI text:
                // verbatim, out of the catalog. The components' labels
                // go through it, as they do in the app.
                Text(verbatim: "Design Gallery").font(.largeTitle)

                group("Type") {
                    Text(verbatim: "Heading").font(.vzHeading)
                    Text(verbatim: "Body: a prompt's words, a note.").font(.vzBody)
                    Text(verbatim: "Callout: a chip, a tab.").font(.vzCallout)
                    Text(verbatim: "Label: a row, a field.").font(.vzLabel)
                    Text(verbatim: "Meta: when, how big, how long.").font(.vzMeta)
                    Text(verbatim: "Caption: a figure, a hint.").font(.vzCaption)
                }

                group("Activity") {
                    HStack(spacing: VZ.Space.l) {
                        ActivityRing(fraction: 0.3)
                        ActivityRing(fraction: nil)
                        ActivityRing(fraction: nil, running: false)
                        ActivityRing(fraction: nil, idle: true)
                    }
                    Meter(name: "GPU", value: 0.62) {
                        FixedFigure(text: "62.0%", widest: "100.0%")
                    }
                    Meter(name: "RAM", value: 0.3, behind: 0.7) {
                        FixedFigure(text: "7.2 GB", widest: "99.99 GB")
                    }
                }

                group("Chips") {
                    HStack {
                        ChipMenu(label: String(localized: "Model"), value: "Auto (Krea 2 Turbo)",
                                 isOpen: $chipOpen) {
                            Text(verbatim: "A popover").padding()
                        }
                        ChipMenu(label: String(localized: "Shape"), value: "16:9 · Large",
                                 systemImage: "rectangle",
                                 isOpen: .constant(false)) { EmptyView() }
                    }
                }

                group("Context bar") {
                    ContextBar {
                        Button("Select", systemImage: "arrow.up.left") {}
                        Button("Brush", systemImage: "paintbrush.pointed") {}
                        Divider().frame(height: 16)
                        Toggle(isOn: .constant(true)) {
                            Label { Text(verbatim: "Snap") }
                                icon: { Image(systemName: "magnet") }
                        }
                    }
                }

                group("Dock card") {
                    DockCard {
                        Text(verbatim: "A card of the content layer.")
                            .padding(VZ.Space.l)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .containerShape(.rect(cornerRadius: VZ.Radius.dock))
                    .padding(VZ.Space.s)
                }

                group("Sections") {
                    CollapsibleStack(
                        folded: $folded, weights: $weights,
                        sections: [
                            CollapsibleSection("a", label: String(localized: "Layers"),
                                               symbol: "square.3.layers.3d"),
                            CollapsibleSection("b", label: String(localized: "Information"),
                                               symbol: "info.circle"),
                        ]) { id in
                        Text(verbatim: "Section \(id)")
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                    .frame(height: 220)
                    .background(Color(nsColor: .controlBackgroundColor))
                }

                group("Empty states") {
                    HStack(alignment: .top) {
                        EmptyState.NoSelection()
                        EmptyState.NoActivity()
                    }
                    .frame(height: 160)
                }
            }
            .padding(VZ.Space.xxl)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(minWidth: 640, minHeight: 480)
    }

    private func group<C: View>(_ title: String,
                                @ViewBuilder _ c: () -> C) -> some View {
        VStack(alignment: .leading, spacing: VZ.Space.s) {
            Text(verbatim: title).font(.vzLabel).foregroundStyle(.secondary)
            c()
        }
    }
}
#endif
