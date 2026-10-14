import SwiftUI
import UniformTypeIdentifiers

/// The window: the current mode full-bleed under a Preview-style title
/// bar (TitleBarContent), the inspector column on the right when asked
/// for, and a navigation panel that stays hidden until ☰ asks for it.
struct RootView: View {
    @Bindable var model: AppModel
    @Environment(\.colorScheme) private var scheme
    /// A drag over the window, and whether it holds a Valtz project
    /// (ProjectDropDelegate).
    @State private var projectDrag = ProjectDrag()

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                main
                // A plain column rather than SwiftUI's .inspector, which
                // over-sizes the stage.
                if model.inspectorOpen && model.screen == .editor {
                    InspectorColumn(model: model)
                }
            }
            // The machine's load and thermal state, the window's width
            // (View › Show Status Bar).
            if model.showsStatusBar {
                StatusBar(model: model)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        // A project dropped anywhere no panel takes a drop of its own:
        // opened, the one open asked about first.
        .onDrop(of: [.fileURL],
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
        .disabled(model.fleetServing != nil)
        .overlay {
            if let s = model.fleetServing {
                FleetServingOverlay(serving: s)
                    .transition(.opacity)
            }
        }
        .fleetAskAlert(model)
        // The title bar's bottom edge: while the pointer is over the title
        // bar, and always while the inspector or the markup toolbar sits
        // under it.
        .overlay(alignment: .top) {
            let editor = model.screen == .editor
            let shown = model.titlebarHover || model.screen == .log
                || (editor && (model.inspectorOpen || model.markupOpen
                               || model.promptImmersive))
            Rectangle()
                .fill(Color(nsColor: .separatorColor))
                .frame(height: 1)
                .opacity(shown ? 1 : 0)
                .animation(.easeInOut(duration: 0.2), value: shown)
                .allowsHitTesting(false)
        }
        .background {
            TitlebarHoverDetector { model.titlebarHover = $0 }
                .frame(width: 0, height: 0)
        }
        .onChange(of: model.searchText) { model.searchChanged() }
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
        // Several tasks under way: Assets shows them (AppModel.revealTasks)
        // -- as their number reaches two, not again while it stays there.
        .onChange(of: model.tasks.count) { was, now in
            if was < 2 && now >= 2 { model.revealTasks() }
        }
        // The window, for a project's size (AppModel.editorWindow).
        .background(WindowReader { model.editorWindow = $0 })
        .navigationTitle(model.windowTitle)
        .toolbar { TitleBarContent(model: model, scheme: scheme) }
        .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    /// The current mode -- under the markup toolbar, a row of its own
    /// below the title bar, as Preview's -- with the navigation panel
    /// over it. Immersive prompt editing takes the editor's place, under
    /// its own toolbar.
    private var main: some View {
        ZStack(alignment: .topLeading) {
            switch model.screen {
            case .editor:
                VStack(spacing: 0) {
                    // The Prompt Editor's toolbar, or the markup toolbar:
                    // the editor grows the prompt card itself (SimpleView).
                    if model.promptImmersive {
                        PromptBar(model: model)
                            // Its foot: the small stage keeps clear of it.
                            .onGeometryChange(for: CGFloat.self) {
                                $0.frame(in: .global).maxY
                            } action: { model.editorToolbarBottom = $0 }
                            .transition(.move(edge: .top)
                                .combined(with: .opacity))
                    } else if model.markupOpen {
                        MarkupBar(model: model)
                            .transition(.move(edge: .top)
                                .combined(with: .opacity))
                    }
                    SimpleView(model: model)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            case .log:
                LogView(model: model)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }

            if model.navOpen {
                // Click anywhere else to put the panel away.
                Color.black.opacity(0.001)
                    .onTapGesture { model.toggleNav() }
                NavPanel(model: model)
                    .padding(.top, 14)
                    .padding(.leading, 10)
                    .padding(.bottom, 10)
                    .transition(.move(edge: .leading)
                        .combined(with: .opacity))
            }
        }
    }
}

/// The inspector at the window's right, with its edge.
struct InspectorColumn: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 0) {
            Divider()
            InspectorView(model: model)
                .frame(width: 300)
        }
        .transition(.move(edge: .trailing).combined(with: .opacity))
    }
}

extension Color {
    /// The grey of Valtz's panels -- the composer's tray, the inspector:
    /// opaque, a shade off the window in either appearance.
    static let valtzPanel = Color(nsColor: NSColor(name: "valtz.panel") { a in
        a.bestMatch(from: [.darkAqua, .vibrantDark]) != nil
            ? NSColor(srgbRed: 0.165, green: 0.165, blue: 0.175, alpha: 1)
            : NSColor(srgbRed: 0.953, green: 0.953, blue: 0.961, alpha: 1)
    })
}

/// The ☰ column: a narrow glass strip of icons -- the screens (the
/// editor, the log) at the top, Settings (⌘,) at the foot -- set well
/// below the title bar's hairline.
struct NavPanel: View {
    @Bindable var model: AppModel
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(spacing: 6) {
            ForEach(AppScreen.allCases) { s in
                NavRow(title: s.label, symbol: s.symbol,
                       selected: model.screen == s) {
                    model.show(s)
                }
            }
            Spacer(minLength: 20)
            NavRow(title: String(localized: "Settings"), symbol: "gearshape",
                   selected: false) {
                model.toggleNav()
                openSettings()
            }
            .shortcutHint("⌘,")
        }
        .padding(6)
        .frame(width: 52)
        .frame(maxHeight: .infinity, alignment: .top)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16,
                                                    style: .continuous))
    }
}

/// One of its icons; its name in the tooltip and to VoiceOver.
private struct NavRow: View {
    let title: String   // already localized
    let symbol: String
    let selected: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 16))
                .foregroundStyle(selected ? Color.accentColor : .secondary)
                .frame(width: 40, height: 36)
            .contentShape(Rectangle())
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(selected ? Color.accentColor.opacity(0.16)
                          : hovering ? Color.primary.opacity(0.06)
                          : .clear))
        }
        .buttonStyle(.plain)
        .help(title)
        .accessibilityLabel(Text(verbatim: title))
        .onHover { hovering = $0 }
        .animation(.smooth(duration: 0.15), value: hovering)
    }
}

/// The window a view is in, told when it has one.
/// Asks before the editor's window closes (its close button, ⌘W, File ›
/// Close): standing in as the window's delegate, `windowShouldClose`
/// asks; everything else goes on to the delegate SwiftUI gave the window,
/// as it was. SwiftUI has no way to refuse a window's close.
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
/// open asked about first, as the Finder's double-click opens one. Only a
/// project is taken: what the drag holds is read as it comes in.
struct ProjectDropDelegate: DropDelegate {
    let model: AppModel
    @Binding var drag: ProjectDrag

    func validateDrop(info: DropInfo) -> Bool {
        info.hasItemsConforming(to: [.fileURL])
    }

    func dropEntered(info: DropInfo) {
        drag.number += 1
        drag.inside = true
        drag.project = nil
        let n = drag.number
        let providers = info.itemProviders(for: [.fileURL])
        Task { @MainActor in
            let found = await Self.project(providers) != nil
            // Read after the drag left or dropped: nothing to show.
            if drag.number == n && drag.inside { drag.project = found }
        }
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: drag.project == false ? .forbidden : .copy)
    }

    func dropExited(info: DropInfo) {
        drag.inside = false
    }

    func performDrop(info: DropInfo) -> Bool {
        let providers = info.itemProviders(for: [.fileURL])
        drag.inside = false
        Task { @MainActor in
            if let url = await Self.project(providers) {
                model.openDropped(url)
            }
        }
        return true
    }

    /// The first Valtz project among the drag's files.
    @MainActor
    private static func project(_ providers: [NSItemProvider]) async
        -> URL? {
        for p in providers where p.canLoadObject(ofClass: NSURL.self) {
            let u: URL? = await withCheckedContinuation { c in
                _ = p.loadObject(ofClass: NSURL.self) { u, _ in
                    c.resume(returning: (u as? NSURL) as URL?)
                }
            }
            if let u, AppModel.isProjectFile(u) { return u }
        }
        return nil
    }
}

/// A drag over the window: its number (a read's answer for an earlier
/// one is dropped), whether it is still over it, and whether its files
/// hold a project -- nil while they are read: refused only once known to
/// hold none.
struct ProjectDrag: Equatable {
    var number = 0
    var inside = false
    var project: Bool?
    var showsProject: Bool { inside && project == true }
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
