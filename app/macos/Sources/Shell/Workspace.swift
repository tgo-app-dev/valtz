import SwiftUI

/// The window's skeleton is fixed -- a sidebar, the stage column, the
/// inspector -- and a mode only decides what fills each region and which
/// regions fold away. Two small types carry that: `WorkspaceChrome`, what
/// the person has set and is kept between launches, and
/// `WorkspaceLayout`, what the window shows now, worked out from the
/// chrome and the model's modes by `resolve(_:)` on every pass. The
/// collapse rules live in `resolve` and nowhere else; `AppModel` stays
/// the source of truth for the modes themselves (view only, the Prompt
/// Editor, markup, the screen the sidebar picked).
///
/// Later phases add to the chrome what their regions need: the right
/// column's Agent panel, the dock's height and the timeline band.

/// The inspector's sections, top first.
enum InspectorSection: String, CaseIterable, Identifiable, Codable, Sendable {
    case layers, info
    var id: String { rawValue }

    var label: String {
        switch self {
        case .layers: String(localized: "Layers")
        case .info: String(localized: "Information")
        }
    }

    var symbol: String {
        switch self {
        case .layers: "square.3.layers.3d"
        case .info: "info.circle"
        }
    }
}

/// What the person has set of the window's regions: whether the sidebar
/// and the inspector are shown, and which inspector sections are folded
/// and how the open ones share the height. Kept between launches (not
/// by a scripted snapshot run).
struct WorkspaceChrome: Codable, Equatable {
    var sidebar: NavigationSplitViewVisibility = .all
    var inspectorShown = false
    var collapsedSections: Set<InspectorSection> = []
    var sectionWeights: [InspectorSection: Double] = [:]

    private static let key = "workspace.chrome"

    static func stored() -> WorkspaceChrome {
        guard AppModel.keepsChrome,
              let data = UserDefaults.standard.data(forKey: key),
              let c = try? JSONDecoder().decode(WorkspaceChrome.self,
                                                from: data)
        else { return WorkspaceChrome() }
        return c
    }

    func store() {
        guard AppModel.keepsChrome,
              let data = try? JSONEncoder().encode(self) else { return }
        UserDefaults.standard.set(data, forKey: Self.key)
    }
}

/// What the window shows this pass: derived, never stored.
struct WorkspaceLayout: Equatable {
    /// The stage column's main region.
    enum Stage: Equatable { case editor, atlas, log }
    /// The contextual row above the stage. Compare joins it in Phase 5;
    /// until then its bar stays under the stage.
    enum Context: Equatable { case none, markup }

    var sidebar: NavigationSplitViewVisibility
    var inspector: Bool
    var context: Context
    var stage: Stage
    /// The toolbar's editor items -- zoom, compare, markup, share, the
    /// inspector -- belong on the editor; the Atlas and the Log hide them.
    var editorTools: Bool

    /// The mode table:
    ///
    ///   mode            sidebar      inspector  context  stage
    ///   compose         chrome       chrome     none     editor
    ///   Prompt Editor   chrome       chrome     none     editor
    ///   markup          chrome       chrome     markup   editor
    ///   view only       detailOnly   off        none     editor
    ///   Atlas, Log      chrome       off        none     atlas, log
    ///   too narrow      detailOnly   chrome     --       --
    ///
    /// View only puts the chrome itself away as it starts and back as it
    /// ends (AppModel.setViewOnly), so the person's sidebar toggle still
    /// works meanwhile; the table's `detailOnly` is that.
    ///
    /// Too narrow: the window has no room for the sidebar, the inspector
    /// and the stage's least width (StageColumn.minWidth) side by side --
    /// the split view would squeeze the stage under it -- so the sidebar
    /// folds while the inspector is shown, and comes back as the window
    /// widens. The chrome is left as it was; showing the sidebar then
    /// hides the inspector instead (AppModel.setSidebar).
    @MainActor
    static func resolve(_ m: AppModel) -> WorkspaceLayout {
        let stage: Stage = switch m.screen {
        case .editor: .editor
        case .atlas: .atlas
        case .log: .log
        }
        let editor = stage == .editor
        let inspector = editor && !m.viewOnly && m.chrome.inspectorShown
        return WorkspaceLayout(
            sidebar: inspector && !m.columnsFit ? .detailOnly
                                                : m.chrome.sidebar,
            inspector: inspector,
            context: editor && m.markupOpen && !m.viewOnly
                && !m.promptImmersive ? .markup : .none,
            stage: stage,
            editorTools: editor)
    }
}

/// A row of the sidebar's list that it selects: the screens, and a
/// recent project (opened as it is picked, the selection staying where
/// it was).
enum SidebarItem: Hashable {
    case stage, atlas, log
    case recent(String)
}

extension AppModel {
    /// The chrome is remembered between launches, but not by a scripted
    /// snapshot run.
    nonisolated static var keepsChrome: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
    }

    /// What the window shows now.
    var layout: WorkspaceLayout { .resolve(self) }

    /// The window's width now: whether the sidebar and the inspector, at
    /// the widths they were last given, leave the stage its least width
    /// (`columnsFit`). Their widths are remembered while they are folded,
    /// so the answer never depends on the folding it decides.
    func windowWidthChanged(_ w: CGFloat) {
        let fits = w - sidebarWidth - inspectorWidth >= StageColumn.minWidth
        if fits != columnsFit { columnsFit = fits }
    }

    /// The sidebar shown or hidden by the person (its toolbar button,
    /// ⌃⌘S). Shown where the window has no room for it beside the
    /// inspector: the inspector goes instead.
    func setSidebar(_ v: NavigationSplitViewVisibility) {
        if v != .detailOnly && !columnsFit && layout.inspector {
            chrome.inspectorShown = false
        }
        chrome.sidebar = v
    }

    /// The inspector shown or hidden: the toolbar's button, ⌃⌘I, the
    /// snapshot hooks. Kept as the name everything called it by.
    var inspectorOpen: Bool {
        get { chrome.inspectorShown }
        set { chrome.inspectorShown = newValue }
    }

    /// The screen the sidebar shows: the stage (the editor), the Atlas,
    /// the Log.
    var screen: AppScreen {
        switch sidebarSelection {
        case .atlas: .atlas
        case .log: .log
        case .stage, .recent: .editor
        }
    }

    /// A sidebar row picked: its screen shown, or its project opened.
    func select(_ item: SidebarItem) {
        switch item {
        case .stage: show(.editor)
        case .atlas: show(.atlas)
        case .log: show(.log)
        case .recent(let path): openDropped(URL(fileURLWithPath: path))
        }
    }

    /// An inspector section brought into sight: the inspector shown, the
    /// section unfolded.
    func revealInspectorSection(_ s: InspectorSection) {
        withAnimation(Self.motion) {
            chrome.inspectorShown = true
            chrome.collapsedSections.remove(s)
        }
    }

    /// A column come or gone: a stage that was fitting fits its new room
    /// once the column's slide has settled (it follows the room as it
    /// changes too; this is the last word, at the size it ends at). A
    /// zoom held by hand stays as it was.
    func refitStage() {
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(350))
            guard let self, stage.fitting else { return }
            stage.fitRequest += 1
        }
    }

    /// A section's share of the inspector's height.
    func sectionWeight(_ s: InspectorSection) -> Double {
        max(0.01, chrome.sectionWeights[s] ?? 1)
    }
}
