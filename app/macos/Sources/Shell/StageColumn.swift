import SwiftUI

/// The middle column: the contextual row, when a mode has one (markup's
/// tools), over the stage region -- the editor (the stage and, until the
/// dock comes, the composer under it: SimpleView), the Atlas or the Log.
/// The region takes whatever room the sidebar and the inspector leave
/// it, never less than a stage's worth, and its background runs on under
/// the sidebar and the inspector's glass.
struct StageColumn: View {
    @Bindable var model: AppModel
    let layout: WorkspaceLayout

    /// The least the stage region is given: the sidebar and the inspector
    /// give way to their own least widths first, and the window's least
    /// width (820) holds all three.
    static let minWidth: CGFloat = 320

    var body: some View {
        VStack(spacing: 0) {
            if layout.context == .markup {
                MarkupBar(model: model)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            region
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                // The composer under the stage is wider than a narrow
                // column until the dock replaces it: cut at the column's
                // edges, never drawn under the sidebar or the inspector.
                .clipped()
                .background(Color(nsColor: .windowBackgroundColor))
                .backgroundExtensionEffect()
        }
        .animation(AppModel.motion, value: layout.context)
    }

    @ViewBuilder
    private var region: some View {
        switch layout.stage {
        case .editor:
            VStack(spacing: 0) {
                // The Prompt Editor's own toolbar: the editor grows the
                // prompt card itself (SimpleView).
                if model.promptImmersive {
                    PromptBar(model: model)
                        // Its foot: the small stage keeps clear of it.
                        .onGeometryChange(for: CGFloat.self) {
                            $0.frame(in: .global).maxY
                        } action: { model.editorToolbarBottom = $0 }
                        .transition(.move(edge: .top)
                            .combined(with: .opacity))
                }
                SimpleView(model: model)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        case .atlas:
            AtlasView(model: model)
        case .log:
            LogView(model: model)
        }
    }
}
