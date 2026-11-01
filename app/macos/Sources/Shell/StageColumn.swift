import SwiftUI

/// The middle column: the contextual row, when a mode has one (markup's
/// tools), over the stage region -- the editor's stage (SimpleView), the
/// Atlas or the Log -- and, under a splitter, the dock: the timeline band
/// and the composer. The region takes whatever room the sidebar, the
/// inspector and the dock leave it, never less than a stage's worth, and
/// its background runs on under the sidebar and the inspector's glass.
/// The Atlas, the Log and view only have no dock; the Prompt Editor lays
/// out its own composer until Phase 5 makes it the dock grown.
struct StageColumn: View {
    @Bindable var model: AppModel
    let layout: WorkspaceLayout
    /// The region's and the dock's height together.
    @State private var columnHeight: CGFloat = 0
    /// The dock's height as laid out: where a drag of the splitter starts.
    @State private var dockShown: CGFloat = 0
    /// A drag of the splitter under way: where it started, and the height
    /// it has set -- the chrome takes it as the drag ends.
    @State private var dragStart: CGFloat?
    @State private var dragHeight: CGFloat?

    /// The least the stage region is given: the sidebar and the inspector
    /// give way to their own least widths first, and the window's least
    /// width (820) holds all three.
    static let minWidth: CGFloat = 320

    private var docked: Bool {
        layout.dock == .composer || layout.dock == .bandAndComposer
    }

    /// The dock's height this pass: the drag's, else the chrome's, held
    /// to its bounds in the column as it is now (a window made shorter
    /// leaves the stage its least); nil, its content's.
    private var dockHeight: CGFloat? {
        guard let h = dragHeight ?? model.chrome.dockHeight else {
            return nil
        }
        guard columnHeight > 0 else { return h }
        let b = DockView.bounds(column: columnHeight)
        return min(max(h, b.lowerBound), b.upperBound)
    }

    var body: some View {
        VStack(spacing: 0) {
            if layout.context == .markup {
                MarkupBar(model: model)
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            VStack(spacing: 0) {
                region
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(Color(nsColor: .windowBackgroundColor))
                    .backgroundExtensionEffect()
                if docked {
                    VStack(spacing: 0) {
                        splitter
                        DockView(model: model, layout: layout,
                                 height: dockHeight,
                                 columnHeight: columnHeight)
                            .onGeometryChange(for: CGFloat.self) {
                                $0.size.height
                            } action: { dockShown = $0 }
                    }
                    .background(Color(nsColor: .windowBackgroundColor))
                    .transition(.move(edge: .bottom)
                        .combined(with: .opacity))
                }
            }
            .onGeometryChange(for: CGFloat.self) { $0.size.height } action: {
                columnHeight = $0
                model.stageColumnHeight = $0
            }
        }
        .animation(AppModel.motion, value: layout.context)
        .animation(AppModel.motion, value: docked)
    }

    private var splitter: some View {
        let h = dockHeight ?? dockShown
        return DockSplitter(
            label: String(localized: "Dock"),
            value: String(localized: "\(Int(h.rounded())) points"),
            changed: { dy in
                let start = dragStart ?? dockShown
                dragStart = start
                // Downward shrinks the dock.
                dragHeight = start - dy
            },
            ended: {
                if let h = dockHeight { model.chrome.dockHeight = h }
                dragStart = nil
                dragHeight = nil
            },
            reset: {
                dragStart = nil
                dragHeight = nil
                withAnimation(AppModel.motion) {
                    model.chrome.dockHeight = nil
                }
            })
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
