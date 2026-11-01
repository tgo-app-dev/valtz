import SwiftUI
import UniformTypeIdentifiers

/// The window: one skeleton, three columns -- the sidebar (the project,
/// its screens, Recents, Assets), the stage column, the inspector (Layers
/// over Information) -- under the system's toolbar. A mode only decides
/// what fills each region and which fold away (WorkspaceLayout.resolve);
/// the window never resizes itself for any of them.
///
/// The stage over-sized in the system's inspector once: it was laid out
/// from a column sized to the composer's fixed width, the window widened
/// for the inspector, and the window's least width followed it. Now the
/// stage region takes what it is given, the window's least width is
/// fixed, and the stage fits again once a column has come or gone.
struct RootView: View {
    @Bindable var model: AppModel
    /// A drag over the window, and whether it holds a Valtz project
    /// (ProjectDropDelegate).
    @State private var projectDrag = ProjectDrag()

    /// The window's least size, fixed: it never resizes itself for a
    /// column, and at it the sidebar or the inspector folds before the
    /// stage goes under its least width (WorkspaceLayout.resolve).
    static let minSize = NSSize(width: 820, height: 600)

    var body: some View {
        let layout = model.layout
        NavigationSplitView(columnVisibility: Binding(
            get: { layout.sidebar },
            // The split view says back what it was told, a fold made
            // for room included: only a change is the person's.
            set: { if $0 != model.layout.sidebar { model.setSidebar($0) } })) {
            SidebarView(model: model)
                .navigationSplitViewColumnWidth(min: 200, ideal: 240,
                                                max: 320)
                // The split view's own toggle lives in the sidebar's part
                // of the toolbar and was lost once the column folded: the
                // editor toolbar's Sidebar button stands in for it.
                .toolbar(removing: .sidebarToggle)
                .onGeometryChange(for: CGFloat.self) { $0.size.width } action: {
                    if $0 > 0 { model.sidebarWidth = $0 }
                }
        } detail: {
            StageColumn(model: model, layout: layout)
                // Only a change is the person's: the inspector says back
                // its hiding on the Atlas, the Log and under view only.
                .inspector(isPresented: Binding(
                    get: { layout.inspector },
                    set: {
                        if $0 != model.layout.inspector {
                            model.chrome.inspectorShown = $0
                        }
                    })) {
                    RightColumn(model: model)
                        .inspectorColumnWidth(min: 280, ideal: 320, max: 480)
                        .onGeometryChange(for: CGFloat.self) {
                            $0.size.width
                        } action: {
                            if $0 > 0 { model.inspectorWidth = $0 }
                        }
                }
                .toolbar(id: "editor") {
                    EditorToolbar(model: model, layout: layout)
                }
                .toolbarRole(.editor)
                .background { WindowTitle(model: model) }
        }
        // The window's width: room for both columns beside the stage, or
        // the sidebar folds while the inspector shows.
        .onGeometryChange(for: CGFloat.self) { $0.size.width } action: {
            model.windowWidthChanged($0)
        }
        // A column come or gone: the stage fits its new room once the
        // columns have settled.
        .onChange(of: layout.sidebar) { _, _ in model.refitStage() }
        .onChange(of: layout.inspector) { _, _ in model.refitStage() }
        // File › Start Over…, with something to lose: asked first.
        .confirmationDialog("Start over?", isPresented: $model.startOverAsked) {
            Button("Start Over", role: .destructive) { model.startOver() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The prompt, the result and the settings are cleared, and this session's images are deleted from disk. Save anything you want to keep first.")
        }
        // A project dropped anywhere no panel takes a drop of its own:
        // opened, the one open asked about first; other media -- files,
        // a browser's picture, an app's bytes, a link: into Assets.
        .onDrop(of: DroppedMedia.dropTypes,
                delegate: ProjectDropDelegate(model: model,
                                              drag: $projectDrag))
        .overlay {
            if projectDrag.showsProject {
                ProjectDropMark()
                    .transition(.opacity)
                    .allowsHitTesting(false)
            }
        }
        .animation(.easeInOut(duration: 0.15),
                   value: projectDrag.showsProject)
        // A fleet job running here (DESIGN §11): the window given over to
        // it -- its controls out of reach, the mark in blue, the job's
        // progress.
        .disabled(model.windowGivenOver)
        .overlay {
            if let s = model.fleetServing {
                FleetServingOverlay(serving: s)
                    .transition(.opacity)
            }
        }
        // A closed project's tasks being stopped -- a quit, another project
        // -- the window given over to the wait, saying what it waits for.
        .overlay {
            if let c = model.closingTasks {
                ClosingTasksOverlay(closing: c)
                    .transition(.opacity)
            }
        }
        .fleetAskAlert(model)
        // Unsaved changes: the dot in the window's close button.
        .onChange(of: model.document.dirty, initial: true) { _, dirty in
            NSApp.windows.first { $0.isVisible }?.isDocumentEdited =
                dirty && !model.isAnonymous
        }
        // The markup toolbar put away, or another picture on the stage:
        // what it had selected is let go -- not when the stage now shows
        // the picture it is on (the edited copy the first mark on a flat
        // picture makes: its new text lost the selection, and the focus).
        .onChange(of: model.markupOpen) { _, open in
            if !open { model.commitSelection() }
        }
        .onChange(of: model.stagePicture?.id) { _, id in
            if model.markup.selectionAsset != id { model.commitSelection() }
        }
        // The window, for a project's size (AppModel.editorWindow).
        .background(WindowReader {
            $0.contentMinSize = RootView.minSize
            model.editorWindow = $0
        })
    }
}

/// The window's title, its subtitle and its document, set from a view of
/// their own: whether the title can be renamed changes which modifier
/// sets it, and that change remakes this empty view, not the stage.
/// The anonymous session's name is renamed in the title (its menu's
/// Rename, as a document's); a project's name is its file's. "Edited"
/// under it while a project has changes not saved; the proxy icon is the
/// project's package, dragged or ⌘-clicked as any document's.
private struct WindowTitle: View {
    @Bindable var model: AppModel

    /// An agent at work holds the project: not renamed meanwhile.
    private var renamable: Bool {
        model.isAnonymous && !model.viewOnly && !model.agentHolds
    }

    var body: some View {
        Group {
            if renamable {
                Color.clear
                    .navigationTitle(Binding(
                        get: { model.anonymousName },
                        set: { model.renameSession($0) }))
            } else {
                Color.clear
                    .navigationTitle(Text(verbatim: model.windowTitle))
            }
        }
        .navigationSubtitle(model.document.dirty && !model.isAnonymous
                            ? Text("Edited") : Text(verbatim: ""))
        .modifier(ProjectDocument(path: model.isAnonymous ? ""
                                        : model.projectPath))
    }
}

/// The project's package as the window's document, when there is one.
private struct ProjectDocument: ViewModifier {
    let path: String

    func body(content: Content) -> some View {
        if path.isEmpty {
            content
        } else {
            content.navigationDocument(URL(fileURLWithPath: path))
        }
    }
}

/// A closed project's TASKS being stopped (AppModel.closingTasks): the
/// window veiled and out of reach, saying what it waits for -- the work
/// still stopping -- and what comes once it has.
struct ClosingTasksOverlay: View {
    let closing: AppModel.ClosingTasks

    var body: some View {
        ZStack {
            // A veil: the window still seen, out of reach.
            Rectangle()
                .fill(Color(nsColor: .windowBackgroundColor).opacity(0.85))
                .ignoresSafeArea()
            VStack(spacing: 14) {
                ProgressView()
                    .controlSize(.large)
                Text("Stopping the work of “\(closing.project)”")
                    .font(.title3.weight(.semibold))
                    .multilineTextAlignment(.center)
                Text(next)
                    .foregroundStyle(.secondary)
                if closing.total > 0 {
                    ProgressView(value: Double(closing.total
                                               - closing.left.count),
                                 total: Double(closing.total))
                        .frame(width: 280)
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(Array(closing.left.prefix(5).enumerated()),
                                id: \.offset) { _, title in
                            Label {
                                Text(verbatim: title)
                            } icon: {
                                Image(systemName: "stop.circle")
                            }
                            .lineLimit(1)
                        }
                    }
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: 380, alignment: .leading)
                }
            }
            .padding(40)
        }
        // Every click is the veil's.
        .contentShape(Rectangle())
        .onTapGesture {}
        .accessibilityElement(children: .combine)
    }

    /// What comes once it has stopped.
    private var next: String {
        switch closing.reason {
        case .quit:
            String(localized: "Valtz quits once it has stopped.")
        case .open, .newProject:
            String(localized: "The next project opens once it has stopped.")
        case .newAnonymous:
            String(localized: "A new anonymous project starts once it has stopped.")
        case .window:
            String(localized: "The window closes once it has stopped.")
        }
    }
}

/// Asks before the editor's window closes (its close button; ⌘W closes
/// the project, File › Close Project): standing in as the window's
/// delegate, `windowShouldClose` asks; everything else goes on to the
/// delegate SwiftUI gave the window, as it was. SwiftUI has no way to
/// refuse a window's close.
@MainActor
final class WindowCloseGuard: NSObject, NSWindowDelegate {
    weak var window: NSWindow?
    nonisolated(unsafe) private weak var inner: NSWindowDelegate?
    private let ask: @MainActor () -> Bool

    init(window: NSWindow, ask: @escaping @MainActor () -> Bool) {
        self.window = window
        self.ask = ask
        super.init()
        inner = window.delegate
        window.delegate = self
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if let i = inner, i.windowShouldClose?(sender) == false {
            return false
        }
        return ask()
    }

    nonisolated override func responds(to aSelector: Selector!) -> Bool {
        super.responds(to: aSelector)
            || (inner?.responds(to: aSelector) ?? false)
    }

    nonisolated override func forwardingTarget(for aSelector: Selector!)
        -> Any? {
        if let inner, inner.responds(to: aSelector) { return inner }
        return super.forwardingTarget(for: aSelector)
    }
}

/// The window a view is in, told when it has one.
private struct WindowReader: NSViewRepresentable {
    let found: (NSWindow) -> Void

    func makeNSView(context: Context) -> Probe {
        let v = Probe()
        v.found = found
        return v
    }

    func updateNSView(_ v: Probe, context: Context) {}

    final class Probe: NSView {
        var found: ((NSWindow) -> Void)?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let window { found?(window) }
        }
    }
}

/// A Valtz PROJECT dragged over the window -- anywhere no panel takes the
/// drop for itself (the prompt, Assets, Layers, the timeline, the compare
/// bar keep theirs; the stage opens a project too) -- OPENS, the project
/// open asked about first, as the Finder's double-click opens one. Other
/// MEDIA let go there go into Assets, as on the list -- files (a
/// folder's pictures, clips and sounds), a browser's or Photos' picture,
/// an app's bytes, a link to media (DroppedMedia): with nothing on the
/// stage this is all of the editor, and it refused every file but a
/// project -- a Finder drag to an empty window's Assets came back (no
/// scripted drag reproduced it; let go here, it is taken all the same).
/// What the drag holds is told by its types as it comes in.
struct ProjectDropDelegate: DropDelegate {
    let model: AppModel
    @Binding var drag: ProjectDrag

    /// Read off the drag's pasteboard, not its providers (DroppedMedia):
    /// asked while the drag moves, a promise's providers would receive
    /// its files -- which AppKit refuses outside a perform, aborting.
    private var pasteboard: NSPasteboard { NSPasteboard(name: .drag) }

    func validateDrop(info: DropInfo) -> Bool {
        holds(pasteboard) != .nothing
    }

    func dropEntered(info: DropInfo) {
        drag.inside = true
        drag.holds = holds(pasteboard)
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: drag.holds == .nothing ? .forbidden : .copy)
    }

    func dropExited(info: DropInfo) {
        drag.inside = false
    }

    func performDrop(info: DropInfo) -> Bool {
        drag.inside = false
        let pb = pasteboard
        let intake = DroppedMedia.take(pb)
        if let url = intake.files.first(where: AppModel.isProjectFile) {
            model.openDropped(url)
            return true
        }
        // View only, or an agent at work holding the project: nothing
        // goes into it.
        if model.viewOnly || model.agentHolds { return false }
        // Not the stage's own drag (its picture flattened, let go beside
        // it: nothing), nor a row of Assets.
        intake.files.removeAll { model.isStageDrag($0) }
        if DroppedMedia.isStage(pb) || !DroppedMedia.rows(pb).isEmpty {
            intake.files = []
        }
        AppModel.dropLog.notice("taken on the window: \(intake.files.count) file(s), makes \(intake.makes)")
        model.takeIntoAssets(intake, added: true)
        return true
    }

    /// What the drag holds: a Valtz project among its files, else media
    /// to take -- files (not the stage's own, nor a row of Assets),
    /// promised files, bytes, links.
    private func holds(_ pb: NSPasteboard) -> ProjectDrag.Holds {
        let p = DroppedMedia.peek(pb)
        if p.files.contains(where: AppModel.isProjectFile) { return .project }
        // View only, or an agent at work: nothing goes into the project.
        if model.viewOnly || model.agentHolds || DroppedMedia.isStage(pb)
            || !DroppedMedia.rows(pb).isEmpty {
            return .nothing
        }
        let files = p.files.filter { !model.isStageDrag($0) }
        return files.isEmpty && p.made == 0 ? .nothing : .files
    }
}

/// A drag over the window: whether it is still over it, and what it
/// holds -- refused only when it holds nothing to take.
struct ProjectDrag: Equatable {
    enum Holds { case project, files, nothing }
    var inside = false
    var holds: Holds?
    var showsProject: Bool { inside && holds == .project }
}

/// The window ringed in the accent while a project is over it, and what
/// letting go does.
private struct ProjectDropMark: View {
    var body: some View {
        ZStack {
            Color.accentColor.opacity(0.08)
            RoundedRectangle(cornerRadius: 10, style: .continuous)
                .strokeBorder(Color.accentColor, lineWidth: 3)
                .padding(4)
            Label("Open Project", systemImage: "folder")
                .font(.title3.weight(.semibold))
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(.regularMaterial, in: Capsule())
        }
    }
}
