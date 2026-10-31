import SwiftUI

/// The window's right column, the system's inspector: Layers over
/// Information, each under a header that folds it, the open ones sharing
/// the height -- a divider between them dragged to move it, double-
/// clicked to even it out. What is folded, and the shares, are the
/// window's chrome, kept between launches. (The Agent's panel joins it,
/// switched with the inspector, in a later phase; a layer's edit
/// sections below Information too.)
struct RightColumn: View {
    @Bindable var model: AppModel

    private var sections: [CollapsibleSection<InspectorSection>] {
        InspectorSection.allCases.map {
            CollapsibleSection($0, label: $0.label, symbol: $0.symbol)
        }
    }

    var body: some View {
        CollapsibleStack(
            folded: $model.chrome.collapsedSections,
            weights: $model.chrome.sectionWeights,
            sections: sections) { s in
            switch s {
            case .layers:
                // Its notes while it has the column to itself.
                LayersSection(
                    model: model,
                    notes: model.chrome.collapsedSections.contains(.info))
                    .padding(.top, VZ.Space.xs)
                    .font(.system(size: 11))
                    .controlSize(.small)
            case .info:
                InformationSection(model: model)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
}
