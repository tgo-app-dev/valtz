import SwiftUI

/// The middle column: the contextual row, when a mode has one (markup's
/// tools, the compare's A, B and modes), over the stage region -- the
/// editor's stage (SimpleView), the Atlas or the Log -- and, under a
/// splitter, the dock: the timeline band and the composer. The region
/// takes whatever room the sidebar, the inspector and the dock leave it,
/// never less than a stage's worth, and its background runs on under the
/// sidebar and the inspector's glass. The Atlas, the Log and view only
/// have no dock.
///
/// The Prompt Editor is the dock grown: the composer the column's height
/// but a strip at the top, where the stage goes small. It is the same
/// dock, the same card, the same text view -- grown, not made again -- so
/// the prompt keeps the keyboard, the caret and its words as it grows and
/// as it goes back. The splitter rests meanwhile; back, the dock is the
/// height it had.
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

    /// The stage's strip over the Prompt Editor: the small stage and its
    /// transport (SimpleView), or a generation's progress.
    static let stageStrip: CGFloat = 168

    private var docked: Bool { layout.dock != .hidden }
    private var expanded: Bool { layout.dock == .expandedPrompt }

    /// The dock's height this pass: the drag's, else the chrome's, held
    /// to its bounds in the column as it is now (a window made shorter
    /// leaves the stage its least); nil, its content's. Grown, it has none
    /// of its own: the strip is the region's height and the dock takes
    /// the rest, so a window made smaller is never held up by a height
    /// worked out from the size it was.
    private var dockHeight: CGFloat? {
        if expanded { return nil }
        guard let h = dragHeight ?? model.chrome.dockHeight else {
            return nil
        }
        guard columnHeight > 0 else { return h }
        let b = DockView.bounds(column: columnHeight)
        return min(max(h, b.lowerBound), b.upperBound)
    }

    var body: some View {
        VStack(spacing: 0) {
            contextRow
            VStack(spacing: 0) {
                region
                    .frame(height: expanded ? Self.stageStrip : nil)
                    .frame(maxWidth: .infinity,
                           maxHeight: expanded ? nil : .infinity)
                    .background(Color(nsColor: .windowBackgroundColor))
                    .backgroundExtensionEffect()
                if docked {
                    VStack(spacing: 0) {
                        // Resting while the dock is grown: its room, no
                        // grip, nothing to drag.
                        if expanded {
                            Color.clear.frame(height: DockSplitter.height)
                        } else {
                            splitter
                        }
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
        .animation(AppModel.motion, value: expanded)
    }

    /// The mode's row: markup's tools (the compare's folded into a menu
    /// at its end, when it is on too), or the compare's. Where it lies,
    /// in the window: a click on it leaves a viewing as it is, as one on
    /// the stage does.
    @ViewBuilder
    private var contextRow: some View {
        Group {
            switch layout.context {
            case .markup:
                MarkupBar(model: model)
                    .transition(.move(edge: .top).combined(with: .opacity))
            case .compare:
                CompareBar(model: model)
                    .transition(.move(edge: .top).combined(with: .opacity))
            case .none:
                EmptyView()
            }
        }
        .onGeometryChange(for: CGRect.self) {
            $0.frame(in: .global)
        } action: { model.contextBarFrame = $0 }
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
            SimpleView(model: model, strip: expanded)
        case .atlas:
            AtlasView(model: model)
        case .log:
            LogView(model: model)
        }
    }
}
