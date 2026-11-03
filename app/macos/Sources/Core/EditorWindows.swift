import AppKit
import SwiftUI

/// The editor's WINDOWS, each showing a project of its own -- its own
/// AppModel -- over the one core a process has (CoreService: one vpipe
/// session, DESIGN §4). The core is made once and its events handed to
/// every window, each taking its own project's (a job's events name its
/// project). The window in FRONT -- the last to be the main window -- is
/// the one the menus, Settings and the snapshot hooks act on.
///
/// A window's model is made before its window, as what it opens is asked
/// for -- the first at launch, a later one by Open… or New Project… --
/// and taken by the window SwiftUI makes for it (EditorWindow); a window
/// SwiftUI makes by itself starts an anonymous session.
@MainActor @Observable
final class EditorWindows {
    static let shared = EditorWindows()

    /// What a new window's model opens.
    enum Opening {
        case anonymous
        case open(String)
        case create(URL)
    }

    /// The process's core: made once, before any session -- what a
    /// crash left behind cleared first -- its events to every window.
    @ObservationIgnored private(set) lazy var core: CoreService? = {
        if ProcessInfo.processInfo.environment["VALTZ_DEFAULT_PROJECT"]
            == nil {
            AppModel.removeOrphanSessions()
        }
        let svc = CoreService()
        svc?.startEvents { ev in
            Task { @MainActor in EditorWindows.shared.dispatch(ev) }
        }
        return svc
    }()

    /// Every window's model, the first made first -- one made for a
    /// window not shown yet among them.
    private(set) var models: [AppModel] = []
    /// The window in front: the last of them to be the main window.
    private(set) var front: AppModel?
    /// Made, waiting for the window SwiftUI makes for them.
    @ObservationIgnored private var unclaimed: [AppModel] = []
    /// A window asked for (newWindow): in front as it is shown -- it is,
    /// though AppKit makes no window of an app not active the main one.
    @ObservationIgnored private weak var asked: AppModel?
    /// The first model made: what the menus act on before any window is
    /// main.
    @ObservationIgnored private var primary: AppModel?
    /// SwiftUI's, to open another window (EditorWindow hands it over).
    @ObservationIgnored var openWindow: OpenWindowAction?
    /// Projects the Finder asked Valtz to open before a window was there.
    @ObservationIgnored private var pending: [URL] = []
    @ObservationIgnored private var watch: [Any] = []
    /// The quit's models still to be asked about (quitReply).
    @ObservationIgnored private var quitting: [AppModel] = []

    private init() {
        let nc = NotificationCenter.default
        watch.append(nc.addObserver(
            forName: NSWindow.didBecomeMainNotification, object: nil,
            queue: .main
        ) { note in
            let w = note.object as? NSWindow
            MainActor.assumeIsolated {
                EditorWindows.shared.becameMain(w)
            }
        })
        // Settings' preferences, as another window's Settings set them.
        watch.append(nc.addObserver(
            forName: UserDefaults.didChangeNotification, object: nil,
            queue: .main
        ) { _ in
            MainActor.assumeIsolated {
                for m in EditorWindows.shared.models { m.readPreferences() }
            }
        })
    }

    /// The model the menus and Settings act on: the window in front's.
    var menuModel: AppModel {
        if let front { return front }
        if let m = models.first { return m }
        if let primary { return primary }
        let m = make(.anonymous)
        unclaimed.append(m)
        return m
    }

    /// The first window's model, made at launch (ValtzApp): a named
    /// project for a dev run (VALTZ_DEFAULT_PROJECT), else an anonymous
    /// session.
    func launch() {
        guard primary == nil else { return }
        let env = ProcessInfo.processInfo.environment
        let m = make(env["VALTZ_DEFAULT_PROJECT"].map { .open($0) }
                     ?? .anonymous, createIfMissing: true)
        unclaimed.append(m)
    }

    private func make(_ opening: Opening,
                      createIfMissing: Bool = false) -> AppModel {
        let m = AppModel(opening: opening, createIfMissing: createIfMissing)
        if primary == nil { primary = m }
        models.append(m)
        return m
    }

    /// A window's model: one made for it, else -- a window SwiftUI made by
    /// itself -- a new anonymous session.
    func claim() -> AppModel {
        if !unclaimed.isEmpty { return unclaimed.removeFirst() }
        return make(.anonymous)
    }

    /// A new window, opening `opening`.
    func newWindow(_ opening: Opening) {
        let m = make(opening)
        unclaimed.append(m)
        asked = m
        openWindow?(id: "editor")
    }

    /// A model's window is there: the first one in front, and the
    /// projects asked for before it opened.
    func shown(_ m: AppModel) {
        // Main already, before its model took it (a new window is ordered
        // in front as SwiftUI makes it): in front now.
        if front == nil || m === asked || m.editorWindow?.isMainWindow == true
            || m.editorWindow?.isKeyWindow == true {
            if m === asked { asked = nil }
            makeFront(m)
        }
        guard !pending.isEmpty else { return }
        let urls = pending
        pending = []
        for u in urls { open(u) }
    }

    private func becameMain(_ w: NSWindow?) {
        guard let w, let m = models.first(where: { $0.editorWindow === w })
        else { return }
        makeFront(m)
    }

    /// `m`'s window in front: the menus', Settings'.
    func makeFront(_ m: AppModel) {
        guard m !== front else { return }
        front = m
        // What Settings shows of the core -- the helpers, upscalers, the
        // agent's settings -- as another window's Settings may have left
        // them.
        if models.count > 1 { m.refreshMachine() }
    }

    /// A window closed (not the last: that one quits Valtz): its model
    /// let go, another window in front.
    func closed(_ m: AppModel) {
        models.removeAll { $0 === m }
        unclaimed.removeAll { $0 === m }
        guard front === m else { return }
        front = models.first { $0.editorWindow === NSApp.mainWindow }
            ?? models.last
    }

    /// More than one editor window: ⌘W closes the window in front.
    var several: Bool { models.count > 1 }

    /// File › Close (⌘W): a window in front of the editors -- Settings --
    /// closed as ⌘W closes any; else, with another editor window open, the
    /// one in front, as its close button closes it (its project asked
    /// about, its tasks stopped); else the last one's project, as New
    /// Anonymous Project closes it, the window staying.
    func closeWindowOrProject() {
        if let w = NSApp.keyWindow, !isEditor(w),
           w.styleMask.contains(.closable) {
            w.performClose(nil)
            return
        }
        let m = menuModel
        if several, let w = m.editorWindow {
            w.performClose(nil)
            return
        }
        m.newAnonymousProject()
    }

    /// The model whose window `w` is -- or is over (a popover, a sheet, a
    /// child window) -- else, for a window of no editor's (Settings, a
    /// panel), the one in front: ONE model takes each event.
    func owner(of w: NSWindow?) -> AppModel? {
        var x = w
        while let win = x {
            if let m = models.first(where: { $0.editorWindow === win }) {
                return m
            }
            x = win.parent ?? win.sheetParent
        }
        return front ?? models.first
    }

    func isEditor(_ w: NSWindow?) -> Bool {
        guard let w else { return false }
        return models.contains { $0.editorWindow === w }
    }

    /// The model showing the named project at `path`, if a window does.
    func model(showing path: String, except: AppModel? = nil) -> AppModel? {
        let want = URL(fileURLWithPath: path).standardizedFileURL.path
        return models.first { m in
            m !== except && !m.isAnonymous && m.projectId != nil
                && URL(fileURLWithPath: m.projectPath).standardizedFileURL
                    .path == want
        }
    }

    /// A project to open -- the Finder's, Open…'s: in front where a window
    /// shows it already; in place of the front window's anonymous session
    /// while that holds nothing; else in a window of its own.
    func open(_ url: URL) {
        guard models.contains(where: { $0.editorWindow != nil }) else {
            pending.append(url)
            return
        }
        if let m = model(showing: url.path) {
            m.bringToFront()
            return
        }
        let f = menuModel
        if f.holdsNothing {
            f.closeProject(.open) { f.openProject(path: url.path) }
        } else {
            newWindow(.open(url.path))
        }
    }

    /// A new project made at `url` (New Project…): as `open` places one.
    func create(_ url: URL) {
        if let m = model(showing: url.path) {
            m.bringToFront()
            m.flash(String(localized: "“\(url.deletingPathExtension().lastPathComponent)” is open in this window: close it before making a new project in its place."), level: .warning)
            return
        }
        let f = menuModel
        if f.holdsNothing {
            f.closeProject(.newProject) { f.createProject(at: url) }
        } else {
            newWindow(.create(url))
        }
    }

    /// An event of the core's, to every window.
    private func dispatch(_ ev: CoreEvent) {
        for m in models { m.handle(ev) }
    }

    // MARK: Quitting

    /// The quit: every window's project asked about in turn, from the
    /// one in front -- its tasks stopped, its changes saved or let go --
    /// any Cancel keeping Valtz open. `.terminateLater` while one waits
    /// for its tasks to stop; the reply then comes from here.
    func quitReply() -> NSApplication.TerminateReply {
        let rest = models.filter { !$0.closeConfirmed }
        quitting = rest.filter { $0 === front } + rest.filter { $0 !== front }
        switch askQuitting() {
        case true?: return .terminateNow
        case false?: return .terminateCancel
        case nil: return .terminateLater
        }
    }

    /// The next of the quit's projects asked about: true all agreed,
    /// false one cancelled, nil one waiting for its tasks (asked on once
    /// they have stopped, the reply given then).
    private func askQuitting() -> Bool? {
        while let m = quitting.first {
            quitting.removeFirst()
            if models.count > 1 { m.bringToFront() }
            switch m.prepareClose(.quit, then: { ok in
                guard ok else {
                    EditorWindows.shared.quitting = []
                    NSApp.reply(toApplicationShouldTerminate: false)
                    return
                }
                m.closeForQuit()
                if let all = EditorWindows.shared.askQuitting() {
                    NSApp.reply(toApplicationShouldTerminate: all)
                }
            }) {
            case .proceed:
                m.closeForQuit()
            case .cancelled:
                quitting = []
                return false
            case .waiting:
                return nil
            }
        }
        return true
    }

    /// Valtz quitting: the engine stopped -- its jobs finished or
    /// cancelled, the projects' databases closed -- then each window's
    /// session folder, and the process's share and drop folders.
    func shutdown() {
        core?.shutdown()
        for m in models { m.removeSession(shared: false) }
        AppModel.removeSharedFolders()
    }
}

/// One editor window: the model it shows -- the one made for it
/// (EditorWindows.claim) -- taken as it first appears.
struct EditorWindow: View {
    @State private var model: AppModel?
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        Group {
            if let model {
                RootView(model: model)
                    .frame(minWidth: model.windowMinWidth, minHeight: 620)
            } else {
                Color.clear
                    .frame(minWidth: AppModel.editorMinWidth, minHeight: 620)
            }
        }
        .onAppear {
            EditorWindows.shared.openWindow = openWindow
            if model == nil { model = EditorWindows.shared.claim() }
        }
    }
}
