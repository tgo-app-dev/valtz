import AppKit
import AVKit
import Carbon.HIToolbox
import ImageIO
import ScreenCaptureKit
import SwiftUI
import UniformTypeIdentifiers

@main
struct ValtzApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var delegate
    @State private var model = AppModel()

    /// One VALTZ_SNAPSHOT_MARKUP_OPS step, as the toolbar and the
    /// stage's pointer would do it.
    @MainActor
    static func markupStep(_ m: AppModel, _ step: String) async {
        let kv = step.split(separator: "=", maxSplits: 1).map(String.init)
        let arg = kv.count > 1 ? kv[1] : ""
        let mk = m.markup
        func points(_ s: String) -> [CGPoint] {
            s.split(separator: ";").compactMap { p in
                let xy = p.split(separator: ",").compactMap {
                    Double($0.trimmingCharacters(in: .whitespaces))
                }
                return xy.count == 2 ? CGPoint(x: xy[0], y: xy[1]) : nil
            }
        }
        func rgba(_ s: String) -> RGBA? {
            let h = s.hasPrefix("#") ? String(s.dropFirst()) : s
            guard h.count == 8, let v = UInt32(h, radix: 16) else { return nil }
            return RGBA(r: Double(v >> 24 & 0xff) / 255,
                        g: Double(v >> 16 & 0xff) / 255,
                        b: Double(v >> 8 & 0xff) / 255,
                        a: Double(v & 0xff) / 255)
        }
        func pointer(_ phase: MarkupPointer.Phase, _ p: CGPoint,
                     clicks: Int = 1, shift: Bool = false) {
            m.markupPointer(MarkupPointer(phase: phase, point: p,
                                          shift: shift, clickCount: clicks,
                                          pixelsPerPoint: 1))
        }
        switch kv[0] {
        case "tool":
            if let t = MarkupTool(rawValue: arg) {
                mk.tool = t
                m.markupToolChanged()
            }
        case "size": mk.radius = Double(arg) ?? mk.radius
        case "soft": mk.softness = Double(arg) ?? mk.softness
        case "width":
            let w = Double(arg) ?? mk.width
            mk.width = w
            m.editSelection { if $0.kind != .text { $0.width = w } }
        case "color":
            if let c = rgba(arg) {
                mk.edge = c
                m.editSelection { $0.stroke = c }
            }
        case "fill":
            if let c = rgba(arg) {
                mk.fill = c
                m.editSelection {
                    if $0.kind == .rect || $0.kind == .ellipse { $0.fill = c }
                }
            }
        case "font":
            // "Family,48,bold,italic,underline"
            let p = arg.split(separator: ",").map(String.init)
            var f = mk.font
            if let fam = p.first, !fam.isEmpty { f.family = fam }
            if p.count > 1, let s = Double(p[1]) { f.size = s }
            f.bold = p.contains("bold")
            f.italic = p.contains("italic")
            f.underline = p.contains("underline")
            mk.font = f
            m.editSelection { if $0.kind == .text { $0.font = f } }
        case "drag", "sdrag":
            let ps = points(arg)
            guard let first = ps.first else { return }
            pointer(.down, first)
            for p in ps.dropFirst() {
                pointer(.drag, p, shift: kv[0] == "sdrag")
            }
            pointer(.up, ps.last!)
        case "click", "dclick", "sclick":
            if let p = points(arg).first {
                let n = kv[0] == "dclick" ? 2 : 1
                pointer(.down, p, clicks: n, shift: kv[0] == "sclick")
                pointer(.up, p, clicks: n, shift: kv[0] == "sclick")
            }
        case "key":
            m.markupKey(arg == "delete" ? .delete : .escape)
        case "text":
            m.editSelection { if $0.kind == .text { $0.text = arg } }
        // Markup objects cut, copied and pasted, as the Edit menu does on
        // the stage.
        case "ocut", "ocopy", "opaste":
            let k: MarkupKey = kv[0] == "ocut" ? .cut
                : kv[0] == "ocopy" ? .copy : .paste
            let can = m.markupCan(k)
            m.markupKey(k)
            print("snapshot: objects \(kv[0]) can=\(can) "
                  + "selected=\(mk.selection.map(\.kind.rawValue)) "
                  + "at=\(mk.selection.map { "\(Int($0.x0)),\(Int($0.y0))" })")
        // The text's edit window: opened, typed into ("\n" a line),
        // a range selected, the Edit menu's commands -- as the menu sends
        // them, to the key window's text, when the panel is key -- and
        // drawn to <snapshot>-text.png.
        case "edit":
            m.editMarkupText()
        case "type", "sel", "cut", "copy", "paste", "undo", "redo",
             "selall", "panel":
            let tp = mk.textPanel
            guard let tv = tp.editor, tp.isOpen else {
                print("snapshot: text-panel closed for \(kv[0]) "
                      + "active=\(NSApp.isActive)")
                return
            }
            let key = NSApp.keyWindow === tp.window
            switch kv[0] {
            case "type":
                tv.insertText(arg.replacingOccurrences(of: "\\n", with: "\n"),
                              replacementRange: tv.selectedRange())
            case "sel":
                let r = arg.split(separator: ",").compactMap { Int($0) }
                if r.count == 2 {
                    tv.setSelectedRange(NSRange(location: r[0], length: r[1]))
                }
            case "selall": tv.selectAll(nil)
            case "cut", "copy", "paste":
                let sel = Selector(kv[0] + ":")
                if !(key && NSApp.sendAction(sel, to: nil, from: nil)) {
                    tv.perform(sel, with: nil)
                }
            case "undo":
                if key { m.undoAny() } else { tv.undoManager?.undo() }
            case "redo":
                if key { m.redoAny() } else { tv.undoManager?.redo() }
            default:
                if let w = tp.window, let v = w.contentView?.superview,
                   let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) {
                    v.cacheDisplay(in: v.bounds, to: rep)
                    let out = ProcessInfo.processInfo
                        .environment["VALTZ_SNAPSHOT"].map {
                            ($0 as NSString).deletingPathExtension + "-text.png"
                        } ?? arg
                    try? rep.representation(using: .png, properties: [:])?
                        .write(to: URL(fileURLWithPath: out))
                }
            }
            // An event through the app: the undo group a step opened ends
            // with it, as with each key typed.
            if let ev = NSEvent.otherEvent(
                with: .applicationDefined, location: .zero, modifierFlags: [],
                timestamp: 0, windowNumber: 0, context: nil, subtype: 0,
                data1: 0, data2: 0) {
                NSApp.postEvent(ev, atStart: false)
            }
            try? await Task.sleep(for: .milliseconds(50))
            let words = m.markup.selectedText?.text ?? "-"
            print("snapshot: text-panel \(kv[0]) key=\(key) "
                  + "undoLevel=\(tv.undoManager?.groupingLevel ?? -1) "
                  + "editor=\(tv.string.debugDescription) "
                  + "object=\(words.debugDescription) "
                  + "selection=\(NSStringFromRange(tv.selectedRange())) "
                  + "pasteboard=\((NSPasteboard.general.string(forType: .string) ?? "-").debugDescription)")
        case "pixels":
            m.materializeSelection()
        case "layers":
            let ids = arg.split(separator: ",", omittingEmptySubsequences: false)
                .map { AppDelegate.layerId(String($0)) }
            if let first = ids.first { m.selectLayer(first) }
            for id in ids.dropFirst() { m.toggleLayerSelection(id) }
        case "merge":
            m.mergeSelectedLayers()
        case "mask", "unmask":
            m.setLayerMask(AppDelegate.layerId(arg), kv[0] == "mask")
        case "add":
            m.addLayer()
        case "close":
            m.markupOpen = false
        default:
            break
        }
    }

    var body: some Scene {
        Window("Valtz", id: "main") {
            RootView(model: model)
                .frame(minWidth: 820, minHeight: 620)
                .onAppear { delegate.model = model }
        }
        .windowToolbarStyle(.unified(showsTitle: false))
        // The first launch's; after it, the window as it was left -- and
        // a project opened at the size it was last looked at.
        .defaultSize(width: 1024, height: 768)
        .commands {
            CommandGroup(after: .appInfo) {
                if model.updater.canCheck {
                    Button("Check for Updates…") {
                        model.updater.checkForUpdates()
                    }
                }
            }
            CommandGroup(replacing: .newItem) {
                Button("New Project…") { newProject() }
                    .keyboardShortcut("n")
                // Private and untitled, as a browser's private window (⇧⌘N).
                Button("New Anonymous Project") {
                    model.newAnonymousProject()
                }
                    .keyboardShortcut("n", modifiers: [.command, .shift])
                Button("Open Project…") { openProject() }
                    .keyboardShortcut("o")
            }
            // The project as a document (DESIGN §5b): Save writes its
            // working copy to its file; Revert to Saved goes back to it.
            CommandGroup(replacing: .saveItem) {
                Button("Save") { model.saveDocument() }
                    .keyboardShortcut("s")
                    .disabled(model.projectId == nil)
                Button("Save As…") { model.saveDocumentAs() }
                    .keyboardShortcut("s", modifiers: [.command, .shift])
                    .disabled(model.projectId == nil)
                Button("Revert to Saved") { model.revertDocument() }
                    .disabled(!model.document.dirty || model.document.untitled
                              || model.isAnonymous)
            }
            // ⌘Z: the text being typed while it has typing to undo, else
            // the project's history of commands.
            CommandGroup(replacing: .undoRedo) {
                Button(model.undoMenuTitle) { model.undoAny() }
                    .keyboardShortcut("z")
                    .disabled(!model.canUndoAny)
                Button(model.redoMenuTitle) { model.redoAny() }
                    .keyboardShortcut("z", modifiers: [.command, .shift])
                    .disabled(!model.canRedoAny)
            }
            CommandGroup(after: .importExport) {
                Button("Attach Media…") { attachMedia() }
                    .keyboardShortcut("i", modifiers: [.command, .shift])
            }
            // The image, video or sound on screen, as the share menu's Save…
            // writes it: a format, its quality.
            CommandGroup(after: .saveItem) {
                Divider()
                Button(model.saveItemTitle) {
                    ShareButton.save(model)
                }
                    .keyboardShortcut("e")
                    .disabled(model.currentAsset?.url == nil)
            }
            CommandGroup(after: .textEditing) {
                Button("Find") { model.findRequest += 1 }
                    .keyboardShortcut("f")
            }
            CommandGroup(after: .toolbar) {
                Divider()
                Button("Zoom In") { model.zoom(.zoomIn) }
                    .keyboardShortcut("+")
                    .disabled(!model.canZoom)
                Button("Zoom Out") { model.zoom(.zoomOut) }
                    .keyboardShortcut("-")
                    .disabled(!model.canZoom)
                Button("Actual Size") { model.zoom(.actualSize) }
                    .keyboardShortcut("0")
                    .disabled(!model.canZoom)
                Button("Zoom to Fit") { model.zoom(.fit) }
                    .keyboardShortcut("9")
                    .disabled(!model.canZoom)
                // Rounded by default; square to see a picture's own
                // corners.
                Toggle("Round Stage Corners", isOn: Binding(
                    get: { model.roundStageCorners },
                    set: { on in
                        withAnimation(.smooth(duration: 0.2)) {
                            model.roundStageCorners = on
                        }
                    }))
                // A 3 × 3 grid over what the stage shows, ⌘' as the grid
                // is in Photoshop.
                Toggle("Show Guides", isOn: Binding(
                    get: { model.showsGuides },
                    set: { model.showsGuides = $0 }))
                    .keyboardShortcut("'")
                Divider()
                Button(model.inspectorOpen ? "Hide Inspector" : "Show Inspector") {
                    withAnimation(AppModel.motion) { model.inspectorOpen.toggle() }
                }
                .keyboardShortcut("i", modifiers: [.command, .option])
                Button(model.markupOpen ? "Hide Markup Toolbar"
                                        : "Show Markup Toolbar") {
                    withAnimation(AppModel.motion) { model.markupOpen.toggle() }
                }
                .keyboardShortcut("a", modifiers: [.command, .shift])
                .disabled(model.promptImmersive)
                // The prompt the window's height, several open as tabs
                // (PromptStudio).
                Toggle("Prompt Editor", isOn: Binding(
                    get: { model.promptImmersive },
                    set: { on in
                        guard on != model.promptImmersive else { return }
                        withAnimation(AppModel.motion) {
                            model.toggleImmersivePrompt()
                        }
                    }))
                .keyboardShortcut("e", modifiers: [.command, .shift])
                Toggle("Markdown as It Reads", isOn: Binding(
                    get: { model.promptMarkdown },
                    set: { model.promptMarkdown = $0 }))
                // The machine's load and thermal state along the bottom:
                // ⌘/, as Finder's status bar.
                Button(model.showsStatusBar ? "Hide Status Bar"
                                            : "Show Status Bar") {
                    withAnimation(AppModel.motion) {
                        model.showsStatusBar.toggle()
                    }
                }
                .keyboardShortcut("/")
            }
            CommandGroup(before: .sidebar) {
                Button(model.navOpen ? "Hide Navigation" : "Show Navigation") {
                    model.toggleNav()
                }
                .keyboardShortcut("n", modifiers: [.command, .control])
                Divider()
            }
        }
        Settings {
            SettingsView(model: model)
        }
    }

    /// Dev aid: a mouse click, sent to `window`, on the first control in
    /// `root` that takes one -- and whether a menu began tracking.
    @MainActor
    static func clickFirstControl(in root: NSView, window: NSWindow) {
        func find(_ v: NSView) -> NSView? {
            if v is NSPopUpButton || v is NSButton
                || String(describing: type(of: v)) == "_FocusRingView" {
                return v
            }
            for s in v.subviews { if let hit = find(s) { return hit } }
            return nil
        }
        func tree(_ v: NSView, _ d: Int) -> String {
            String(repeating: " ", count: d) + String(describing: type(of: v))
                + " \(v.frame)\n" + v.subviews.map { tree($0, d + 1) }.joined()
        }
        FileHandle.standardError.write(Data("snapshot: accessory\n\(tree(root, 1))".utf8))
        // Where to click: a control found, else the accessory's left
        // third, where the menu is.
        let target = find(root)
        let local = target.map { NSPoint(x: $0.bounds.midX, y: $0.bounds.midY) }
            ?? NSPoint(x: root.bounds.width * 0.55, y: root.bounds.maxY - 22)
        let inWindow = (target ?? root).convert(local, to: nil)
        final class Seen: @unchecked Sendable { var menu: NSMenu? }
        let seen = Seen()
        let obs = NotificationCenter.default.addObserver(
            forName: NSMenu.didBeginTrackingNotification, object: nil,
            queue: nil) { note in
            seen.menu = note.object as? NSMenu
            // Closed again a moment later, from the run loop.
            let t = Timer(timeInterval: 0.5, repeats: false) { _ in
                seen.menu?.cancelTracking()
            }
            RunLoop.main.add(t, forMode: .common)
        }
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            if let e = NSEvent.mouseEvent(
                with: type, location: inWindow, modifierFlags: [],
                timestamp: ProcessInfo.processInfo.systemUptime,
                windowNumber: window.windowNumber, context: nil,
                eventNumber: 0, clickCount: 1, pressure: 1) {
                window.sendEvent(e)
            }
        }
        NotificationCenter.default.removeObserver(obs)
        FileHandle.standardError.write(Data(
            "snapshot: clicked \(target.map { String(describing: type(of: $0)) } ?? "the accessory") at \(inWindow): menu \(seen.menu != nil ? "opened" : "did NOT open")\n".utf8))
    }

    /// `body` run by the main run loop itself, not from a task: a modal
    /// panel opened from a task holds the main queue until it closes.
    static func fromRunLoop(_ body: @escaping @MainActor () -> Void) {
        CFRunLoopPerformBlock(CFRunLoopGetMain(),
                              CFRunLoopMode.commonModes.rawValue) {
            MainActor.assumeIsolated { body() }
        }
        CFRunLoopWakeUp(CFRunLoopGetMain())
    }

    private func newProject() {
        guard model.confirmClose() else { return }
        let panel = NSSavePanel()
        panel.title = String(localized: "New Valtz Project")
        panel.nameFieldStringValue = "Untitled.valtz"
        panel.allowedContentTypes = [.valtzProject]
        if panel.runModal() == .OK, let url = panel.url {
            model.createProject(at: url)
        }
    }

    private func openProject() {
        guard model.confirmClose() else { return }
        let panel = NSOpenPanel()
        panel.title = String(localized: "Open Valtz Project")
        panel.canChooseDirectories = true
        panel.treatsFilePackagesAsDirectories = false
        panel.allowedContentTypes = [.valtzProject]
        if panel.runModal() == .OK, let url = panel.url {
            model.openProject(path: url.path)
        }
    }

    /// Pictures and clips into the prompt, as its paperclip puts them.
    private func attachMedia() {
        let panel = NSOpenPanel()
        panel.title = String(localized: "Attach Media")
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [.image, .movie]
        if panel.runModal() == .OK {
            model.addReferences(panel.urls)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    @MainActor var model: AppModel? {
        didSet { openPending() }
    }
    /// Projects Finder asked Valtz to open -- a double-click, a drop on
    /// its Dock icon -- held until the window's model is there.
    @MainActor private var pending: [URL] = []

    func application(_ application: NSApplication, open urls: [URL]) {
        pending += urls.filter { $0.pathExtension.lowercased() == "valtz" }
        openPending()
    }

    /// One window, one project: the last one asked for, once the one open
    /// has been saved or let go.
    @MainActor
    private func openPending() {
        guard let model, let url = pending.last else { return }
        pending = []
        guard model.confirmClose() else { return }
        model.openProject(path: url.path)
    }

    func applicationDidFinishLaunching(_ notification: Notification) {
        // A bare executable (not launched through LaunchServices) starts
        // as a background process; make it a regular foreground app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate()
        scheduleSnapshotIfRequested()
    }

    /// A layer as a hook names it: "0" is layer 0, whose id is "" ("bg"
    /// too, as it was once called).
    nonisolated static func layerId(_ s: String) -> String {
        s == "0" || s == "bg" ? "" : s
    }

    /// The main window drawn by AppKit (cacheDisplay) to a PNG: a second
    /// picture in one snapshot run.
    @MainActor
    static func drawWindow(to path: String) {
        guard let win = NSApp.windows.first(where: {
                  $0.isVisible && !$0.className.contains("Popover") }),
              let content = win.contentView else { return }
        let v = content.superview ?? content
        guard let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds)
        else { return }
        v.cacheDisplay(in: v.bounds, to: rep)
        try? rep.representation(using: .png, properties: [:])?
            .write(to: URL(fileURLWithPath: path))
    }

    /// Dev aid: VALTZ_SNAPSHOT=<file.png> captures the main window after
    /// VALTZ_SNAPSHOT_DELAY seconds (default 4), prints a one-line summary
    /// of the UI state to stderr, then quits -- for UI checks from a
    /// terminal or CI. ScreenCaptureKit (current-process content) gives a
    /// true composite but needs a properly signed build; ad-hoc dev builds
    /// fall back to cacheDisplay, which renders the Core Animation viewer
    /// faithfully but omits vibrancy-backed controls.
    @MainActor
    private func scheduleSnapshotIfRequested() {
        let env = ProcessInfo.processInfo.environment
        guard let path = env["VALTZ_SNAPSHOT"] else { return }
        let delay = Double(env["VALTZ_SNAPSHOT_DELAY"] ?? "") ?? 4
        Task { @MainActor in
            // Optional scripting for UI checks: a prompt, the drawer or
            // the navigation open, and Start pressed.
            try? await Task.sleep(for: .seconds(1))
            if let m = model {
                // The window's size, "1600x900" (points).
                if let sz = env["VALTZ_SNAPSHOT_SIZE"],
                   let win = NSApp.windows.first(where: { $0.isVisible }) {
                    let wh = sz.split(separator: "x").compactMap { Double($0) }
                    if wh.count == 2 {
                        var f = win.frame
                        f.origin.y += f.height - wh[1]
                        f.size = CGSize(width: wh[0], height: wh[1])
                        win.setFrame(f, display: true)
                    }
                }
                if let p = env["VALTZ_SNAPSHOT_PROMPT"] { m.setPrompt(p) }
                // A captured prompt into the box: its id, or "newest".
                if let v = env["VALTZ_SNAPSHOT_USE_PROMPT"],
                   let a = v == "newest"
                       ? m.assets.last(where: \.isPrompt)
                       : m.assets.first(where: { $0.id == v }) {
                    m.usePrompt(a)
                }
                // The generation card's Create row, then its model field
                // (a model id; unset is Auto).
                if let v = env["VALTZ_SNAPSHOT_MODALITY"],
                   let v = Modality(rawValue: v) { m.setModality(v) }
                if let id = env["VALTZ_SNAPSHOT_MODEL"] { m.modelChoice = id }
                // Favor ("speed", ..., "custom": its panel opens), and
                // Custom's values ("sol_attn=true,steps=8"), as the panel
                // sets them.
                if let v = env["VALTZ_SNAPSHOT_FAVOR"],
                   let p = Preference(rawValue: v) {
                    // Favor lives in the generation card: open, as a
                    // person choosing it has it.
                    m.openPanel = .generate
                    try? await Task.sleep(for: .milliseconds(400))
                    m.choosePreference(p)
                }
                // Weights (LoRAs, DiT / VAE checkpoints), as if dropped
                // on Custom's panel: each filed by what it is.
                if let spec = env["VALTZ_SNAPSHOT_WEIGHTS"] {
                    m.addWeights(spec.split(separator: ",").map {
                        URL(fileURLWithPath: String($0))
                    })
                }
                if let spec = env["VALTZ_SNAPSHOT_TUNE"] {
                    for part in spec.split(separator: ",") {
                        let kv = part.split(separator: "=").map(String.init)
                        guard kv.count == 2 else { continue }
                        // "turbo=off": the Turbo LoRA unchecked in the list.
                        if kv[0] == "turbo" {
                            if let p = m.tuningInfo?.turboLoRA, !p.isEmpty {
                                m.setLoRA(p, on: !["off", "false", "none"]
                                              .contains(kv[1]))
                            }
                            continue
                        }
                        switch kv[1] {
                        case "true", "on": m.setTuning(kv[0], flag: true)
                        case "false", "off": m.setTuning(kv[0], flag: false)
                        default:
                            if let d = Double(kv[1]) {
                                m.setTuning(kv[0], number: d)
                            }
                        }
                    }
                }
                // Custom's panel drawn by AppKit beside the snapshot (a
                // popover is a window of its own): <snapshot>-custom.png.
                if env["VALTZ_SNAPSHOT_FAVOR"] == "custom",
                   let shot = env["VALTZ_SNAPSHOT"] {
                    try? await Task.sleep(for: .milliseconds(800))
                    let pops = NSApp.windows.filter {
                        $0.isVisible && $0.className.contains("Popover")
                    }
                    if let v = pops.first?.contentView,
                       let rep = v.bitmapImageRepForCachingDisplay(
                           in: v.bounds) {
                        v.cacheDisplay(in: v.bounds, to: rep)
                        try? rep.representation(using: .png, properties: [:])?
                            .write(to: URL(fileURLWithPath: shot
                                .replacingOccurrences(of: ".png",
                                                      with: "-custom.png")))
                    }
                    FileHandle.standardError.write(Data(
                        "snapshot: custom panel=\(pops.isEmpty ? "-" : "drawn")\n".utf8))
                }
                // Favor's Custom clicked again, `n` times ("2"): clicks
                // posted to the window just under the tip of the panel's
                // arrow, which points at the Custom segment -- each one
                // prints whether the panel is open after it.
                if let n = Int(env["VALTZ_SNAPSHOT_TUNE_CLICKS"] ?? ""),
                   let win = NSApp.windows.first(where: {
                       $0.isVisible && !$0.className.contains("Popover") }) {
                    var at: NSPoint?
                    for i in 0..<n {
                        if let pop = NSApp.windows.first(where: {
                            $0.isVisible && $0.className.contains("Popover")
                        }) {
                            // The panel opens above the segment (there is
                            // no room below the card), its arrow's tip at
                            // the panel's foot: the click goes just under
                            // it, on the segment.
                            at = NSPoint(x: pop.frame.midX - win.frame.minX,
                                         y: win.frame.maxY - pop.frame.minY
                                            + 10)
                        }
                        guard let p = at else { break }
                        Self.postClick(x: p.x, y: p.y)
                        try? await Task.sleep(for: .milliseconds(900))
                        FileHandle.standardError.write(Data(
                            "snapshot: tune-click \(i + 1) at \(Int(p.x)),\(Int(p.y)) open=\(m.showsTuning)\n".utf8))
                    }
                }
                // A song's plan ("full", "melody", "off") and the most it
                // may run, in seconds ("auto": as long as the model makes
                // it).
                if let v = env["VALTZ_SNAPSHOT_SONG_PLAN"],
                   let p = SongPlan(rawValue: v) { m.songPlan = p }
                if let v = env["VALTZ_SNAPSHOT_SONG_LENGTH"] {
                    m.songSeconds = Int(v)
                }
                // A clip's length: a preset's seconds ("5"), or what the
                // Length field takes ("100 frames", "7.5 s").
                if let v = env["VALTZ_SNAPSHOT_LENGTH"] {
                    if let n = Int(v), AppModel.clipLengths.contains(n) {
                        m.clipSeconds = n
                    } else {
                        m.setClipLength(v)
                    }
                }
                if env["VALTZ_SNAPSHOT_WIPE_LINE"] == "0" {
                    m.stage.wipeLine = false
                }
                if env["VALTZ_SNAPSHOT_COMPACT"] != nil {
                    m.promptCompact = true
                }
                if let a = env["VALTZ_SNAPSHOT_APPEARANCE"] {
                    m.setAppearance(dark: a == "dark")
                }
                if env["VALTZ_SNAPSHOT_INSPECTOR"] != nil {
                    m.inspectorOpen = true
                }
                if env["VALTZ_SNAPSHOT_STATUS_BAR"] != nil {
                    m.showsStatusBar = true
                }
                if env["VALTZ_SNAPSHOT_MARKUP"] != nil { m.markupOpen = true }
                if let n = env["VALTZ_SNAPSHOT_RENAME"] { m.renameSession(n) }
                if env["VALTZ_SNAPSHOT_TITLE_EDIT"] != nil {
                    // After the prompt has taken the focus, as a click on
                    // the name would come.
                    try? await Task.sleep(for: .milliseconds(500))
                    m.renamingTitle = true
                    // Clicks posted to the window ("x,y" in points from
                    // its top left; ";" between), as a pointer's would
                    // come (the pointer itself does not move).
                    if let spec = env["VALTZ_SNAPSHOT_TITLE_CLICK"] {
                        try? await Task.sleep(for: .milliseconds(700))
                        for c in spec.split(separator: ";") {
                            let xy = c.split(separator: ",")
                                .compactMap { Double($0) }
                            guard xy.count == 2 else { continue }
                            Self.postClick(x: xy[0], y: xy[1])
                            try? await Task.sleep(for: .milliseconds(500))
                            FileHandle.standardError.write(Data(
                                "snapshot: title-click \(Int(xy[0])),\(Int(xy[1])) renaming=\(m.renamingTitle)\n".utf8))
                        }
                    }
                }
                if env["VALTZ_SNAPSHOT_TITLEBAR_HOVER"] != nil {
                    m.titlebarHover = true
                }
                // A picture attached as the paperclip would, then time
                // for its import to land (Start needs the asset).
                // A picture dropped on the stage: the base to edit.
                if let f = env["VALTZ_SNAPSHOT_BASE"] {
                    let url = URL(fileURLWithPath: f)
                    m.setBase(from: url)
                    for _ in 0..<100 where m.asset(forFile: url) == nil
                        || m.baseAttachment == nil {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                }
                // Comma-separated: each staged in turn.
                if let f = env["VALTZ_SNAPSHOT_ATTACH"] {
                    let urls = f.split(separator: ",").map {
                        URL(fileURLWithPath: String($0))
                    }
                    m.addReferences(urls)
                    for url in urls {
                        for _ in 0..<100 where m.asset(forFile: url) == nil {
                            try? await Task.sleep(for: .milliseconds(100))
                        }
                    }
                }
                // The prompt row's media, by place (0 first): "focus=1"
                // (a click on it), "mention=0" (dragged into the text, at
                // its end), "move=2:0" (along the row), "remove=1",
                // "base=1" (its pencil badge).
                if let spec = env["VALTZ_SNAPSHOT_REFS"] {
                    for step in spec.split(separator: ";") {
                        try? await Task.sleep(for: .milliseconds(500))
                        let kv = step.split(separator: "=").map(String.init)
                        let args = (kv.count > 1 ? kv[1] : "")
                            .split(separator: ":").compactMap { Int($0) }
                        let row = m.promptAttachments
                        guard let n = args.first, row.indices.contains(n)
                        else { continue }
                        let id = row[n].id
                        switch kv[0] {
                        case "focus": m.toggleReferenceFocus(id)
                        case "remove": m.removeReference(id)
                        case "base": m.toggleBase(id)
                        case "move" where args.count > 1:
                            m.moveReference(id, to: args[1])
                        case "mention":
                            if let win = NSApp.windows.first(where: {
                                   $0.isVisible }),
                               let root = win.contentView?.superview,
                               let tv = Self.find(PromptTextView.self,
                                                  in: root) {
                                tv.insertMentions([id],
                                                  at: tv.string.utf16.count)
                            }
                        default: break
                        }
                    }
                    try? await Task.sleep(for: .milliseconds(500))
                }
                // Text typed at the end of the prompt, a character at a time
                // (a tag typed whole becomes a tag).
                if let typed = env["VALTZ_SNAPSHOT_TYPE_TEXT"],
                   let win = NSApp.windows.first(where: { $0.isVisible }),
                   let root = win.contentView?.superview,
                   let tv = Self.find(PromptTextView.self, in: root) {
                    // The scripted prompt in the view first.
                    try? await Task.sleep(for: .milliseconds(300))
                    win.makeFirstResponder(tv)
                    tv.setSelectedRange(NSRange(location: tv.string.utf16.count,
                                                length: 0))
                    for ch in typed {
                        tv.insertText(String(ch),
                                      replacementRange: tv.selectedRange())
                    }
                    try? await Task.sleep(for: .milliseconds(300))
                }
                // An input method's composition, as one drives the text
                // view: the caret put somewhere, unconfirmed text marked
                // there and grown, then committed. Each step prints where
                // the marked text is and where its panel would go.
                if let script = env["VALTZ_SNAPSHOT_IME"],
                   let win = NSApp.windows.first(where: { $0.isVisible }),
                   let root = win.contentView?.superview,
                   let tv = Self.find(PromptTextView.self, in: root) {
                    try? await Task.sleep(for: .milliseconds(300))
                    win.makeFirstResponder(tv)
                    let none = NSRange(location: NSNotFound, length: 0)
                    for step in script.split(separator: ";") {
                        let parts = step.split(separator: ":", maxSplits: 1)
                        let arg = parts.count > 1 ? String(parts[1]) : ""
                        switch parts.first ?? "" {
                        case "caret":
                            tv.setSelectedRange(NSRange(
                                location: min(Int(arg) ?? 0,
                                              tv.string.utf16.count),
                                length: 0))
                        case "mark":
                            tv.setMarkedText(
                                arg,
                                selectedRange: NSRange(
                                    location: arg.utf16.count, length: 0),
                                replacementRange: none)
                        case "commit":
                            tv.insertText(arg, replacementRange: none)
                        default:
                            continue
                        }
                        // As between keystrokes: the views update.
                        try? await Task.sleep(for: .milliseconds(150))
                        let m = tv.markedRange()
                        let probe = m.location == NSNotFound
                            ? tv.selectedRange() : m
                        let r = tv.firstRect(forCharacterRange: probe,
                                             actualRange: nil)
                        let inWin = win.convertFromScreen(r)
                        print("snapshot: ime \(step) sel=\(tv.selectedRange().location),\(tv.selectedRange().length) marked=\(m.location == NSNotFound ? "-" : "\(m.location),\(m.length)") panelX=\(Int(inWin.minX)) panelY=\(Int(win.frame.height - inWin.maxY)) text=\(tv.string.replacingOccurrences(of: "\n", with: "|"))")
                    }
                    try? await Task.sleep(for: .milliseconds(300))
                }
                // The same through a real input method: Pinyin selected,
                // key events posted to the window as typing sends them (the
                // input method composes in its own process), each step
                // printed; the input source put back after.
                if let script = env["VALTZ_SNAPSHOT_IME_KEYS"],
                   let win = NSApp.windows.first(where: { $0.isVisible }),
                   let root = win.contentView?.superview,
                   let tv = Self.find(PromptTextView.self, in: root) {
                    try? await Task.sleep(for: .milliseconds(300))
                    win.makeFirstResponder(tv)
                    let before = TISCopyCurrentKeyboardInputSource()
                        .takeRetainedValue()
                    let filter = [kTISPropertyInputSourceID as String:
                                    "com.apple.inputmethod.SCIM.ITABC"]
                        as CFDictionary
                    NSApp.activate(ignoringOtherApps: true)
                    win.makeKeyAndOrderFront(nil)
                    if let list = TISCreateInputSourceList(filter, false)?
                        .takeRetainedValue() as? [TISInputSource],
                       let pinyin = list.first {
                        let st = TISSelectInputSource(pinyin)
                        print("snapshot: ime-key select=\(st)")
                    }
                    try? await Task.sleep(for: .milliseconds(500))
                    let now = TISCopyCurrentKeyboardInputSource()
                        .takeRetainedValue()
                    if let id = TISGetInputSourceProperty(
                        now, kTISPropertyInputSourceID) {
                        let s = Unmanaged<CFString>.fromOpaque(id)
                            .takeUnretainedValue() as String
                        print("snapshot: ime-key source=\(s) active=\(NSApp.isActive) context=\(tv.inputContext?.selectedKeyboardInputSource ?? "-")")
                    }
                    let codes: [Character: Int] = [
                        "a": kVK_ANSI_A, "b": kVK_ANSI_B, "c": kVK_ANSI_C,
                        "d": kVK_ANSI_D, "e": kVK_ANSI_E, "f": kVK_ANSI_F,
                        "g": kVK_ANSI_G, "h": kVK_ANSI_H, "i": kVK_ANSI_I,
                        "j": kVK_ANSI_J, "k": kVK_ANSI_K, "l": kVK_ANSI_L,
                        "m": kVK_ANSI_M, "n": kVK_ANSI_N, "o": kVK_ANSI_O,
                        "p": kVK_ANSI_P, "q": kVK_ANSI_Q, "r": kVK_ANSI_R,
                        "s": kVK_ANSI_S, "t": kVK_ANSI_T, "u": kVK_ANSI_U,
                        "v": kVK_ANSI_V, "w": kVK_ANSI_W, "x": kVK_ANSI_X,
                        "y": kVK_ANSI_Y, "z": kVK_ANSI_Z, " ": kVK_Space,
                        "^": kVK_Escape,
                    ]
                    func report(_ step: String) {
                        let m = tv.markedRange()
                        let marking = tv.hasMarkedText()
                        let r = tv.firstRect(
                            forCharacterRange: marking ? m : tv.selectedRange(),
                            actualRange: nil)
                        let inWin = win.convertFromScreen(r)
                        var glyph = ""
                        if marking, let lm = tv.layoutManager,
                           let tc = tv.textContainer {
                            let g = lm.glyphIndexForCharacter(at: m.location)
                            let frag = lm.lineFragmentRect(forGlyphAt: g,
                                                           effectiveRange: nil)
                            let loc = lm.location(forGlyphAt: g)
                            let box = lm.boundingRect(
                                forGlyphRange: lm.glyphRange(
                                    forCharacterRange: m,
                                    actualCharacterRange: nil),
                                in: tc)
                            let font = tv.textStorage?.attribute(
                                .font, at: m.location, effectiveRange: nil)
                                as? NSFont
                            glyph = " glyph=\(g) fragX=\(Int(frag.minX)) locX=\(Int(loc.x)) boxX=\(Int(box.minX)) font=\(font?.fontName ?? "-")"
                        }
                        print("snapshot: ime-key \(step)\(glyph) sel=\(tv.selectedRange().location),\(tv.selectedRange().length) marked=\(marking ? "\(m.location),\(m.length)" : "-") panelX=\(Int(inWin.minX)) panelY=\(Int(win.frame.height - inWin.maxY)) text=\(tv.string.replacingOccurrences(of: "\n", with: "|"))")
                    }
                    for step in script.split(separator: ";") {
                        let parts = step.split(separator: ":", maxSplits: 1)
                        let arg = parts.count > 1 ? String(parts[1]) : ""
                        switch parts.first ?? "" {
                        case "caret":
                            tv.setSelectedRange(NSRange(
                                location: min(Int(arg) ?? 0,
                                              tv.string.utf16.count),
                                length: 0))
                            try? await Task.sleep(for: .milliseconds(150))
                            report(String(step))
                        case "keys":
                            for ch in arg.replacingOccurrences(
                                of: "_", with: " ") {
                                guard let code = codes[ch] else { continue }
                                // Backed by a CGEvent, as the window
                                // server's are: the input method reads it.
                                let src = CGEventSource(
                                    stateID: .hidSystemState)
                                for down in [true, false] {
                                    if let cg = CGEvent(
                                        keyboardEventSource: src,
                                        virtualKey: CGKeyCode(code),
                                        keyDown: down),
                                       let e = NSEvent(cgEvent: cg) {
                                        win.sendEvent(e)
                                    }
                                }
                                try? await Task.sleep(for: .milliseconds(250))
                                report("key \(ch == " " ? "space" : ch == "^" ? "esc" : String(ch))")
                            }
                        default:
                            continue
                        }
                    }
                    TISSelectInputSource(before)
                    try? await Task.sleep(for: .milliseconds(300))
                }
                // A positional tag in the text selected (its editor opens)
                // -- the n-th -- and digits typed into it.
                if let v = env["VALTZ_SNAPSHOT_SELECT_TAG"], let n = Int(v),
                   let win = NSApp.windows.first(where: { $0.isVisible }),
                   let root = win.contentView?.superview,
                   let tv = Self.find(PromptTextView.self, in: root),
                   let storage = tv.textStorage {
                    var at: [Int] = []
                    storage.enumerateAttribute(
                        .attachment,
                        in: NSRange(location: 0, length: storage.length)
                    ) { value, range, _ in
                        if value is RefTagAttachment { at.append(range.location) }
                    }
                    if at.indices.contains(n) {
                        win.makeFirstResponder(tv)
                        tv.setSelectedRange(NSRange(location: at[n], length: 1))
                        let right = tv.selectedRange()
                        try? await Task.sleep(for: .milliseconds(400))
                        FileHandle.standardError.write(Data(
                            "snapshot: tag at=\(at[n]) selection=\(right.location),\(right.length) later=\(tv.selectedRange().location),\(tv.selectedRange().length) tag=\(tv.selectedTag != nil)\n".utf8))
                        if let digits = env["VALTZ_SNAPSHOT_TAG_TYPE"] {
                            for d in digits {
                                tv.insertText(String(d),
                                              replacementRange: tv.selectedRange())
                                try? await Task.sleep(for: .milliseconds(200))
                            }
                        }
                        try? await Task.sleep(for: .milliseconds(300))
                        let pops = NSApp.windows.filter {
                            $0.isVisible && $0.className.contains("Popover")
                        }
                        // The editor drawn by AppKit beside the snapshot
                        // (a popover is a window of its own).
                        if let shot = env["VALTZ_SNAPSHOT"],
                           let v = pops.first?.contentView,
                           let rep = v.bitmapImageRepForCachingDisplay(
                               in: v.bounds) {
                            v.cacheDisplay(in: v.bounds, to: rep)
                            try? rep.representation(using: .png,
                                                    properties: [:])?
                                .write(to: URL(fileURLWithPath: shot
                                    .replacingOccurrences(of: ".png",
                                                          with: "-editor.png")))
                        }
                        FileHandle.standardError.write(Data(
                            "snapshot: tag selected=\(tv.selectedTag.map { $0.tag.tag } ?? "-") editor=\(pops.map { "#\($0.windowNumber)" }.joined(separator: ","))\n".utf8))
                    }
                }
                // Immersive prompt editing (PromptStudio): Markdown drawn
                // or not; the mode entered; prompts opened as tabs
                // ("new:<text>" -- "\n" a line break -- "select:<n>",
                // "close:<n>", from 0); Markdown on the text ("sel:<at>,<n>",
                // "caret:<at>", "bold", "italic", "underline"); and back.
                if let v = env["VALTZ_SNAPSHOT_MARKDOWN"] {
                    m.promptMarkdown = v != "0"
                }
                // The stage's corners: "square" or "round" (View › Round
                // Stage Corners).
                if let v = env["VALTZ_SNAPSHOT_STAGE_CORNERS"] {
                    m.roundStageCorners = v != "square"
                }
                if env["VALTZ_SNAPSHOT_IMMERSIVE"] != nil {
                    withAnimation(AppModel.motion) { m.enterImmersivePrompt() }
                    try? await Task.sleep(for: .milliseconds(700))
                }
                // The Prompt Editor's settings popover ("1"; "custom": with
                // Favor's Custom, Tune's options inside it): how many
                // popovers are open, and the popover drawn to
                // <snapshot>-settings.png.
                if let v = env["VALTZ_SNAPSHOT_EDITOR_SETTINGS"],
                   m.promptImmersive {
                    m.showsGenerationSettings = true
                    try? await Task.sleep(for: .milliseconds(700))
                    if v == "custom" {
                        m.choosePreference(.custom)
                        if !m.showsTuning { m.toggleTuning() }
                        try? await Task.sleep(for: .milliseconds(900))
                    }
                    let pops = NSApp.windows.filter {
                        $0.isVisible && $0.className.contains("Popover")
                    }
                    if let shot = env["VALTZ_SNAPSHOT"],
                       let v = pops.first?.contentView,
                       let rep = v.bitmapImageRepForCachingDisplay(
                           in: v.bounds) {
                        v.cacheDisplay(in: v.bounds, to: rep)
                        try? rep.representation(using: .png, properties: [:])?
                            .write(to: URL(fileURLWithPath: shot
                                .replacingOccurrences(of: ".png",
                                                      with: "-settings.png")))
                    }
                    FileHandle.standardError.write(Data(
                        "snapshot: editor-settings popovers=\(pops.count) tuning=\(m.showsTuning) size=\(pops.first.map { "\(Int($0.frame.width))x\(Int($0.frame.height))" } ?? "-")\n".utf8))
                }
                if let spec = env["VALTZ_SNAPSHOT_PROMPT_TABS"] {
                    for op in spec.split(separator: ";") {
                        let kv = op.split(separator: ":", maxSplits: 1)
                            .map(String.init)
                        let arg = kv.count > 1 ? kv[1] : ""
                        switch kv[0] {
                        case "new":
                            m.newPromptTab()
                            if !arg.isEmpty {
                                m.setPrompt(arg.replacingOccurrences(
                                    of: "\\n", with: "\n"))
                            }
                        case "select":
                            if let n = Int(arg) { m.selectPromptTab(n) }
                        case "close":
                            if let n = Int(arg) { m.requestClosePromptTab(n) }
                        // "open:<asset id>|newest": a text asset selected
                        // in Assets; "type:<text>" typed over the open
                        // tab's words; "retreat" another kind selected;
                        // "shrink" the tab's way back to the box.
                        case "open":
                            let a = arg == "newest"
                                ? m.assets.filter(\.isPrompt)
                                    .max { $0.created < $1.created }
                                : m.assets.first { $0.id == arg }
                            if let a { m.openTextAsset(a) }
                        case "type":
                            m.setPrompt(arg.replacingOccurrences(
                                of: "\\n", with: "\n"))
                        case "retreat": m.retreatFromEditor()
                        case "shrink":
                            if m.promptImmersive && !m.activeTabDoNotApply {
                                m.toggleImmersivePrompt()
                            }
                        default: break
                        }
                        try? await Task.sleep(for: .milliseconds(400))
                        FileHandle.standardError.write(Data(
                            "snapshot: tabs \(op) immersive=\(m.promptImmersive) active=\(m.activePromptTab) tabs=\(m.promptTabs.indices.map { i in "\(i):\(m.tabDoNotApply(i) ? "look" : "edit")\(m.promptTabs[i].readOnly ? "-ro" : "")" }.joined(separator: ",")) shrink=\(m.promptImmersive && !m.activeTabDoNotApply) prompt=\(String(m.prompt.prefix(24)).replacingOccurrences(of: " ", with: "_")) prompts=\(m.assets.filter(\.isPrompt).count)\n".utf8))
                    }
                }
                if let spec = env["VALTZ_SNAPSHOT_MD"],
                   let tv = m.promptTextView {
                    tv.window?.makeFirstResponder(tv)
                    for op in spec.split(separator: ";") {
                        let kv = op.split(separator: ":", maxSplits: 1)
                            .map(String.init)
                        let nums = (kv.count > 1 ? kv[1] : "")
                            .split(separator: ",").compactMap { Int($0) }
                        switch kv[0] {
                        case "sel" where nums.count == 2:
                            tv.setSelectedRange(NSRange(location: nums[0],
                                                        length: nums[1]))
                        case "caret" where nums.count == 1:
                            tv.setSelectedRange(NSRange(location: nums[0],
                                                        length: 0))
                        case "bold": tv.toggleMarkdown(.bold)
                        case "italic": tv.toggleMarkdown(.italic)
                        case "underline": tv.toggleMarkdown(.underline)
                        default: break
                        }
                        try? await Task.sleep(for: .milliseconds(200))
                    }
                    FileHandle.standardError.write(Data(
                        "snapshot: md text=\(tv.string.replacingOccurrences(of: "\n", with: "|")) selection=\(tv.selectedRange().location),\(tv.selectedRange().length)\n".utf8))
                }
                if env["VALTZ_SNAPSHOT_EXIT_IMMERSIVE"] != nil {
                    withAnimation(AppModel.motion) { m.exitImmersivePrompt() }
                    try? await Task.sleep(for: .milliseconds(700))
                }
                // The output size: "portrait", "16:9", "large", or a
                // typed "1200x800" (after a base has brought its own).
                if let o = env["VALTZ_SNAPSHOT_ORIENTATION"],
                   let o = Orientation(rawValue: o) { m.setOrientation(o) }
                if let r = env["VALTZ_SNAPSHOT_RATIO"],
                   let r = AspectRatio(rawValue: r) { m.aspectRatio = r }
                if let c = env["VALTZ_SNAPSHOT_SIZE_CLASS"],
                   let c = SizeClass.allCases.first(where: {
                       "\($0)" == c
                   }) { m.sizeChoice = c }
                if let t = env["VALTZ_SNAPSHOT_OUTPUT_SIZE"] {
                    try? await Task.sleep(for: .milliseconds(300))
                    m.setCustomSize(t)
                }
                // Clicks on the pencil badges of the prompt row's pictures
                // ("0,1": the first, then the second).
                if let list = env["VALTZ_SNAPSHOT_CLICK_BADGE"] {
                    for n in list.split(separator: ",").compactMap({ Int($0) }) {
                        try? await Task.sleep(for: .milliseconds(400))
                        m.toggleBase(picture: n)
                    }
                    try? await Task.sleep(for: .milliseconds(400))
                }
                // The assistant's button with the box empty: the model's
                // prompt template, written for the row ("snapshot: outline
                // model= enabled= lines= text=", its line breaks as ⏎).
                if env["VALTZ_SNAPSHOT_OUTLINE"] != nil {
                    try? await Task.sleep(for: .milliseconds(400))
                    let enabled = m.assistantEnabled
                    m.assistantPressed()
                    try? await Task.sleep(for: .milliseconds(400))
                    let lines = m.prompt.split(separator: "\n",
                                               omittingEmptySubsequences: false)
                    FileHandle.standardError.write(Data(
                        "snapshot: outline model=\(m.outlineModel?.id ?? "-") enabled=\(enabled) lines=\(lines.count) text=\(m.prompt.replacingOccurrences(of: "\n", with: "⏎"))\n".utf8))
                }
                // The assistant's rewrite: "show" waits for the
                // suggestion; "accept" also takes it.
                if let e = env["VALTZ_SNAPSHOT_ENHANCE"] {
                    try? await Task.sleep(for: .milliseconds(400))
                    m.enhance()
                    // "stream": the suggestion caught while it is being
                    // written (160 characters in) -- <snapshot>-stream.png,
                    // drawn by AppKit -- then waited for as "show" is;
                    // "stream-reject": Reject pressed right then.
                    if e.hasPrefix("stream") {
                        for _ in 0..<Int(delay * 20)
                        where m.enhanceDraft.count < 160 && m.enhanced == nil {
                            try? await Task.sleep(for: .milliseconds(50))
                        }
                        let n = m.enhanceDraft.count
                        let writing = m.suggestionWriting
                        Self.drawWindow(to: path.replacingOccurrences(
                            of: ".png", with: "-stream.png"))
                        FileHandle.standardError.write(Data(
                            "snapshot: stream draft=\(n) writing=\(writing)\n".utf8))
                        if e == "stream-reject" {
                            m.dismissEnhanced()
                            try? await Task.sleep(for: .seconds(2))
                            FileHandle.standardError.write(Data(
                                "snapshot: rejected job=\(m.enhanceJob ?? "-") suggesting=\(m.suggesting) jobs=\(m.jobs.values.map(\.state).sorted())\n".utf8))
                        }
                    }
                    for _ in 0..<Int(delay * 4) where m.enhanced == nil {
                        try? await Task.sleep(for: .milliseconds(250))
                    }
                    if e == "accept" {
                        m.acceptEnhanced()
                        try? await Task.sleep(for: .milliseconds(600))
                    }
                }
                // The suggestion beside the prompt, in the Prompt Editor:
                // words typed at its end ("type:<text>"), then "apply" or
                // "reject" -- its half closes.
                if let spec = env["VALTZ_SNAPSHOT_SUGGESTION"],
                   let win = NSApp.windows.first(where: { $0.isVisible }),
                   let root = win.contentView?.superview {
                    try? await Task.sleep(for: .milliseconds(800))
                    for op in spec.split(separator: ";") {
                        let kv = op.split(separator: ":", maxSplits: 1)
                            .map(String.init)
                        switch kv[0] {
                        case "type":
                            if let tv = Self.find(SuggestionTextView.self,
                                                  in: root) {
                                win.makeFirstResponder(tv)
                                tv.setSelectedRange(NSRange(
                                    location: tv.string.utf16.count,
                                    length: 0))
                                tv.insertText(kv.count > 1 ? kv[1] : "",
                                              replacementRange:
                                                  tv.selectedRange())
                            }
                        case "apply": m.acceptEnhanced()
                        case "reject": m.dismissEnhanced()
                        default: break
                        }
                        try? await Task.sleep(for: .milliseconds(600))
                    }
                }
                if env["VALTZ_SNAPSHOT_DRAWER"] != nil {
                    withAnimation(AppModel.motion) { m.openPanel = .generate }
                }
                // An asset on the simple stage as if just made: its id, or
                // "newest" / "newest-video" / "newest-image" /
                // "newest-audio".
                // The project set up from the inspector: "still:1024x768",
                // "timeline:832x480", "sound"; its output after a third
                // field, "timeline:832x480:rec2020,30,1,44100" (colour, fps,
                // channels, sample rate).
                if let spec = env["VALTZ_SNAPSHOT_NEW_PROJECT"] {
                    let p = spec.split(separator: ":").map(String.init)
                    let wh = (p.count > 1 ? p[1] : "").split(separator: "x")
                        .compactMap { Int($0) }
                    var o = OutputSettingsDTO()
                    if p.count > 2 {
                        let f = p[2].split(separator: ",").map(String.init)
                        if !f.isEmpty { o.color = f[0] }
                        if f.count > 1, let n = Int64(f[1]) { o.fps = [n, 1] }
                        if f.count > 2, let c = Int(f[2]) { o.channels = c }
                        if f.count > 3, let r = Int(f[3]) { o.sampleRate = r }
                    }
                    m.setUpProject(p[0] == "still" ? .still
                                   : p[0] == "sound" ? .sound : .timeline,
                                   width: wh.first ?? 1024,
                                   height: wh.last ?? 1024, output: o)
                    try? await Task.sleep(for: .milliseconds(600))
                }
                if let which = env["VALTZ_SNAPSHOT_STAGE_ASSET"] {
                    let made = m.assets.filter { $0.head > 0 && !$0.isDrawn }
                    let a: AssetDTO? = switch which {
                    case "newest": made.last
                    case "newest-video": made.last { $0.kind == "video" }
                    case "newest-image": made.last { $0.kind == "image" }
                    case "newest-audio": made.last { $0.kind == "audio" }
                    case "project": m.projectViews.first
                    default: m.assets.first { $0.id == which }
                    }
                    if let a { m.putOnStage(a) }
                    try? await Task.sleep(for: .milliseconds(500))
                    // A frame grabbed where the player is (after a seek):
                    // its Grab Frame under the stage.
                    if env["VALTZ_SNAPSHOT_GRAB"] != nil {
                        if let f = env["VALTZ_SNAPSHOT_SEEK"].flatMap({
                            Int($0) }) {
                            m.seekVideo(to: f)
                            try? await Task.sleep(for: .milliseconds(600))
                        }
                        m.grabFrame()
                        print("snapshot: grab frame=\(m.videoFrame) refs=\(m.promptAttachments.count)")
                        try? await Task.sleep(for: .milliseconds(600))
                    }
                    // Continue's menu, opened under its button: its items.
                    if env["VALTZ_SNAPSHOT_CONTINUE_MENU"] != nil {
                        m.continueMenuRequest += 1
                        try? await Task.sleep(for: .milliseconds(800))
                        let items = ClipBarMenu.opened?.items.map {
                            $0.title + ($0.subtitle.map { " [\($0)]" } ?? "")
                        } ?? []
                        print("snapshot: continue-menu \(items.joined(separator: " | "))")
                        ClipBarMenu.opened?.cancelTracking()
                    }
                    // The clip carried on: its Continue under the stage --
                    // from its last N seconds (a guide), or "1" as it is.
                    if let c = env["VALTZ_SNAPSHOT_CONTINUE"] {
                        if let s = Int(c), s >= 2 {
                            m.continueClip(seconds: s)
                        } else {
                            m.continueClip()
                        }
                        try? await Task.sleep(for: .milliseconds(400))
                    }
                    // ...and dragged from the stage into the prompt, as the
                    // picture to edit.
                    // What the stage shows, captured, as the drag does.
                    if env["VALTZ_SNAPSHOT_EDIT_STAGE"] != nil {
                        m.addStageToPrompt()
                        try? await Task.sleep(for: .milliseconds(800))
                    }
                }
                // The prompt's Clear button.
                if env["VALTZ_SNAPSHOT_CLEAR_PROMPT"] != nil {
                    try? await Task.sleep(for: .milliseconds(600))
                    m.clearPrompt()
                    try? await Task.sleep(for: .milliseconds(600))
                }
                // The inspector stacked ("1") -- before the asset list's
                // actions, so they play in it -- its sections folded
                // ("info,assets"; neither saved by a snapshot run).
                if let m = model, env["VALTZ_SNAPSHOT_INSPECTOR_STACKED"] == "1" {
                    m.inspectorStacked = true
                    m.inspectorFolded = Set(
                        (env["VALTZ_SNAPSHOT_INSPECTOR_FOLD"] ?? "")
                            .split(separator: ",")
                            .compactMap { InspectorTab(rawValue: String($0)) })
                    // Their shares ("layers:2,assets:1"; none: even).
                    m.inspectorWeights = Dictionary(uniqueKeysWithValues:
                        (env["VALTZ_SNAPSHOT_INSPECTOR_WEIGHTS"] ?? "")
                            .split(separator: ",").compactMap { kv in
                                let p = kv.split(separator: ":")
                                guard p.count == 2,
                                      let t = InspectorTab(rawValue: String(p[0])),
                                      let w = Double(p[1]) else { return nil }
                                return (t, w)
                            })
                }
                // The asset list's actions, in turn: "prompt:<id>" (into
                // the prompt's row), "stage:<id>" (dropped on the stage),
                // "capture:all|selected", "folder:<name>" (a new one),
                // "move:<id>:<folder name>", "remove:<id>", "remove-stale".
                if let spec = env["VALTZ_SNAPSHOT_ASSETS"] {
                    for step in spec.split(separator: ";") {
                        try? await Task.sleep(for: .milliseconds(600))
                        let p = step.split(separator: ":").map(String.init)
                        var arg = p.count > 1 ? p[1] : ""
                        // "newest-capture": the last capture made.
                        if arg == "newest-capture",
                           let c = m.assets.filter(\.isCapture)
                               .max(by: { $0.created < $1.created }) {
                            arg = c.id
                        }
                        switch p[0] {
                        case "prompt": m.addAssetReferences([arg])
                        case "active":
                            if let a = m.assets.first(where: { $0.id == arg }) {
                                m.setActive(a)
                            }
                        case "stage": m.dropAssetOnStage(arg)
                        // A single click on a row: the asset VIEWED on
                        // the stage ("view:<id>"); "unview" the list
                        // losing focus -- the active one back.
                        case "view":
                            if let a = m.assets.first(where: { $0.id == arg }) {
                                m.viewAsset(a)
                            }
                        case "unview": m.endViewing()
                        // A blank asset of the New menu ("new:timeline",
                        // "new:still", "new:sound"), made active.
                        case "new":
                            m.newBlankAsset(arg == "still" ? .still
                                            : arg == "sound" ? .sound
                                            : .timeline)
                        // Sound recorded into the blank sound on the stage
                        // ("record:system", "record:mic" -- the first
                        // microphone -- or a source's id), "stop".
                        case "record":
                            m.refreshCaptureSources()
                            let want = p.dropFirst().joined(separator: ":")
                            if let s = m.captureSources.first(where: {
                                $0.id == want
                                    || (want == "mic" && $0.kind == "microphone")
                            }) {
                                m.captureSource = s.id
                            }
                            m.startCapture()
                            FileHandle.standardError.write(Data(
                                "snapshot: record source=\(m.captureSource)\n".utf8))
                        case "wait":
                            try? await Task.sleep(
                                for: .seconds(Double(arg) ?? 1))
                            FileHandle.standardError.write(Data(
                                "snapshot: capture recording=\(m.capture.recording) seconds=\(String(format: "%.1f", m.capture.seconds)) level=\(String(format: "%.2f", m.capture.level))\n".utf8))
                        case "stop": m.stopCapture()
                        case "capture":
                            m.captureStage(selectedOnly: arg == "selected")
                        case "folder":
                            if let id = m.createFolder() {
                                m.renameFolder(id, arg)
                            }
                        case "move" where p.count > 2:
                            let f = m.assetFolders.first { $0.name == p[2] }
                            m.moveAsset(arg, to: f?.id ?? "")
                        // "rename:<id>:<name>"; "renaming:<id>" its field
                        // open, as the row's Rename opens it.
                        case "rename" where p.count > 2:
                            m.renameAsset(arg, p[2...].joined(separator: ":"))
                        case "renaming": m.renamingAsset = arg
                        // "fold:<folder name>" -- folded, or open again.
                        case "fold":
                            if let f = m.assetFolders.first(where: {
                                $0.name == arg }) {
                                m.toggleFolder(f.id)
                            }
                        case "remove": m.removeAsset(arg)
                        case "remove-stale": m.removeStaleAssets()
                        default: break
                        }
                    }
                    try? await Task.sleep(for: .milliseconds(800))
                }
                // The inspector's section ("info", "layers"), and layer work on
                // the picture on the stage: "add;source=<asset>;hide=<layer>;
                // select=<layer>;move=<layer>:<by>" ("0" is layer 0).
                if let m = model, let t = env["VALTZ_SNAPSHOT_INSPECTOR_TAB"],
                   let tab = InspectorTab(rawValue: t == "history" ? "assets" : t) {
                    m.revealInspector(tab)
                }
                // Canvas Size applied ("1200x700:tl"; the anchor tl, t,
                // tr, l, c, r, bl, b, br; "own": Own Size) and a clip's
                // Length (frames; 0 its own) -- as the popover and the
                // field set them.
                if let m = model, let spec = env["VALTZ_SNAPSHOT_CANVAS"],
                   let a = m.stageStack {
                    let p = spec.split(separator: ":").map(String.init)
                    let wh = p[0].split(separator: "x").compactMap { Int($0) }
                    let anchors = ["tl": (0.0, 0.0), "t": (0.5, 0.0),
                                   "tr": (1.0, 0.0), "l": (0.0, 0.5),
                                   "c": (0.5, 0.5), "r": (1.0, 0.5),
                                   "bl": (0.0, 1.0), "b": (0.5, 1.0),
                                   "br": (1.0, 1.0)]
                    let (ax, ay) = anchors[p.count > 1 ? p[1] : "c"]
                        ?? (0.5, 0.5)
                    if spec == "own" {
                        m.resetCanvas(a)
                    } else if wh.count == 2 {
                        m.setCanvas(a, width: wh[0], height: wh[1],
                                    anchorX: ax, anchorY: ay)
                    }
                    try? await Task.sleep(for: .seconds(1.5))
                }
                if let m = model, let f = env["VALTZ_SNAPSHOT_TIMELINE"]
                    .flatMap({ Int($0) }), let a = m.stageStack {
                    m.setTimeline(a, frames: f)
                    try? await Task.sleep(for: .seconds(1.5))
                }
                // Information › Canvas › Change…, open.
                if let m = model, env["VALTZ_SNAPSHOT_CANVAS_SIZE"] != nil {
                    m.revealInspector(.info)
                    try? await Task.sleep(for: .milliseconds(500))
                    m.showsCanvasSize = true
                }
                if let m = model, let spec = env["VALTZ_SNAPSHOT_LAYER_OPS"] {
                    for step in spec.split(separator: ";") {
                        let kv = step.split(separator: "=", maxSplits: 1)
                            .map(String.init)
                        let arg = kv.count > 1 ? kv[1] : ""
                        switch kv[0] {
                        case "add": m.addLayer()
                        case "source":
                            if let core = m.core, let pid = m.projectId,
                               let pic = m.stageStack {
                                _ = core.layerOp(project: pid, asset: pic.id,
                                                 "source", layer: m.activeLayer,
                                                 extra: ["source": arg])
                                m.layersChanged()
                            }
                        // A file dropped on the selected layer's row.
                        case "drop":
                            m.setLayerSource(
                                m.activeLayer,
                                from: URL(fileURLWithPath: arg))
                            try? await Task.sleep(for: .seconds(2))
                        // The minus button on that layer ("0": layer 0).
                        case "remove": m.removeLayer(Self.layerId(arg))
                        case "hide": m.setLayerVisible(Self.layerId(arg), false)
                        case "show": m.setLayerVisible(Self.layerId(arg), true)
                        case "select": m.selectLayer(Self.layerId(arg))
                        case "move":
                            let p = arg.split(separator: ":").map(String.init)
                            if p.count == 2, let by = Int(p[1]) {
                                m.moveLayer(Self.layerId(p[0]), by: by)
                            }
                        // Its layers in its place (a layer showing a
                        // composition).
                        case "decompose":
                            m.decomposeLayer(Self.layerId(arg))
                        // A still's pages (DESIGN §6a): one added after
                        // the page shown, that page removed, the stage at
                        // a page ("page=2", from 1), the selected layer's
                        // pages ("pages=this|from|every").
                        case "page-add": m.addPage()
                        case "page-remove": m.removePage()
                        case "page":
                            if let n = Int(arg) { m.goToPage(n - 1) }
                        case "pages":
                            m.setLayerPages(m.activeLayer,
                                            arg == "every" ? .every
                                            : arg == "from" ? .fromHere
                                            : .thisPage)
                        // A key toggled on the selected layer's track at
                        // the page or frame shown ("key=place").
                        case "key":
                            m.toggleKey(arg == "adjust" ? .adjust
                                        : arg == "turn" ? .turn : .place)
                        // The Crop panel's values ("crop=offset_x:0.3").
                        case "crop":
                            let p = arg.split(separator: ":")
                            if p.count == 2, let v = Double(p[1]) {
                                m.setCropValues([String(p[0]): v])
                            }
                        case "adjust":
                            let p = arg.split(separator: ":")
                            if p.count == 2, let v = Double(p[1]),
                               let k = ImageAdjustments.Key(
                                   rawValue: String(p[0])) {
                                m.setAdjustment(k, v)
                            }
                        // An asset of the list put on the stage: a new
                        // layer ("newest-image", or an id).
                        case "place":
                            let id = arg == "newest-image"
                                ? m.assets.filter {
                                    $0.kind == "image" && !$0.isDrawn
                                }.max { $0.created < $1.created }?.id
                                : arg
                            if let id { m.dropAssetOnStage(id) }
                        default: break
                        }
                        try? await Task.sleep(for: .milliseconds(400))
                    }
                }
                // Clicks posted to the window ("x,y" in points from its top
                // left; ";" between), as the pointer's would come: each
                // prints what has the focus after it.
                if let m = model, let spec = env["VALTZ_SNAPSHOT_CLICKS"] {
                    for c in spec.split(separator: ";") {
                        let xy = c.split(separator: ",").compactMap {
                            Double($0)
                        }
                        guard xy.count == 2 else { continue }
                        Self.postClick(x: xy[0], y: xy[1])
                        try? await Task.sleep(for: .milliseconds(500))
                        m.refreshTextUndo()
                        let fr = NSApp.windows.first { $0.isVisible }?
                            .firstResponder
                        FileHandle.standardError.write(Data(
                            "snapshot: click \(Int(xy[0])),\(Int(xy[1])) focus=\(fr.map { String(describing: type(of: $0)) } ?? "-") undo=\(m.undoMenuTitle.replacingOccurrences(of: " ", with: "_"))\n".utf8))
                    }
                }
                // Drags posted to the window ("x,y>x2,y2" in points from
                // its top left; ";" between), as the pointer's would come:
                // each prints the stacked inspector's shares after it.
                if let m = model, let spec = env["VALTZ_SNAPSHOT_DRAGS"] {
                    for d in spec.split(separator: ";") {
                        let ends = d.split(separator: ">").map {
                            $0.split(separator: ",").compactMap { Double($0) }
                        }
                        guard ends.count == 2, ends[0].count == 2,
                              ends[1].count == 2 else { continue }
                        await Self.postDrag(from: (ends[0][0], ends[0][1]),
                                            to: (ends[1][0], ends[1][1]))
                        try? await Task.sleep(for: .milliseconds(500))
                        FileHandle.standardError.write(Data(
                            "snapshot: drag \(d) panels=\(Self.panelSummary(m))\n".utf8))
                    }
                }
                // The project as a document (DESIGN §5b), in turn: "undo",
                // "redo", "save", "saveas:<path>", "revert" -- each prints
                // `snapshot: edit <step> dirty= undo= redo=`.
                if let m = model, let spec = env["VALTZ_SNAPSHOT_EDIT"] {
                    for step in spec.split(separator: ";").map(String.init) {
                        try? await Task.sleep(for: .milliseconds(600))
                        if step == "undo" {
                            m.undoProject()
                        } else if step == "redo" {
                            m.redoProject()
                        } else if step == "save" {
                            // Untitled, Save is Save As: its panel would
                            // wait for a person.
                            if !m.document.untitled && !m.isAnonymous {
                                m.saveDocument()
                            }
                        } else if step.hasPrefix("saveas:") {
                            m.save(as: URL(fileURLWithPath: String(
                                step.dropFirst("saveas:".count))))
                        } else if step == "revert" {
                            m.revertDocument(confirm: false)
                        }
                        try? await Task.sleep(for: .milliseconds(400))
                        FileHandle.standardError.write(Data(
                            "snapshot: edit \(step) dirty=\(m.document.dirty) undo=\(m.document.undo ?? "-") redo=\(m.document.redo ?? "-") title=\(m.windowTitle.replacingOccurrences(of: " ", with: "_"))\n".utf8))
                    }
                }
                // A file dropped on the stage: instantiated at the
                // selected layer.
                if let m = model, let f = env["VALTZ_SNAPSHOT_STAGE_DROP"] {
                    _ = m.dropFilesOnStage([URL(fileURLWithPath: f)])
                    try? await Task.sleep(for: .seconds(2))
                }
                // Markup, through the pointer as the stage sends it
                // (canvas pixels): "tool=rect|color=#ff0000ff|drag=10,10;
                // 200,120|click=50,50|key=delete|text=Hi|pixels|..."
                if let m = model, let spec = env["VALTZ_SNAPSHOT_MARKUP_OPS"] {
                    m.markupOpen = true
                    try? await Task.sleep(for: .milliseconds(300))
                    for step in spec.split(separator: "|") {
                        await ValtzApp.markupStep(m, String(step))
                        try? await Task.sleep(for: .milliseconds(450))
                    }
                }
                if let panel = env["VALTZ_SNAPSHOT_PANEL"] {
                    withAnimation(AppModel.motion) {
                        m.openPanel = panel == "adjust" ? .adjust
                            : panel == "crop" ? .crop
                            : panel == "trim" ? .trim : .generate
                    }
                }
                if env["VALTZ_SNAPSHOT_NAV"] != nil { m.toggleNav() }
                // Adjustments on the picture to edit, before Start
                // ("exposure=0.4,vibrance=0.3"): the model gets them.
                if let spec = env["VALTZ_SNAPSHOT_BASE_ADJUST"] {
                    for _ in 0..<40 where !m.adjustsBase {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                    m.adjustments = Self.adjustments(spec)
                    withAnimation(AppModel.motion) { m.openPanel = .adjust }
                    try? await Task.sleep(for: .seconds(1))
                }
                // The Crop panel: values on the picture on the stage
                // ("scale=0.7,rotate=10,pad_a=0"), editing, the guides; a
                // clip's marks ("12,35") and where its player is.
                if let spec = env["VALTZ_SNAPSHOT_CROP"] {
                    for _ in 0..<40 where !m.canAdjust {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                    var kv: [String: Double] = [:]
                    for (k, v) in Self.adjustments(spec).json { kv[k] = v }
                    for part in spec.split(separator: ",") {
                        let p = part.split(separator: "=")
                        if p.count == 2, let v = Double(p[1]) {
                            kv[String(p[0])] = v
                        }
                    }
                    m.setCropValues(kv)
                }
                if env["VALTZ_SNAPSHOT_CROP_EDIT"] != nil { m.cropEditing = true }
                if let g = env["VALTZ_SNAPSHOT_GUIDES"] {
                    m.showsGuides = g != "0"
                }
                // A sound's marks typed as times ("1.25,0:03.5"), as its
                // Trim panel's fields take them.
                if let t = env["VALTZ_SNAPSHOT_TRIM_TIMES"] {
                    let parts = t.split(separator: ",").map(String.init)
                    if parts.count == 2 {
                        m.setMark(in: true, typed: parts[0])
                        m.setMark(in: false, typed: parts[1])
                    }
                }
                // The selected layer's start on the timeline, a frame.
                if let t = env["VALTZ_SNAPSHOT_TRIM_START"], let f = Int(t) {
                    m.trimOffset = f
                    m.persistTrim()
                    try? await Task.sleep(for: .milliseconds(600))
                }
                // ... or typed as its field takes it ("00:00:01:12", "#36",
                // "1.5").
                if let t = env["VALTZ_SNAPSHOT_TRIM_START_TEXT"] {
                    let took = m.setStart(typed: t)
                    m.persistTrim()
                    FileHandle.standardError.write(Data(
                        "snapshot: start typed=\(t) took=\(took) offset=\(m.trimOffset)\n".utf8))
                    try? await Task.sleep(for: .milliseconds(600))
                }
                // Its speed and sound, as the Trim panel's rows set them:
                // "2", "50%"; "volume=0.5,pitch=-2,follow=1".
                if let t = env["VALTZ_SNAPSHOT_SPEED"],
                   let v = SpeedRow.parse(t) {
                    m.setLayerSpeed(v)
                    m.persistTrim()
                    try? await Task.sleep(for: .milliseconds(600))
                }
                if let spec = env["VALTZ_SNAPSHOT_SOUND"] {
                    for part in spec.split(separator: ",") {
                        let p = part.split(separator: "=")
                        guard p.count == 2, let v = Double(p[1]) else {
                            continue
                        }
                        switch p[0] {
                        case "volume": m.setLayerVolume(v)
                        case "pitch": m.setLayerPitch(v)
                        case "follow": m.pitchFollowsSpeed = v != 0
                        default: break
                        }
                    }
                    m.persistTrim()
                    try? await Task.sleep(for: .milliseconds(600))
                }
                if let t = env["VALTZ_SNAPSHOT_TRIM"] {
                    let f = t.split(separator: ",").compactMap { Int($0) }
                    if f.count == 2 {
                        m.seekVideo(to: f[0])
                        try? await Task.sleep(for: .milliseconds(600))
                        m.videoFrame = f[0]
                        m.setMark(in: true)
                        // The mark-in moved the clip on the timeline.
                        try? await Task.sleep(for: .milliseconds(600))
                        let out = m.timelineFrame(ofSource: f[1])
                        m.seekVideo(to: out)
                        try? await Task.sleep(for: .milliseconds(600))
                        m.videoFrame = out
                        m.setMark(in: false)
                    }
                }
                // The Trim panel's marks set where the player is, and its
                // transport: "seek:2500;in;play;wait:1;pause;out", each
                // printing where the player is on the timeline and in
                // the clip.
                if let spec = env["VALTZ_SNAPSHOT_MARKS"] {
                    for step in spec.split(separator: ";").map(String.init) {
                        let kv = step.split(separator: ":").map(String.init)
                        switch kv[0] {
                        case "seek":
                            m.seekVideo(to: Int(kv.last ?? "") ?? 0)
                        case "in", "out":
                            m.setMark(in: kv[0] == "in")
                        case "play": m.playVideo(rate: 1)
                        case "pause": m.playVideo(rate: 0)
                        default: break
                        }
                        let wait = kv[0] == "wait"
                            ? Double(kv.last ?? "") ?? 1 : 0.8
                        try? await Task.sleep(for: .seconds(wait))
                        print("snapshot: mark \(step) frame=\(m.videoFrame) "
                              + "clip=\(m.sourceFrameAtPlayhead) "
                              + "in=\(m.trim.markIn ?? -1) "
                              + "out=\(m.trim.markOut ?? -1) "
                              + "rate=\(m.videoRate) "
                              + "stage=\(m.currentClip?.name ?? "-")")
                    }
                }
                // A clip's keys: "adjust:55:exposure=1.5;crop:55:scale=0.6;
                // rotate:55:rotate=20".
                if let spec = env["VALTZ_SNAPSHOT_KEYS"] {
                    for part in spec.split(separator: ";") {
                        let f = part.split(separator: ":", maxSplits: 2)
                        guard f.count == 3, let frame = Int(f[1]) else {
                            continue
                        }
                        var kv: [String: Double] = [:]
                        for item in f[2].split(separator: ",") {
                            let p = item.split(separator: "=")
                            if p.count == 2, let v = Double(p[1]) {
                                kv[String(p[0])] = v
                            }
                        }
                        m.setKey(f[0] == "crop" ? .place
                                     : f[0] == "rotate" ? .turn : .adjust,
                                 at: frame, kv)
                    }
                }
                // Render Upscaled (the Crop panel's), once the stage's layer
                // is scaled up: whether it is offered, its size, the job.
                if env["VALTZ_SNAPSHOT_UPSCALE"] == "1" {
                    try? await Task.sleep(for: .milliseconds(400))
                    // A composition: Flatten First, then upscale.
                    if m.upscaleNeedsFlatten {
                        m.flattenLayerForUpscale()
                        for _ in 0..<600 where m.flatteningLayer {
                            try? await Task.sleep(for: .milliseconds(100))
                        }
                        try? await Task.sleep(for: .milliseconds(400))
                        print("snapshot: upscale flattened source=\(m.activeLayerSourceName)")
                    }
                    let can = m.canUpscaleLayer
                    let size = m.upscaleTarget.map { "\($0.width)x\($0.height)" }
                        ?? "-"
                    if can { m.upscaleLayer() }
                    print("snapshot: upscale can=\(can) size=\(size) job=\(m.upscaleJob ?? "-")")
                }
                if let s = env["VALTZ_SNAPSHOT_SEEK"], let n = Int(s) {
                    m.seekVideo(to: n)
                    try? await Task.sleep(for: .milliseconds(400))
                }
                // The Trim panel's transport: a rate ("-1", "2"; 0 is
                // pause), or a step ("-1", "3").
                if let s = env["VALTZ_SNAPSHOT_PLAY"], let r = Float(s) {
                    m.playVideo(rate: r)
                }
                if let s = env["VALTZ_SNAPSHOT_STEP"], let n = Int(s) {
                    m.stepVideo(n)
                }
                // The player's floating play button: the player played
                // directly, as the button does (not the Trim panel's
                // transport); where it is 0.4 s later is printed.
                if env["VALTZ_SNAPSHOT_PLAYER_PLAY"] != nil,
                   let win = NSApp.windows.first(where: { $0.isVisible }),
                   let root = win.contentView?.superview,
                   let pv = Self.find(AVPlayerView.self, in: root) {
                    let from = m.videoFrame
                    pv.player?.play()
                    try? await Task.sleep(for: .milliseconds(400))
                    FileHandle.standardError.write(Data(
                        "snapshot: player-play from=\(from) now=\(m.videoFrame) rate=\(m.videoRate)\n".utf8))
                }
                // Frame N of the clip as its player draws it, with its
                // look: "30:/tmp/frame.png".
                if let spec = env["VALTZ_SNAPSHOT_LOOK_FRAME"],
                   let colon = spec.firstIndex(of: ":"),
                   let n = Int(spec[..<colon]), let core = m.core {
                    let out = URL(fileURLWithPath:
                        String(spec[spec.index(after: colon)...]))
                    // A composition's frame as its plan draws it; a clip's
                    // through its look.
                    if let plan = m.stackPlayback, !plan.soundOnly,
                       let img = core.stackStill(plan: plan.plan, frame: n) {
                        _ = AppModel.writePNG(img, to: out)
                    } else if let url = m.stage.video,
                              let look = m.stageClipLook,
                              let img = await VideoPlayerView.frame(
                                  url, n, look: look, core: core) {
                        _ = AppModel.writePNG(img, to: out)
                    }
                }
                // The attach menu's captures, in turn: "mic" / "system"
                // (a sound recorded), "camera", "card:sound|camera" (the
                // card shown alone), "wait:<s>", "stop" (the
                // sound attached), "snap", "record", "stop-video",
                // "cancel" -- each printing `snapshot: attach <step>
                // capture= recording= seconds= frames= refs=`.
                if let spec = env["VALTZ_SNAPSHOT_ATTACH_CAPTURE"] {
                    for step in spec.split(separator: ";").map(String.init) {
                        let kv = step.split(separator: ":").map(String.init)
                        switch kv[0] {
                        case "mic", "system":
                            m.attachSound(system: kv[0] == "system")
                        case "camera": m.attachCamera()
                        // "card:sound" / "card:camera": the card alone, as
                        // a recording or the camera shows it (no device:
                        // what it looks like).
                        case "card":
                            if kv.count > 1 && kv[1] == "camera" {
                                m.camera = CameraStateDTO(
                                    on: true, name: "MacBook Pro Camera",
                                    width: 1920, height: 1080)
                                m.attachCapture = .camera
                            } else {
                                m.capture = CaptureStateDTO(
                                    recording: true, name: "System Audio",
                                    seconds: 7.4, level: 0.6)
                                m.attachCapture = .sound
                            }
                        case "wait":
                            try? await Task.sleep(for: .seconds(
                                Double(kv.count > 1 ? kv[1] : "1") ?? 1))
                        // "land:<asset id>": that asset as if just
                        // captured -- the card turning into its thumbnail.
                        case "land" where kv.count > 1:
                            m.capture = CaptureStateDTO()
                            m.camera = CameraStateDTO()
                            m.landCapture(kv[1])
                        case "stop": m.finishAttachSound()
                        case "snap": m.cameraSnap()
                        case "record": m.cameraRecord()
                        case "stop-video": m.cameraStopRecording()
                        case "cancel":
                            if m.attachCapture == .camera {
                                m.closeCamera()
                            } else {
                                m.cancelAttachSound()
                            }
                        default: break
                        }
                        try? await Task.sleep(for: .milliseconds(1200))
                        FileHandle.standardError.write(Data(
                            "snapshot: attach \(step) capture=\(m.attachCapture.map { "\($0)" } ?? "-") recording=\(m.capture.recording || m.camera.recording) seconds=\(String(format: "%.1f", max(m.capture.seconds, m.camera.seconds))) frames=\(m.camera.frames) refs=\(m.promptAttachments.map { $0.kind + ":" + $0.url.lastPathComponent }.joined(separator: ",").replacingOccurrences(of: " ", with: "_"))\n".utf8))
                    }
                }
                // File › New Anonymous Project.
                if env["VALTZ_SNAPSHOT_NEW_ANONYMOUS"] != nil {
                    m.newAnonymousProject()
                    try? await Task.sleep(for: .milliseconds(800))
                    FileHandle.standardError.write(Data(
                        "snapshot: new-anonymous anonymous=\(m.isAnonymous) project=\(m.projectName.replacingOccurrences(of: " ", with: "_")) assets=\(m.assets.count)\n".utf8))
                }
                if env["VALTZ_SNAPSHOT_START"] != nil { m.start() }
                // The TASK QUEUE (DESIGN §3a), in turn: "prompt:<text>",
                // "start", "wait:<s>", "watch:<n>" (the task at T<n>, as a
                // click on its row), "view:<asset id>|newest-image",
                // "active:project", "cancel:<n>", "undo", "compare" (its
                // button), "export:<format>:<file>", "idle:<s>" (until
                // no task is left, at most s) -- each printing `snapshot:
                // task <step> tasks= watched= stage=`.
                if let spec = env["VALTZ_SNAPSHOT_TASKS"] {
                    for step in spec.split(separator: ";").map(String.init) {
                        let kv = step.split(separator: ":", maxSplits: 1)
                            .map(String.init)
                        let arg = kv.count > 1 ? kv[1] : ""
                        switch kv[0] {
                        case "prompt": m.setPrompt(arg)
                        case "start": m.start()
                        case "wait":
                            try? await Task.sleep(
                                for: .seconds(Double(arg) ?? 1))
                        case "watch":
                            if let t = m.tasks.first(where: {
                                $0.position == Int(arg) }) {
                                m.watchTask(t.job)
                            }
                        case "view":
                            let a = arg == "newest-image"
                                ? m.assets.filter { $0.kind == "image"
                                    && $0.head > 0 && $0.op != "project" }
                                    .max { $0.created < $1.created }
                                : m.assets.first { $0.id == arg }
                            if let a { m.viewAsset(a) }
                        case "active":
                            if let a = m.projectViews.first { m.setActive(a) }
                        case "cancel":
                            if let t = m.tasks.first(where: {
                                $0.position == Int(arg) }) {
                                m.cancel(job: t.job)
                            }
                        case "undo": m.undoProject()
                        // The title bar's compare button.
                        case "compare": m.toggleCompare()
                        // "export:<format>:<file>": what the stage shows,
                        // saved as Save… would.
                        case "export":
                            let fp = arg.split(separator: ":", maxSplits: 1)
                                .map(String.init)
                            if fp.count == 2,
                               let c = ExportChoice(rawValue: fp[0]) {
                                m.save(to: URL(fileURLWithPath: fp[1]), as: c)
                            }
                        case "idle":
                            for _ in 0..<Int((Double(arg) ?? 60) * 4)
                            where !m.tasks.isEmpty {
                                try? await Task.sleep(for: .milliseconds(250))
                            }
                        default: break
                        }
                        try? await Task.sleep(for: .milliseconds(400))
                        FileHandle.standardError.write(Data(
                            "snapshot: task \(step.replacingOccurrences(of: " ", with: "_")) tasks=\(Self.taskSummary(m)) compareOn=\(m.compareOn) canCompare=\(m.canToggleCompare) activeComp=\(m.activeComposition.map { "\($0.name.replacingOccurrences(of: " ", with: "_")):\($0.layerStack.count)layers" } ?? "-") watched=\(m.tasks.first { $0.job == m.generationJob }?.badge ?? (m.generationJob == nil ? "-" : "done")) stage=\(m.stageAssetId.flatMap { id in m.assets.first { $0.id == id }?.name }?.replacingOccurrences(of: " ", with: "_") ?? "-") caption=\(m.generationCaption.replacingOccurrences(of: " ", with: "_"))\n".utf8))
                    }
                }
                // Stop the generation after some seconds, as the Stop
                // button would; then, with VALTZ_SNAPSHOT_RESTART, Start
                // again once it has stopped.
                if let s = env["VALTZ_SNAPSHOT_STOP_AFTER"].flatMap(Double.init) {
                    try? await Task.sleep(for: .seconds(s))
                    m.stop()
                    for _ in 0..<80 where m.isGenerating {
                        try? await Task.sleep(for: .milliseconds(250))
                    }
                    if env["VALTZ_SNAPSHOT_RESTART"] != nil {
                        try? await Task.sleep(for: .milliseconds(500))
                        m.start()
                        FileHandle.standardError.write(Data(
                            "snapshot: restart generating=\(m.isGenerating) banner=\(m.banner ?? "-") base=\(m.baseAttachment?.url.lastPathComponent ?? "-")\n".utf8))
                    }
                }
                // Document steps once the generation started above has
                // landed (VALTZ_SNAPSHOT_EDIT's, "undo;redo"): it is
                // undone as one command, placed result and all.
                if let spec = env["VALTZ_SNAPSHOT_EDIT_AFTER"] {
                    for _ in 0..<Int(delay * 4) where m.isGenerating {
                        try? await Task.sleep(for: .milliseconds(250))
                    }
                    for step in spec.split(separator: ";").map(String.init) {
                        try? await Task.sleep(for: .milliseconds(800))
                        if step == "undo" { m.undoProject() }
                        if step == "redo" { m.redoProject() }
                        try? await Task.sleep(for: .milliseconds(400))
                        FileHandle.standardError.write(Data(
                            "snapshot: edit-after \(step) dirty=\(m.document.dirty) undo=\(m.document.undo ?? "-") redo=\(m.document.redo ?? "-") stage=\(m.stageAssetId ?? "-") layers=\(m.stageStack?.layerStack.map { $0.id.isEmpty ? "0" : $0.id }.joined(separator: ",") ?? "-") assets=\(m.assets.count)\n".utf8))
                    }
                }
                // Adjustments ("exposure=0.4,saturation=0.3"), once the
                // generation started above has a result to adjust.
                if let spec = env["VALTZ_SNAPSHOT_ADJUST"] {
                    for _ in 0..<Int(delay * 4) where !m.canAdjust {
                        try? await Task.sleep(for: .milliseconds(250))
                    }
                    // As the panel sets them (a clip's go to its keys).
                    let a = Self.adjustments(spec)
                    for key in ImageAdjustments.Key.allCases where a[key] != 0 {
                        m.setAdjustment(key, a[key])
                    }
                    withAnimation(AppModel.motion) { m.openPanel = .adjust }
                    try? await Task.sleep(for: .seconds(1.5))
                }
                // The key button, here: "adjust" or "crop".
                if let t = env["VALTZ_SNAPSHOT_KEY_TOGGLE"] {
                    m.toggleKey(t == "crop" ? .place
                                    : t == "rotate" ? .turn : .adjust)
                }
            }
            // Start Over, straight through (no confirmation).
            // Bypass held ("adjust", "place", "turn"; comma-separated), as
            // its button does while pressed.
            if let m = model, let spec = env["VALTZ_SNAPSHOT_BYPASS"] {
                for part in spec.split(separator: ",") {
                    switch part {
                    case "adjust": m.setBypass(.adjust, true)
                    case "place": m.setBypass(.place, true)
                    case "turn": m.setBypass(.turn, true)
                    default: break
                    }
                }
                try? await Task.sleep(for: .seconds(1))
            }
            // Focus the field `name` names (its placeholder, in any
            // window -- Custom's panel is a popover) and click the
            // stepper beside it: "Steps:up:2" (events sent to the window,
            // not the pointer). The summary's `tuning=` says what took.
            if let spec = env["VALTZ_SNAPSHOT_STEPPER"] {
                let p = spec.split(separator: ":").map(String.init)
                if p.count >= 2 {
                    try? await Task.sleep(for: .milliseconds(600))
                    await Self.clickStepper(field: p[0], up: p[1] == "up",
                                            times: p.count > 2
                                                ? Int(p[2]) ?? 1 : 1)
                    try? await Task.sleep(for: .milliseconds(600))
                }
            }
            // Tab through the text fields from the one `start` names (its
            // placeholder), as the key does: the order focus takes.
            if let spec = env["VALTZ_SNAPSHOT_TABS"] {
                let parts = spec.split(separator: ",").map(String.init)
                let n = parts.count > 1 ? Int(parts[1]) ?? 8 : 8
                try? await Task.sleep(for: .milliseconds(500))
                let order = await Self.tabOrder(from: parts.first ?? "",
                                                count: n)
                FileHandle.standardError.write(Data(
                    "snapshot: tabs=\(order.joined(separator: " > "))\n"
                        .utf8))
            }
            if let m = model, env["VALTZ_SNAPSHOT_START_OVER"] != nil {
                let old = m.projectPath
                m.startOver()
                let gone = !FileManager.default.fileExists(atPath: old)
                FileHandle.standardError.write(Data(
                    "snapshot: startOver oldPackageGone=\(gone)\n".utf8))
            }
            // The stage's compare mode ("difference"), once a result
            // has landed to compare.
            if let m = model, let name = env["VALTZ_SNAPSHOT_STAGE_MODE"],
               let mode = CompareMode(rawValue: name) {
                for _ in 0..<Int(delay * 4) where !m.canAdjust {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                m.setStageMode(mode)
            }
            // Once a result has landed: attach a picture, then click
            // badges ("0,0") -- the base toggle against a result.
            if let m = model, env["VALTZ_SNAPSHOT_THEN_ATTACH"] != nil
                || env["VALTZ_SNAPSHOT_THEN_CLICK_BADGE"] != nil {
                for _ in 0..<Int(delay * 4) where !m.canAdjust {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                if let f = env["VALTZ_SNAPSHOT_THEN_ATTACH"] {
                    let url = URL(fileURLWithPath: f)
                    m.addReferences([url])
                    for _ in 0..<100 where m.asset(forFile: url) == nil {
                        try? await Task.sleep(for: .milliseconds(100))
                    }
                }
                for n in (env["VALTZ_SNAPSHOT_THEN_CLICK_BADGE"] ?? "")
                    .split(separator: ",").compactMap({ Int($0) }) {
                    try? await Task.sleep(for: .milliseconds(400))
                    m.toggleBase(picture: n)
                }
                try? await Task.sleep(for: .milliseconds(400))
            }
            // Save… as an export ("exr:/tmp/out.exr"), as the save
            // panel's format menu would, of the asset on the stage (the
            // newest picture or video put there when the stage has
            // another kind).
            if let m = model, let spec = env["VALTZ_SNAPSHOT_EXPORT"],
               let colon = spec.firstIndex(of: ":") {
                let choice = ExportChoice(rawValue: String(spec[..<colon]))
                    ?? .original
                let dest = URL(fileURLWithPath:
                    String(spec[spec.index(after: colon)...]))
                let kind = choice.video ? "video" : "image"
                if m.currentAsset?.kind != kind,
                   let last = m.assets.last(where: {
                       $0.kind == kind && $0.head > 0
                   }) {
                    m.putOnStage(last)
                }
                try? FileManager.default.removeItem(at: dest)
                m.save(to: dest, as: choice)
                // VALTZ_SNAPSHOT_EXPORT_WAIT=<s>: the snapshot that long
                // after it starts, while it runs (a clip's export on the
                // stage), not once it is saved.
                if let wait = env["VALTZ_SNAPSHOT_EXPORT_WAIT"]
                    .flatMap(Double.init) {
                    try? await Task.sleep(for: .seconds(wait))
                    let size = m.exportShow?.preview.map {
                        "\($0.width)x\($0.height)" } ?? "-"
                    FileHandle.standardError.write(Data(
                        "snapshot: exporting caption=\(m.exportCaption) preview=\(size)\n".utf8))
                    if env["VALTZ_SNAPSHOT_EXPORT_STOP"] != nil {
                        m.stopExport()
                        try? await Task.sleep(for: .seconds(1.5))
                        FileHandle.standardError.write(Data(
                            "snapshot: export-stopped showing=\(m.exportShow != nil) exists=\(FileManager.default.fileExists(atPath: dest.path))\n".utf8))
                    }
                }
                for _ in 0..<Int(delay * 4) where
                    env["VALTZ_SNAPSHOT_EXPORT_WAIT"] == nil &&
                    !FileManager.default.fileExists(atPath: dest.path) {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                try? await Task.sleep(for: .milliseconds(500))
                FileHandle.standardError.write(Data(
                    "snapshot: export \(choice.rawValue) -> \(dest.path) exists=\(FileManager.default.fileExists(atPath: dest.path)) choices=\(m.exportChoices.map(\.rawValue))\n".utf8))
            }
            // Save… through its panel: "jpeg:60:/tmp/out.jpg" (the
            // format, its quality -- a JPEG's 1...100, PNG's or TIFF's
            // depth 8 or 16 -- and the file), chosen in the panel as a
            // person would, then Save pressed. With VALTZ_SNAPSHOT_SHARE
            // the share menu opens first (captured a moment) and its
            // Save… item runs it.
            let saveSpec = (env["VALTZ_SNAPSHOT_SAVE"] ?? "")
                .split(separator: ":", maxSplits: 2).map(String.init)
            if saveSpec.count == 3 {
                let dest = URL(fileURLWithPath: saveSpec[2])
                try? FileManager.default.removeItem(at: dest)
                try? FileManager.default.createDirectory(
                    at: dest.deletingLastPathComponent(),
                    withIntermediateDirectories: true)
                ShareButton.scriptedDestination = dest
                ShareButton.script = { panel, options in
                    // A click on the Format menu, as a pointer makes it:
                    // does its menu open?
                    FileHandle.standardError.write(Data(
                        "snapshot: panel sheetParent=\(panel.sheetParent.map { String(describing: type(of: $0)) } ?? "-") visible=\(panel.isVisible) accessoryWindow=\(panel.accessoryView?.window.map { "\(type(of: $0))#\($0.windowNumber)" } ?? "-") panel#\(panel.windowNumber)\n".utf8))
                    if env["VALTZ_SNAPSHOT_SAVE_CLICK"] != nil,
                       let acc = panel.accessoryView,
                       let w = acc.window {
                        ValtzApp.clickFirstControl(in: acc, window: w)
                    }
                    let f = ExportChoice(rawValue: saveSpec[0]) ?? .original
                    options.setFormat(f)
                    if f.hasDepth { options.setDeep(saveSpec[1] == "16") }
                    if let q = Double(saveSpec[1]), f == .jpeg {
                        options.setQuality(q)
                    }
                    Task { @MainActor in
                        // Long enough to capture the panel.
                        try? await Task.sleep(for: .seconds(3))
                        FileHandle.standardError.write(Data(
                            "snapshot: save panel \(type(of: panel)) choice=\(options.choice.rawValue) quality=\(options.jpegQuality.map(String.init) ?? "-") estimate=\(options.estimate.map(String.init) ?? "-") name=\(panel.nameFieldStringValue)\n".utf8))
                        // As Save would: NSSavePanel.ok(_:) throws when
                        // not sent by its button.
                        if let parent = panel.sheetParent {
                            parent.endSheet(panel, returnCode: .OK)
                        } else {
                            NSApp.stopModal(withCode: .OK)
                        }
                    }
                }
            }
            if let m = model, env["VALTZ_SNAPSHOT_SHARE"] != nil {
                m.shareRequest += 1
                try? await Task.sleep(for: .milliseconds(2500))
                let menu = ShareButton.openedMenu
                let file = NSApp.mainMenu?.items.dropFirst().first?.submenu
                // As it is about to open: SwiftUI brings its items up to
                // date then, not before.
                if let file { file.delegate?.menuNeedsUpdate?(file) }
                FileHandle.standardError.write(Data(
                    "snapshot: share menu \(menu?.items.map(\.title) ?? []) file=\(file?.items.map { $0.title + ($0.keyEquivalent.isEmpty ? "" : "⌘" + $0.keyEquivalent) } ?? [])\n".utf8))
                menu?.cancelTracking()
                // "sheet": its Share… then -- the system's sheet.
                if env["VALTZ_SNAPSHOT_SHARE"] == "sheet",
                   let i = menu?.items.lastIndex(where: { !$0.isSeparatorItem }) {
                    try? await Task.sleep(for: .milliseconds(300))
                    ValtzApp.fromRunLoop { menu?.performActionForItem(at: i) }
                } else if saveSpec.count == 3 {
                    try? await Task.sleep(for: .milliseconds(300))
                    // From the run loop, as a click is: run from this
                    // task, the panel's modal loop would hold the main
                    // queue -- and every main-actor task -- until it
                    // closed.
                    ValtzApp.fromRunLoop { menu?.performActionForItem(at: 0) }
                }
            } else if let m = model, saveSpec.count == 3 {
                ValtzApp.fromRunLoop { ShareButton.save(m) }
            }
            if saveSpec.count == 3 {
                let dest = URL(fileURLWithPath: saveSpec[2])
                for _ in 0..<Int(delay * 4) where
                    !FileManager.default.fileExists(atPath: dest.path) {
                    try? await Task.sleep(for: .milliseconds(250))
                }
                try? await Task.sleep(for: .milliseconds(500))
                let size = (try? FileManager.default.attributesOfItem(
                    atPath: dest.path)[.size] as? Int) ?? -1
                FileHandle.standardError.write(Data(
                    "snapshot: saved \(dest.path) bytes=\(size)\n".utf8))
            }
            // Pictures onto the compare's sides, as a drop on A or B makes
            // it (the asset list's rows, and their bases): "b:<asset>"
            // compares an asset as B, "a:base:<asset>" the picture it was
            // made from as A ("newest" the newest generated picture,
            // "newest-edit" the newest with a base); "stage:<asset>" a
            // made clip's or song's Show on Stage ("newest-video",
            // "newest-audio"); "return" puts the current state back in A.
            if let m = model, let spec = env["VALTZ_SNAPSHOT_COMPARE"] {
                let made = m.assets.filter(\.isGeneration)
                    .sorted { $0.created > $1.created }
                @MainActor func pick(_ arg: String) -> AssetDTO? {
                    switch arg {
                    case "newest": made.first { $0.kind == "image" }
                    case "newest-video": made.first { $0.kind == "video" }
                    case "newest-audio": made.first { $0.kind == "audio" }
                    case "newest-edit": made.first { m.baseEntry(of: $0) != nil }
                    default: m.assets.first { $0.id == arg }
                    }
                }
                for step in spec.split(separator: ";") {
                    try? await Task.sleep(for: .milliseconds(600))
                    if step == "return" {
                        m.returnToCurrent()
                        continue
                    }
                    let p = step.split(separator: ":").map(String.init)
                    guard p.count >= 2, let a = pick(p.last ?? "") else {
                        continue
                    }
                    if p[0] == "stage" {
                        m.showMadeOnStage(a)
                        continue
                    }
                    let slot: StageSlot = p[0] == "a" ? .a : .b
                    let took: Bool
                    if p.count == 3 && p[1] == "base" {
                        took = m.baseEntry(of: a).map {
                            m.dropOnCompare($0.url, in: slot)
                        } ?? false
                    } else {
                        took = m.dropOnCompare(URL(string: AppModel
                            .assetPrefix + a.id)!, in: slot)
                    }
                    FileHandle.standardError.write(Data(
                        "snapshot: compare \(step) took=\(took)\n".utf8))
                }
                try? await Task.sleep(for: .milliseconds(600))
            }
            // Toggles in turn, animated as their buttons are --
            // "inspector:2,generate:3": the inspector 2 s after this
            // point, then the generation card's drawer 3 s after that
            // (inspector | editor | compare | generate | adjust | crop | trim). Each prints
            // its time, to find it in a filmed run.
            if let m = model, let steps = env["VALTZ_SNAPSHOT_TOGGLES"] {
                for step in steps.split(separator: ",") {
                    let p = step.split(separator: ":").map(String.init)
                    guard p.count == 2, let w = Double(p[1]) else { continue }
                    try? await Task.sleep(for: .seconds(w))
                    FileHandle.standardError.write(Data(
                        "snapshot: toggle \(p[0]) at \(Date().timeIntervalSince1970)\n".utf8))
                    withAnimation(AppModel.motion) {
                        if p[0] == "inspector" {
                            m.inspectorOpen.toggle()
                        } else if p[0] == "editor" {
                            m.toggleImmersivePrompt()
                        } else if p[0] == "compare" {
                            m.toggleCompare()
                            FileHandle.standardError.write(Data(
                                "snapshot: compare on=\(m.compareOn) b=\(m.stage.b.map { "\($0.width)x\($0.height)" } ?? "-") mode=\(m.stage.mode)\n".utf8))
                        } else if let panel = ComposerPanel.allCases.first(
                            where: { "\($0)" == p[0] }) {
                            m.openPanel = m.openPanel == panel ? nil : panel
                        }
                    }
                }
            }
            if let m = model, env["VALTZ_SNAPSHOT_DRAG_READY"] != nil {
                m.stageDragReady = true
            }
            if let m = model, let q = env["VALTZ_SNAPSHOT_SEARCH"] {
                m.searchText = q
            }
            // The title bar's zoom group: "in,in,out,fit,actual".
            if let m = model, let z = env["VALTZ_SNAPSHOT_ZOOM"] {
                for step in z.split(separator: ",") {
                    switch step {
                    case "in": m.zoom(.zoomIn)
                    case "out": m.zoom(.zoomOut)
                    case "fit": m.zoom(.fit)
                    case "actual": m.zoom(.actualSize)
                    case "unfit": m.toggleFit()
                    default: break
                    }
                    try? await Task.sleep(for: .milliseconds(200))
                }
            }
            // A pan of the stage ("-4000,2500": points, as a drag or a
            // trackpad moves it -- through the canvas's own pan, in 20
            // steps), then where the picture is in the view. Not mouse
            // events: a test window is never active, and its first click
            // would only activate it.
            if let spec = env["VALTZ_SNAPSHOT_PAN"],
               let win = NSApp.windows.first(where: {
                   $0.isVisible && Self.find(CompareCanvas.self,
                                             in: $0.contentView ?? NSView()) != nil
               }),
               let canvas = Self.find(CompareCanvas.self,
                                      in: win.contentView ?? NSView()) {
                let d = spec.split(separator: ",").compactMap { Double($0) }
                if d.count == 2 {
                    for _ in 0..<20 {
                        canvas.panBy(dx: d[0] / 20, dy: d[1] / 20)
                    }
                    try? await Task.sleep(for: .milliseconds(300))
                    let shown = canvas.shownRectA
                    let seen = shown.intersection(canvas.bounds)
                    FileHandle.standardError.write(Data(
                        "snapshot: shown=\(Int(shown.minX)),\(Int(shown.minY)) \(Int(shown.width))x\(Int(shown.height)) in \(Int(canvas.bounds.width))x\(Int(canvas.bounds.height)), visible \(Int(seen.width))x\(Int(seen.height)) pt\n".utf8))
                }
            }
            // A screen ("log"), or Settings on a page ("capabilities",
            // "storage", "general") -- then the snapshot is of that window.
            if let m = model, let s = env["VALTZ_SNAPSHOT_SCREEN"],
               let screen = AppScreen(rawValue: s) {
                m.show(screen)
            }
            let settings = env["VALTZ_SNAPSHOT_SETTINGS"] != nil
            if settings,
               let app = NSApp.mainMenu?.items.first?.submenu,
               let i = app.items.firstIndex(where: {
                   $0.keyEquivalent == "," }) {
                // The app menu's Settings…, as a person would choose it.
                app.performActionForItem(at: i)
            }
            // The download prompt's Open Capabilities: Settings on that
            // page, the family opened ("minimax-h3").
            if settings, let fam = env["VALTZ_SNAPSHOT_OPEN_CAPS"],
               let m = model {
                try? await Task.sleep(for: .milliseconds(1200))
                m.openCapabilities(family: fam)
                try? await Task.sleep(for: .milliseconds(800))
            }
            // A gated model's Download (Capabilities): its sheet opened,
            // a token typed if given (never downloaded here), the sheet
            // drawn to <snapshot>-gated.png.
            if settings, let id = env["VALTZ_SNAPSHOT_GATED"], let m = model {
                try? await Task.sleep(for: .milliseconds(1200))
                let member = m.capabilityTree?.families
                    .flatMap(\.members).first { $0.model == id }
                m.snapshotGatedToken = env["VALTZ_SNAPSHOT_GATED_TOKEN"]
                m.gatedDownload = member
                try? await Task.sleep(for: .milliseconds(1200))
                let sheet = NSApp.windows.first { $0.isSheet && $0.isVisible }
                if let w = sheet, let v = w.contentView?.superview,
                   let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds),
                   let snap = env["VALTZ_SNAPSHOT"] {
                    v.cacheDisplay(in: v.bounds, to: rep)
                    let out = (snap as NSString).deletingPathExtension
                        + "-gated.png"
                    try? rep.representation(using: .png, properties: [:])?
                        .write(to: URL(fileURLWithPath: out))
                }
                FileHandle.standardError.write(Data(
                    ("snapshot: gated model=\(id) found=\(member != nil) "
                     + "gated=\(member?.isGated ?? false) "
                     + "sheet=\(sheet.map { "\(Int($0.frame.width))x\(Int($0.frame.height))" } ?? "-")\n").utf8))
                // Closed again, as Cancel does: an open sheet holds the
                // quit.
                m.gatedDownload = nil
                try? await Task.sleep(for: .milliseconds(600))
            }
            // Clicks posted to the Settings window ("x,y" points from its
            // top left; ";" between) once it is open.
            if settings, let spec = env["VALTZ_SNAPSHOT_SETTINGS_CLICKS"] {
                try? await Task.sleep(for: .milliseconds(1500))
                let win = NSApp.windows.first(where: {
                    $0.isVisible && $0.identifier?.rawValue
                        .contains("Settings") == true
                }) ?? NSApp.keyWindow
                for c in spec.split(separator: ";") {
                    let xy = c.split(separator: ",").compactMap {
                        Double($0)
                    }
                    guard xy.count == 2, let win else { continue }
                    Self.postClick(x: xy[0], y: xy[1], in: win)
                    try? await Task.sleep(for: .milliseconds(800))
                    FileHandle.standardError.write(Data(
                        "snapshot: settings-click \(Int(xy[0])),\(Int(xy[1]))\n".utf8))
                }
            }
            try? await Task.sleep(for: .seconds(max(0, delay - 1)))
            // The window to capture: Settings when it was asked for, else
            // the main one.
            @MainActor func target() -> NSWindow? {
                if settings,
                   let w = NSApp.windows.first(where: {
                       $0.isVisible && $0.identifier?.rawValue
                           .contains("Settings") == true
                   }) ?? NSApp.keyWindow {
                    return w
                }
                return NSApp.windows.first(where: { $0.isVisible })
            }
            do {
                // VALTZ_SNAPSHOT_APPKIT=1: skip ScreenCaptureKit, draw the
                // whole window frame (title bar included) with AppKit.
                if env["VALTZ_SNAPSHOT_APPKIT"] != nil {
                    throw CocoaError(.featureUnsupported)
                }
                let content = try await SCShareableContent.currentProcess
                let number = target().map { CGWindowID($0.windowNumber) }
                guard let win = content.windows.first(where: {
                    $0.isOnScreen && $0.windowLayer == 0
                        && (number == nil || $0.windowID == number)
                }) else { throw CocoaError(.fileNoSuchFile) }
                let filter = SCContentFilter(desktopIndependentWindow: win)
                let cfg = SCStreamConfiguration()
                let scale = CGFloat(filter.pointPixelScale)
                cfg.width = Int(filter.contentRect.width * scale)
                cfg.height = Int(filter.contentRect.height * scale)
                cfg.showsCursor = false
                let img = try await SCScreenshotManager.captureImage(
                    contentFilter: filter, configuration: cfg)
                let url = URL(fileURLWithPath: path) as CFURL
                if let dst = CGImageDestinationCreateWithURL(
                    url, UTType.png.identifier as CFString, 1, nil) {
                    CGImageDestinationAddImage(dst, img, nil)
                    CGImageDestinationFinalize(dst)
                }
            } catch {
                FileHandle.standardError.write(Data(
                    "snapshot: ScreenCaptureKit unavailable (\(error.localizedDescription)); using cacheDisplay\n".utf8))
                if let content = target()?.contentView,
                    let view = content.superview ?? Optional(content),
                    let rep = view.bitmapImageRepForCachingDisplay(
                        in: view.bounds) {
                    view.cacheDisplay(in: view.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?
                        .write(to: URL(fileURLWithPath: path))
                }
            }
            if let m = model {
                // The focus's undo as it is now (no event may have come
                // since the last scripted step).
                m.refreshTextUndo()
                let ready = m.capabilities
                    .filter { $0.availability == "ready" }
                    .map(\.capability)
                let models = m.installedImageModels.map(\.model)
                let line = "snapshot: project=\(m.projectName) "
                    + "assets=\(m.assets.count) imageModel=\(m.imageModel) "
                    + "editModel=\(m.editModel) willEdit=\(m.willEdit) "
                    + "modality=\(m.modality.rawValue) "
                    + "modelChoice=\(m.modelChoice.isEmpty ? "auto" : m.modelChoice) "
                    + "panel=\(String(describing: m.openPanel)) "
                    + "compact=\(m.promptCompact) "
                    + "title=\(m.windowTitle) inspector=\(m.inspectorOpen) "
                    + "titlebarHover=\(m.titlebarHover) "
                    + "anonymous=\(m.isAnonymous) projectPath=\(m.projectPath) "
                    + "base=\(m.baseAttachment?.url.lastPathComponent ?? "-") refs=\(m.promptAttachments.filter { !$0.isBase }.count) "
                    + "dragReady=\(m.stageDragReady && m.canDragStage) attachments=\(m.promptAttachments.count) "
                    + "hits=\(m.searchHits.sorted()) "
                    + "window=\(NSApp.windows.first { $0.isVisible }.map { "\(Int($0.frame.width))x\(Int($0.frame.height))" } ?? "-") "
                    + "screen=\(NSScreen.main.map { "\(Int($0.visibleFrame.width))x\(Int($0.visibleFrame.height))" } ?? "-") "
                    + "installed=\(models) ready=\(ready) "
                    + "zoom=\(m.currentViewer.zoomText) fit=\(m.currentViewer.fitting) actual=\(m.currentViewer.isActualSize) "
                    + "output=\(m.dimensions.width)x\(m.dimensions.height) "
                    + "custom=\(m.customSize != nil) orientation=\(m.shownOrientation.rawValue) "
                    + "ratioShown=\(m.showsAspectRatio) "
                    + "marked=\(m.promptMarked.replacingOccurrences(of: "\u{FFFC}", with: "[*]").replacingOccurrences(of: " ", with: "_")) "
                    + "languages=\(L10n.available.joined(separator: ",")) "
                    + "refs=\(m.promptAttachments.map { a in (a.isBase ? "*" : "") + (a.ownCopy ? "+" : "") + a.url.lastPathComponent + (m.referenceNumber(a).map { "#\($0)" } ?? "") }.joined(separator: ",")) "
                    + "active=\((m.assets.first { $0.id == m.stageAssetId }?.name ?? "-").replacingOccurrences(of: " ", with: "_")) "
                    + "assets=\(m.assets.count) ops=\(Dictionary(grouping: m.assets, by: { $0.op ?? "import" }).map { "\($0.key):\($0.value.count)" }.sorted().joined(separator: ",")) stale=\(m.assets.filter { m.isStale($0, used: m.assetsInUse) }.count) folders=\(m.assetFolders.map { f in "\(f.name)\(m.closedFolders.contains(f.id) ? "-folded" : ""):\(m.assets.filter { $0.folder == f.id }.count)" }.joined(separator: "|").replacingOccurrences(of: " ", with: "_")) renaming=\(m.renamingAsset ?? "-") "
                    + "mentions=\(m.promptMentions.count) focus=\(m.promptAttachments.firstIndex { $0.id == m.focusedReference }.map(String.init) ?? "-") "
                    + "stack=\(m.stackPlayback.map { "\($0.width)x\($0.height)/\($0.frames)f/\($0.clips.count)clips" } ?? "-") "
                    + "look=\(m.stageStack?.layerStack.map { l in let k = m.layerLook(l.id); return "\(l.id.isEmpty ? "0" : l.id):\(k.adjust ? "a" : "")\(k.place ? "p" : "")\(k.turn ? "t" : "")\(k.keyed ? "k" : "")" }.joined(separator: ",") ?? "-") "
                    + "layers=\(m.stageStack?.layerStack.map { "\($0.id.isEmpty ? "0" : $0.id)\($0.visible ? "" : "-hidden")\($0.isEmpty ? "-empty" : "")\($0.isMarkup ? "-markup\($0.markup?.raster.isEmpty == false ? "+px" : "")\($0.markup.map { "+\($0.objects.count)obj" } ?? "")" : "")\($0.mask ? "-mask" : "")" }.joined(separator: ",") ?? "-") selectedLayer=\(m.activeLayer.isEmpty ? "0" : m.activeLayer) selectedLayers=\(m.selectedLayers.map { $0.isEmpty ? "0" : $0 }.sorted()) inspector=\(m.inspectorStacked ? "stacked" : "one"):\(InspectorTab.allCases.filter { !m.inspectorFolded.contains($0) }.map(\.rawValue).joined(separator: "+")) panels=\(Self.panelSummary(m)) tasks=\(Self.taskSummary(m)) projectOutput=\(m.projectOutput.color),\(m.projectOutput.fps.map(String.init).joined(separator: "/")),\(m.projectOutput.channels),\(m.projectOutput.sampleRate) options=\(m.modelOptions(for: m.activeModality).map(\.name).joined(separator: "|").replacingOccurrences(of: " ", with: "_")) runningModel=\(m.runningModel) hint=\(String(m.promptHint.prefix(40)).replacingOccurrences(of: " ", with: "_")) promptCompact=\(m.promptCompact) framed=\(m.stageStack?.canvas.map { c in c.framed ? "\(c.fw ?? 0)x\(c.fh ?? 0)" + (c.resized ? "@\(c.w)x\(c.h)" : "") : "-" } ?? "-") "
                    + "markup=\(m.markupOpen ? m.markup.tool.rawValue : "off"):sel[\(m.markup.selection.map(\.kind.rawValue).joined(separator: ","))]@\(m.markup.selectionLayer ?? "-") "
                    + "favor=\(m.preference.rawValue) tuning=\(m.preference == .custom ? m.customSummary : "-") "
                    + "upscale=\(m.upscaleJob != nil ? "running" : m.canUpscaleLayer ? "offered" : m.upscaleNeedsFlatten ? "flatten-first" : "-") "
                    + "capture=\(m.capture.recording ? "recording" : m.showsCapture ? "offered" : "-") captureSources=\(m.captureSources.map(\.kind).joined(separator: ",")) "
                    + "viewed=\(m.viewedAsset.flatMap { id in m.assets.first { $0.id == id }?.name.replacingOccurrences(of: " ", with: "_") } ?? "-") stageLift=\(String(format: "%.2f", m.stageLift)) "
                    + "pages=\(m.stagePages) page=\(m.stagePage + 1) layerPages=\(m.stagePicture.map { p in p.layerStack.map { l in "\(l.id.isEmpty ? "0" : l.id):\(p.pageSpan(of: l).map { "\($0.lowerBound + 1)-\($0.upperBound + 1)" } ?? "none")" }.joined(separator: ",") } ?? "-") keyed=\(m.keyedStage) keys=\(m.keyFrames(.place).map(String.init).joined(separator: ",")) "
                    + "memoryFailure=\(m.memoryFailure.map { f in "\(f.step):\(MemoryFailure.size(f.need)):\(f.gates.filter { !$0.ok }.map(\.name).joined(separator: "+")):parts=\(f.parts.map(\.name).joined(separator: "+")):plan=\(MemoryFailure.size(f.planPeak))" } ?? "-") "
                    + "suggests=\([m.suggestsShorterClip ? "length" : nil, m.suggestsSmallerSize ? "size" : nil, m.suggestsFewerReferences ? "refs" : nil].compactMap { $0 }.joined(separator: ",")) "
                    + "bypassed=\(m.bypassed.map { "\($0)" }.sorted()) "
                    + "statusBar=\(m.showsStatusBar) "
                    + "load=ane:\(m.monitor.load.ane.map { String(format: "%.1f", $0) } ?? "-"),gpu:\(m.monitor.load.gpu.map { String(format: "%.1f", $0) } ?? "-"),ram:\(m.monitor.load.footprint.map { String(format: "%.2fGB", $0 / 1e9) } ?? "-"),sys:\(m.monitor.load.systemUsed.map { String(format: "%.2fGB", $0 / 1e9) } ?? "-") "
                    + "thermal=\(m.monitor.thermal?.verdict ?? "-"):\(m.monitor.thermalLabel.replacingOccurrences(of: " ", with: "_")) "
                    + "firstResponder=\(NSApp.windows.first { $0.isVisible }?.firstResponder.map { r in ((r as? NSTextView)?.delegate as? NSTextField).map { "field(\($0.placeholderString ?? $0.stringValue))" } ?? String(describing: type(of: r)) } ?? "-") "
                    + "stageA=\(m.stage.aLabel.replacingOccurrences(of: " ", with: "_")) stageB=\(m.stage.bLabel.replacingOccurrences(of: " ", with: "_")) "
                    + "history=\(m.history.count) current=\(m.currentSlot.map { "\($0)" } ?? "-") takesCompare=\(m.stageTakesCompare) compareOn=\(m.compareOn) compareB=\(m.stage.b.map { "\($0.width)x\($0.height)" } ?? "-")@\(String(format: "%.4f", m.stage.bFit.scale)),\(String(format: "%.1f", m.stage.bFit.origin.x)),\(String(format: "%.1f", m.stage.bFit.origin.y)) compareA=\(m.stage.a.map { "\($0.width)x\($0.height)" } ?? "-") mode=\(m.stage.mode) dirty=\(m.document.dirty) untitled=\(m.document.untitled) undoNext=\(m.document.undo ?? "-") redoNext=\(m.document.redo ?? "-") undoTitle=\(m.undoMenuTitle.replacingOccurrences(of: " ", with: "_")) textUndo=\(m.textUndo.map { "\($0.canUndo)" } ?? "-") compared=\(m.stage.aCompared.map { _ in "a" } ?? "")\(m.stage.bCompared.map { _ in "b" } ?? "") "
                    + "stageMode=\(m.stage.mode.rawValue) "
                    + "bFit=\(m.stage.bFit.scale)@\(m.stage.bFit.origin.x),\(m.stage.bFit.origin.y) "
                    + "b=\(m.stage.b.map { "\($0.width)x\($0.height)" } ?? "-") "
                    + "a=\(m.stage.a.map { "\($0.width)x\($0.height)" } ?? "-") "
                    + "clip=\(m.stage.clip.map { "\($0.frames.count)f@\($0.fps)" } ?? "-") "
                    + "video=\(m.stage.video?.lastPathComponent ?? "-") stageVisible=\(m.stageVisible) stageCorners=\(m.roundStageCorners ? "round" : "square") suggestion=\(m.enhanced != nil) "
                    + "clipFrames=\(m.clipFrames) favorHint=\(m.preferenceHint.replacingOccurrences(of: " ", with: "_")) "
                    + "wipeLine=\(m.stage.wipeLine) canDragStage=\(m.canDragStage) "
                    + "lastError=\(m.log.last { $0.level == "error" }?.text ?? "-") "
                    + "banner=\((m.banner ?? "-").replacingOccurrences(of: " ", with: "_")) "
                    + "generating=\(m.isGenerating) exporting=\(m.exportShow != nil) "
                    + "stackFrames=\(m.stackCounts.map { "\($0.drawn)/\($0.skipped)" } ?? "-") "
                    + "promptText=\(m.prompt.replacingOccurrences(of: " ", with: "_").replacingOccurrences(of: "\n", with: "|")) "
                    + "immersive=\(m.promptImmersive) tabs=\(m.promptTabs.count) activeTab=\(m.activePromptTab) tabTitles=\(m.promptTabs.indices.map { m.promptTabTitle($0).replacingOccurrences(of: " ", with: "_") }.joined(separator: "|")) markdown=\(m.promptMarkdown) "
                    + "promptAsset=\(m.promptAssetId ?? "-") unbound=\(m.unboundRefTags.joined(separator: ",")) refTags=\(AppModel.refTags(in: m.prompt).count) prompts=\(m.assets.filter(\.isPrompt).count) "
                    + "references=\(m.usesReferences) badges=\(m.promptAttachments.map { (m.referenceBadge($0) ?? "-") + ($0.continues ? ">" : "") }.joined(separator: ",")) canContinue=\(m.canContinueClip) guides=\(m.guideLengths.map { "\($0.asked):\($0.frames)" }.joined(separator: ",")) running=\(m.runningModel) "
                    + "songPlan=\(m.songPlan.rawValue) songLength=\(m.songSeconds.map(String.init) ?? "auto") "
                    + "lyrics=\(m.songWords.sections)s/\(m.songWords.lines)l style=\(m.songWords.style.replacingOccurrences(of: " ", with: "_")) "
                    + "audio=\(m.stageIsAudio) rate=\(m.stageFrameRate.num)/\(m.stageFrameRate.den) timecode=\(m.stageFrameRate.timecode(m.videoFrame)) "
                    + "crop=\(m.crop.json.keys.sorted().map { "\($0):\(m.crop.json[$0]!)" }.joined(separator: ",")) "
                    + "cropEditing=\(m.cropEditing) guides=\(m.showsGuides) "
                    + "cropPlaced=\(m.stageCropPlacement.map { "\(Int($0.canvas.width))x\(Int($0.canvas.height))" } ?? "-") "
                    + "trim=\(m.trim.markIn.map(String.init) ?? "-")..\(m.trim.markOut.map(String.init) ?? "-") offset=\(m.trimOffset) "
                    + "clipTime=\(m.clipEdge?.rawValue ?? m.trimRate.timecode(m.sourceFrameAtPlayhead)) clipEdge=\(m.clipEdge?.rawValue ?? "-") layerEnd=\(m.layerClock.map { $0.end.isFinite ? String(format: "%.3f", $0.end) : "inf" } ?? "-") "
                    + "speed=\(m.layerSpeed.keys.map { "\($0.frame):\(CropField.display($0.value.rate))" }.joined(separator: ",")) sound=\(m.layerSound.keys.map { "\($0.frame):\(CropField.display($0.value.volume))/\(CropField.display($0.value.pitch))" }.joined(separator: ",")) followSpeed=\(m.pitchFollowsSpeed) resampled=\(m.layerResampled) timedLayer=\(m.layerIsTimed) layerSound=\(m.layerHasSound) "
                    + "viewing=\(m.stageViewingName.map { "\"\($0)\"" } ?? "-") project=\(m.projectViews.first.map { p in (p.kind == "audio" ? "sound" : p.isTimeline ? "timeline" : "still") + (p.ownFrame.map { ":\($0.w)x\($0.h)" } ?? "") + ":\(p.layerStack.count)layers" } ?? "-") "
                    + "videoFrame=\(m.videoFrame) showsCrop=\(m.showsCrop) "
                    + "showsTrim=\(m.showsTrim) showsAdjust=\(m.showsAdjust) "
                    + "adjustKeys=\(m.keyFrames(.adjust)) placeKeys=\(m.keyFrames(.place)) turnKeys=\(m.keyFrames(.turn)) "
                    + "cropHere=\(CropField.allCases.map { CropField.display($0.value(m.crop)) }.joined(separator: ",")) videoRate=\(m.videoRate) "
                    + "exposureHere=\(m.adjustments.exposure) rotateHere=\(m.crop.rotate) "
                    + "\n"
                FileHandle.standardError.write(Data(line.utf8))
            }
            NSApp.terminate(nil)
        }
    }

    /// "exposure=0.4,saturation=0.3" as adjustments.
    private static func adjustments(_ spec: String) -> ImageAdjustments {
        var a = ImageAdjustments()
        for pair in spec.split(separator: ",") {
            let kv = pair.split(separator: "=")
            if kv.count == 2,
               let key = ImageAdjustments.Key(rawValue: String(kv[0])),
               let v = Double(kv[1]) { a[key] = v }
        }
        return a
    }

    /// A drag posted to the window, from one point to another (points
    /// from its top left), in eight steps.
    @MainActor
    private static func postDrag(from a: (Double, Double),
                                 to b: (Double, Double)) async {
        guard let win = NSApp.windows.first(where: { $0.isVisible }) else {
            return
        }
        func post(_ type: NSEvent.EventType, _ x: Double, _ y: Double) {
            if let ev = NSEvent.mouseEvent(
                with: type, location: NSPoint(x: x, y: win.frame.height - y),
                modifierFlags: [], timestamp: ProcessInfo.processInfo
                    .systemUptime,
                windowNumber: win.windowNumber, context: nil,
                eventNumber: 0, clickCount: 1, pressure: 1) {
                win.sendEvent(ev)
            }
        }
        // The pointer arrives first, as a person's would: SwiftUI places
        // its views' AppKit hit areas on the first event it sees.
        post(.mouseMoved, a.0, a.1)
        try? await Task.sleep(for: .milliseconds(100))
        post(.leftMouseDown, a.0, a.1)
        try? await Task.sleep(for: .milliseconds(60))
        for i in 1...8 {
            let t = Double(i) / 8
            post(.leftMouseDragged, a.0 + (b.0 - a.0) * t,
                 a.1 + (b.1 - a.1) * t)
            try? await Task.sleep(for: .milliseconds(30))
        }
        post(.leftMouseUp, b.0, b.1)
    }

    /// The task queue: "T0:running:generate,T1:queued:export".
    @MainActor
    static func taskSummary(_ m: AppModel) -> String {
        m.tasks.isEmpty ? "-" : m.tasks.map {
            "\($0.badge):\($0.state):\($0.kind)"
        }.joined(separator: ",")
    }

    /// The stacked inspector's open sections: "layers:1.00@312" -- each
    /// one's share and height.
    @MainActor
    static func panelSummary(_ m: AppModel) -> String {
        InspectorTab.allCases.filter { !m.inspectorFolded.contains($0) }
            .map { t in
                "\(t.rawValue):\(String(format: "%.2f", m.inspectorWeight(t)))@\(Int(m.inspectorHeights[t] ?? 0))"
            }.joined(separator: ",")
    }

    /// A click -- down, then up -- posted to the window at (x, y) points
    /// from its top left, through the app's own event queue.
    @MainActor
    private static func postClick(x: Double, y: Double,
                                  in window: NSWindow? = nil) {
        guard let win = window
                ?? NSApp.windows.first(where: { $0.isVisible }) else {
            return
        }
        let p = NSPoint(x: x, y: win.frame.height - y)
        for type in [NSEvent.EventType.leftMouseDown, .leftMouseUp] {
            if let ev = NSEvent.mouseEvent(
                with: type, location: p, modifierFlags: [], timestamp: 0,
                windowNumber: win.windowNumber, context: nil,
                eventNumber: 0, clickCount: 1, pressure: 1) {
                NSApp.postEvent(ev, atStart: false)
            }
        }
    }

    /// The fields focus visits from `start`, pressing Tab `count` times.
    @MainActor
    private static func tabOrder(from start: String,
                                 count: Int) async -> [String] {
        guard let win = NSApp.windows.first(where: { $0.isVisible }),
              let root = win.contentView?.superview,
              let field = fields(in: root).first(where: {
                  $0.placeholderString == start })
        else { return ["no field \(start)"] }
        win.makeKeyAndOrderFront(nil)
        win.makeFirstResponder(field)
        var out = [focusName(win.firstResponder)]
        for _ in 0..<count {
            guard let ev = NSEvent.keyEvent(
                with: .keyDown, location: .zero, modifierFlags: [],
                timestamp: 0, windowNumber: win.windowNumber, context: nil,
                characters: "\t", charactersIgnoringModifiers: "\t",
                isARepeat: false, keyCode: 48) else { break }
            win.sendEvent(ev)
            try? await Task.sleep(for: .milliseconds(150))
            out.append(focusName(win.firstResponder))
        }
        return out
    }

    @MainActor
    private static func clickStepper(field: String, up: Bool,
                                     times: Int) async {
        for win in NSApp.windows where win.isVisible {
            guard let root = win.contentView?.superview,
                  let f = fields(in: root).first(where: {
                      $0.placeholderString == field }) else { continue }
            win.makeFirstResponder(f)
            let fr = f.convert(f.bounds, to: nil)
            // The stepper beside it: the nearest one to its right, on
            // its row.
            let steppers = views(NSStepper.self, in: root).map {
                ($0, $0.convert($0.bounds, to: nil))
            }.filter { abs($0.1.midY - fr.midY) < 8 && $0.1.minX >= fr.maxX }
            guard let (_, sr) = steppers.min(by: { $0.1.minX < $1.1.minX })
            else { return }
            let pt = NSPoint(x: sr.midX,
                             y: up ? sr.maxY - sr.height / 4
                                   : sr.minY + sr.height / 4)
            func event(_ type: NSEvent.EventType) -> NSEvent? {
                NSEvent.mouseEvent(
                    with: type, location: pt, modifierFlags: [],
                    timestamp: ProcessInfo.processInfo.systemUptime,
                    windowNumber: win.windowNumber, context: nil,
                    eventNumber: 0, clickCount: 1, pressure: 1)
            }
            for _ in 0..<times {
                // The stepper tracks the press in its own loop: the
                // release waits in the queue for it.
                guard let down = event(.leftMouseDown),
                      let upEv = event(.leftMouseUp) else { break }
                NSApp.postEvent(upEv, atStart: false)
                win.sendEvent(down)
                try? await Task.sleep(for: .milliseconds(250))
            }
            let still = (win.firstResponder as? NSTextView)?.delegate === f
            FileHandle.standardError.write(Data(
                "snapshot: stepper field=\(field) stillFocused=\(still) text=\(f.stringValue)\n"
                    .utf8))
            return
        }
    }

    @MainActor
    private static func views<V: NSView>(_ type: V.Type,
                                         in v: NSView) -> [V] {
        var out: [V] = []
        if let hit = v as? V { out.append(hit) }
        for sub in v.subviews { out += views(type, in: sub) }
        return out
    }

    @MainActor
    private static func fields(in v: NSView) -> [NSTextField] {
        var out: [NSTextField] = []
        if let f = v as? NSTextField, f.isEditable { out.append(f) }
        for sub in v.subviews { out += fields(in: sub) }
        return out
    }

    @MainActor
    private static func focusName(_ r: NSResponder?) -> String {
        if let tv = r as? NSTextView, let f = tv.delegate as? NSTextField {
            return f.placeholderString ?? f.stringValue
        }
        return r.map { String(describing: type(of: $0)) } ?? "-"
    }

    @MainActor
    static func find<V: NSView>(_ type: V.Type, in v: NSView) -> V? {
        if let hit = v as? V { return hit }
        for sub in v.subviews {
            if let hit = find(type, in: sub) { return hit }
        }
        return nil
    }

    func applicationShouldTerminateAfterLastWindowClosed(
        _ sender: NSApplication
    ) -> Bool { true }

    /// Unsaved changes: saved, let go, or the quit cancelled.
    @MainActor
    func applicationShouldTerminate(
        _ sender: NSApplication
    ) -> NSApplication.TerminateReply {
        guard let m = model else { return .terminateNow }
        guard m.confirmClose() else { return .terminateCancel }
        m.closeForQuit()
        return .terminateNow
    }

    @MainActor
    func applicationWillTerminate(_ notification: Notification) {
        // Stop the engine (finishing or cancelling the running job) and
        // close the project databases before the process exits.
        model?.shutdown()
    }
}
