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
/// The dock under the stage -- the timeline band and the composer --
/// keeps its height and whether the band is wanted in the chrome too.

/// The inspector's sections, top first: the stage's layers, what is
/// selected, then the selected layer's edits -- its look (Adjust, Color,
/// Grade), its crop, its trim, its sound.
enum InspectorSection: String, CaseIterable, Identifiable, Codable, Sendable {
    case layers, info, adjust, color, grade, crop, trim, audio
    var id: String { rawValue }

    var label: String {
        switch self {
        case .layers: String(localized: "Layers")
        case .info: String(localized: "Information")
        case .adjust: String(localized: "Adjust")
        case .color: String(localized: "Color")
        case .grade: String(localized: "Grade")
        case .crop: String(localized: "Crop")
        case .trim: String(localized: "Trim")
        case .audio: String(localized: "Audio")
        }
    }

    var symbol: String {
        switch self {
        case .layers: "square.3.layers.3d"
        case .info: "info.circle"
        case .adjust: "slider.horizontal.3"
        case .color: "paintpalette"
        case .grade: "camera.filters"
        case .crop: "crop"
        case .trim: "timeline.selection"
        case .audio: "speaker.wave.2"
        }
    }

    /// A layer's edit, not the stack's or the selection's.
    var isEdit: Bool { self != .layers && self != .info }

    /// The composer's old panel for it, for the hooks that still name
    /// one.
    var panel: ComposerPanel? {
        switch self {
        case .layers, .info: nil
        case .adjust: .adjust
        case .color: .color
        case .grade: .grade
        case .crop: .crop
        case .trim: .trim
        case .audio: .audio
        }
    }
}

extension ComposerPanel {
    /// The inspector section a layer's edit panel is now.
    var section: InspectorSection {
        switch self {
        case .adjust: .adjust
        case .color: .color
        case .grade: .grade
        case .crop: .crop
        case .trim: .trim
        case .audio: .audio
        }
    }
}

/// What the right column shows: the inspector's sections, or the Agent.
enum RightPanel: String, Codable, Sendable {
    case inspector, agent
}

/// What the person has set of the window's regions: whether the sidebar
/// and the inspector are shown, whether the right column is the
/// inspector or the Agent, which inspector sections are folded and how
/// the open ones share the height, the dock's height and whether its
/// timeline band is wanted. Kept between launches (not by a scripted
/// snapshot run).
struct WorkspaceChrome: Codable, Equatable {
    var sidebar: NavigationSplitViewVisibility = .all
    var inspectorShown = false
    var rightPanel: RightPanel = .inspector
    /// The dock's height as its splitter left it; nil, the height its
    /// content asks (a double-click on the splitter goes back to that).
    var dockHeight: CGFloat?
    /// The timeline band over the composer is wanted: shown while the
    /// stage has a timeline (AppModel.timelineOpen).
    var timelineBand = false
    /// A layer's edits but its tone start folded: Adjust opens beside
    /// Layers and Information, the others as they are asked for.
    var collapsedSections: Set<InspectorSection> =
        [.color, .grade, .crop, .trim, .audio]
    var sectionWeights: [InspectorSection: Double] = [:]

    private static let key = "workspace.chrome"

    init() {}

    /// What an older version kept lacks the newer fields: each one
    /// missing is its default, not the whole chrome lost.
    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = WorkspaceChrome()
        sidebar = try c.decodeIfPresent(NavigationSplitViewVisibility.self,
                                        forKey: .sidebar) ?? d.sidebar
        inspectorShown = try c.decodeIfPresent(Bool.self,
                                               forKey: .inspectorShown)
            ?? d.inspectorShown
        rightPanel = (try? c.decodeIfPresent(RightPanel.self,
                                             forKey: .rightPanel))
            .flatMap { $0 } ?? d.rightPanel
        collapsedSections = (try? c.decodeIfPresent(
            Set<InspectorSection>.self, forKey: .collapsedSections))
            .flatMap { $0 } ?? d.collapsedSections
        sectionWeights = (try? c.decodeIfPresent(
            [InspectorSection: Double].self, forKey: .sectionWeights))
            .flatMap { $0 } ?? d.sectionWeights
        dockHeight = (try? c.decodeIfPresent(CGFloat.self,
                                             forKey: .dockHeight))
            .flatMap { $0 } ?? d.dockHeight
        timelineBand = (try? c.decodeIfPresent(Bool.self,
                                               forKey: .timelineBand))
            .flatMap { $0 } ?? d.timelineBand
    }

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
    /// The dock under the stage: none, the composer, the timeline band
    /// over it -- or, in the Prompt Editor, the prompt expanded (until
    /// Phase 5 makes that the dock grown, the editor lays it out itself
    /// and the dock stands aside).
    enum Dock: Equatable { case hidden, composer, bandAndComposer,
                                expandedPrompt }

    var sidebar: NavigationSplitViewVisibility
    var inspector: Bool
    var context: Context
    var stage: Stage
    var dock: Dock
    /// The toolbar's editor items -- zoom, compare, markup, share, the
    /// inspector -- belong on the editor; the Atlas and the Log hide them.
    var editorTools: Bool

    /// The mode table:
    ///
    ///   mode            sidebar      inspector  context  stage   dock
    ///   compose         chrome       chrome     none     editor  composer *
    ///   Prompt Editor   chrome       chrome     none     editor  expandedPrompt
    ///   markup          chrome       chrome     markup   editor  as compose
    ///   view only       detailOnly   off        none     editor  hidden
    ///   Atlas, Log      chrome       off        none     atlas,  hidden
    ///                                                    log
    ///   too narrow      detailOnly   chrome     --       --      --
    ///
    ///   * bandAndComposer while a timeline is on the stage and the band
    ///     is wanted (chrome.timelineBand: AppModel.timelineOpen).
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
        let dock: Dock = !editor || m.viewOnly ? .hidden
            : m.promptImmersive ? .expandedPrompt
            : m.timelineOpen ? .bandAndComposer : .composer
        return WorkspaceLayout(
            sidebar: inspector && !m.columnsFit ? .detailOnly
                                                : m.chrome.sidebar,
            inspector: inspector,
            context: editor && m.markupOpen && !m.viewOnly
                && !m.promptImmersive ? .markup : .none,
            stage: stage,
            dock: dock,
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

    /// An inspector section brought into sight -- the Adjust, Crop, Trim
    /// ... asked for from a menu, a notice, a task: the inspector shown,
    /// the right column on the inspector, the section unfolded.
    func revealInspectorSection(_ s: InspectorSection) {
        withAnimation(Self.motion) {
            chrome.inspectorShown = true
            chrome.rightPanel = .inspector
            chrome.collapsedSections.remove(s)
        }
        switch s {
        // Crop shows the whole canvas.
        case .crop: stage.fitRequest += 1
        // Trim's ← and → step the clip: the prompt gives up the keyboard
        // (its caret took them; a click in it gets them back).
        case .trim:
            if let w = editorWindow, w.firstResponder is NSText {
                w.makeFirstResponder(nil)
            }
        default: break
        }
    }

    /// A section folded if it shows, else brought into sight (a hook's
    /// "panel" step, as the tray's chevrons toggled).
    func toggleInspectorSection(_ s: InspectorSection) {
        if sectionShown(s) {
            withAnimation(Self.motion) {
                _ = chrome.collapsedSections.insert(s)
            }
        } else {
            revealInspectorSection(s)
        }
    }

    /// A hook's panel by name: a layer's edit ("adjust" ... "audio")
    /// brought into sight in the inspector; a chip's popover ("shape",
    /// "favor" ...; "generate", the Generate drawer that was, Model's)
    /// opened; nil, the chip open shut.
    func openPanelNamed(_ name: String?) {
        guard let name else {
            openChip = nil
            return
        }
        if let p = ComposerPanel.allCases.first(where: { "\($0)" == name }) {
            revealInspectorSection(p.section)
        } else {
            openChip = GenerateChip(rawValue: name) ?? .model
        }
    }

    /// The right column on the Agent, shown: a new session, ⌘4.
    func showAgentPanel() {
        withAnimation(Self.motion) {
            chrome.inspectorShown = true
            chrome.rightPanel = .agent
        }
    }

    /// Whether the inspector has the section at all now -- the rest are
    /// left out, never greyed. Layers with a stack on the stage;
    /// Information always; a layer's edits while the stage has something
    /// for them, and not while an agent at work holds the project or the
    /// Prompt Editor has the window (Layers and Information alone).
    func sectionApplies(_ s: InspectorSection) -> Bool {
        let edits = !agentHolds && !promptImmersive
        switch s {
        case .layers: return stageStack != nil
        case .info: return true
        case .adjust: return edits && showsAdjust
        case .color, .grade: return edits && showsAdjust && !stageIsAudio
        case .crop: return edits && showsCrop
        case .trim: return edits && showsTrim
        case .audio: return edits && showsAudio
        }
    }

    /// The sections the inspector lists now, top first.
    var inspectorSections: [InspectorSection] {
        InspectorSection.allCases.filter(sectionApplies)
    }

    /// The section is in sight: the inspector shown and on its sections,
    /// the section there and open. (The Color section's target tool works
    /// only while it is.)
    func sectionShown(_ s: InspectorSection) -> Bool {
        layout.inspector && chrome.rightPanel == .inspector
            && !chrome.collapsedSections.contains(s) && sectionApplies(s)
    }

    /// For the snapshot hooks' summaries: a chip's popover open
    /// ("chip-shape"), or the edit sections in sight ("adjust+crop"), or
    /// "-".
    var panelSummary: String {
        if let c = openChip { return "chip-" + c.rawValue }
        let shown = InspectorSection.allCases.filter {
            $0.isEdit && sectionShown($0)
        }
        return shown.isEmpty ? "-"
            : shown.map(\.rawValue).joined(separator: "+")
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
