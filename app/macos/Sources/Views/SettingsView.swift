import SwiftUI
import AppKit

/// Valtz ▸ Settings (⌘,), laid out as macOS System Settings: the pages in
/// a sidebar, each with its coloured icon, and the page on the right,
/// opening on a card that names it (SettingsHeader).
///   General       this Mac, where things are kept, the open project
///   Capabilities  the model families, what each makes here, and every
///                 resource it runs with -- downloaded, or linked from
///                 where it already is (CapabilitiesView)
///   Agentic Helper  the model that writes prompts with the person: which
///                 one, and how it runs (HelperView)
///   Upscaling     the upscalers' arithmetic -- int8 GEMMs, Sage, Sol --
///                 which no generation card sets (UpscalingView)
///   Fleet         this Mac as a member of a fleet of Valtz Macs: joined,
///                 made, discoverable; the jobs it takes (FleetView)
///   Storage       what Valtz keeps on the internal SSD (StorageView)
///   Permissions   what Valtz may ask macOS to use (PermissionsView)
///   Experimental  what is still being built, switched on to try it --
///                 the Agent (ExperimentalView)
///
/// The sidebar is System Settings' measure -- 32 pt rows inset 10 pt, 20
/// pt icons, 13 pt names -- with the selection in Valtz's own accent (a
/// near-black; near-white in the dark), as the rest of the app. A List's
/// selection takes the system's highlight colour (blue) instead, so the
/// rows are drawn here; ↑ and ↓ move between them.
struct SettingsView: View {
    @Bindable var model: AppModel
    @FocusState private var sidebarFocused: Bool

    private var place: SettingsPlace { model.settingsHistory.current }

    /// The sidebar's rows: every page -- or, while a search is typed, the
    /// pages it finds, the pages under them it finds, and under each the
    /// settings it finds there (SettingsMarks).
    private var rows: [SettingsResult] {
        model.settingsSearch.isEmpty
            ? SettingsPage.allCases.map { SettingsResult(.page($0)) }
            : SettingsPlace.results(model.settingsSearch, model)
    }

    var body: some View {
        NavigationSplitView {
            ScrollView {
                VStack(spacing: 0) {
                    ForEach(rows, id: \.self) { r in
                        SidebarRow(result: r, query: model.settingsSearch,
                                   selected: selected(r)) {
                            pick(r)
                        }
                    }
                    if rows.isEmpty {
                        Text("No Results")
                            .font(.system(size: 13))
                            .foregroundStyle(.secondary)
                            .padding(.top, 24)
                    }
                }
                .padding(.horizontal, 10)
                .padding(.top, 6)
            }
            .focusable()
            .focused($sidebarFocused)
            .focusEffectDisabled()
            .onKeyPress(.upArrow) { step(-1) }
            .onKeyPress(.downArrow) { step(1) }
            .navigationSplitViewColumnWidth(Self.sidebarWidth)
            .background(SidebarWidthKeeper(width: Self.sidebarWidth))
            .background(UnifiedToolbar())
            .toolbar(removing: .sidebarToggle)
        } detail: {
            ScrollViewReader { scroller in
            Group {
                switch place {
                case .page(.general): GeneralSettings(model: model)
                case .page(.capabilities): CapabilitiesView(model: model)
                case .page(.helper): HelperView(model: model)
                case .page(.upscaling): UpscalingView(model: model)
                case .page(.fleet): FleetView(model: model) { go($0) }
                case .page(.storage): StorageView(model: model)
                case .page(.permissions): PermissionsView(model: model)
                case .page(.experimental): ExperimentalView(model: model)
                case .fleetOffload: FleetOffloadView(model: model)
                case .fleetSharing: FleetSharingView(model: model)
                }
            }
            // What the search finds, highlighted on the page; a setting
            // picked from its results scrolled to once the page is laid
            // out -- one just opened is not at once -- and not animated:
            // either way, a page's list scrolls nowhere. The pulse draws
            // the eye.
            .environment(\.settingsQuery, model.settingsSearch)
            .environment(\.settingsReveal, model.settingsReveal)
            .onChange(of: model.settingsReveal) { _, r in
                guard let r else { return }
                Task { @MainActor in
                    try? await Task.sleep(for: .milliseconds(500))
                    if let at = SettingsMarksShown.shared.anchor(r.name) {
                        scroller.scrollTo(at, anchor: .center)
                    }
                }
            }
            }
            // Named beside the arrows, as System Settings names a pane
            // (and for the Window menu).
            .navigationTitle(place.title)
            .toolbar {
                ToolbarItem(placement: .navigation) {
                    ControlGroup {
                        Button {
                            model.settingsHistory.back()
                        } label: {
                            Label("Back", systemImage: "chevron.left")
                        }
                        .disabled(!model.settingsHistory.canGoBack)
                        .keyboardShortcut("[", modifiers: .command)
                        .help("Back")
                        Button {
                            model.settingsHistory.forward()
                        } label: {
                            Label("Forward", systemImage: "chevron.right")
                        }
                        .disabled(!model.settingsHistory.canGoForward)
                        .keyboardShortcut("]", modifiers: .command)
                        .help("Forward")
                    }
                    .controlGroupStyle(.navigation)
                    // On the pair: the toolbar draws them itself, and a
                    // button's own modifiers are not kept.
                    .shortcutHint("⌘[  ⌘]")
                }
            }
        }
        // Search at the top of the sidebar, as System Settings has it.
        .searchable(text: $model.settingsSearch, placement: .sidebar,
                    prompt: Text("Search"))
        .frame(minWidth: 860, idealWidth: 920, minHeight: 600,
               idealHeight: 680)
        // Controls take Valtz's accent, not the system's.
        .tint(Color.valtzAccent)
        .background(SettingsWindowKeeper(model: model))
        .onAppear {
            sidebarFocused = true
            takeRequest()
        }
        // Another part of the app asking for a page (a preset's LoRA:
        // Capabilities).
        .onChange(of: model.settingsPageRequest) { _, _ in takeRequest() }
    }

    private func go(_ p: SettingsPlace) {
        model.settingsHistory.go(p)
    }

    /// A row picked: its place shown -- and a setting found there
    /// scrolled to and pulsing.
    private func pick(_ r: SettingsResult) {
        go(r.place)
        if let s = r.setting {
            model.settingsReveal = SettingsReveal(
                name: s, serial: (model.settingsReveal?.serial ?? 0) + 1)
        } else {
            model.settingsReveal = nil
        }
    }

    /// The setting picked from the results, while Settings is still where
    /// it was picked.
    private var revealed: SettingsResult? {
        guard let r = model.settingsReveal else { return nil }
        let res = SettingsResult(place, r.name)
        return rows.contains(res) ? res : nil
    }

    /// The row of where Settings is -- the setting picked there, when one
    /// was -- or of the page it is under, unless a search lists that page
    /// itself.
    private func selected(_ r: SettingsResult) -> Bool {
        if let v = revealed { return r == v }
        guard r.setting == nil else { return false }
        if r.place == place { return true }
        return r.place == .page(place.page)
            && !rows.contains(SettingsResult(place))
    }

    private func takeRequest() {
        guard let r = model.settingsPageRequest,
              let p = SettingsPlace(raw: r) else { return }
        go(p)
        model.settingsPageRequest = nil
    }

    /// System Settings' sidebar, which does not resize.
    static let sidebarWidth: CGFloat = 232

    private func step(_ by: Int) -> KeyPress.Result {
        let all = rows
        guard !all.isEmpty else { return .ignored }
        let i = all.firstIndex { selected($0) } ?? 0
        pick(all[min(all.count - 1, max(0, i + by))])
        return .handled
    }
}

enum SettingsPage: String, CaseIterable, Identifiable, Hashable {
    case general, capabilities, helper, upscaling, fleet, storage,
         permissions, experimental
    var id: String { rawValue }

    var title: String {
        switch self {
        case .general: String(localized: "General")
        case .capabilities: String(localized: "Capabilities")
        case .helper: String(localized: "Agentic Helper")
        case .upscaling: String(localized: "Upscaling")
        case .fleet: String(localized: "Fleet")
        case .storage: String(localized: "Storage")
        case .permissions: String(localized: "Permissions")
        case .experimental: String(localized: "Experimental")
        }
    }

    var symbol: String {
        switch self {
        case .general: "gearshape.fill"
        case .capabilities: "sparkles"
        case .helper: "brain"
        case .upscaling: "arrow.up.left.and.arrow.down.right"
        case .fleet: "point.3.connected.trianglepath.dotted"
        case .storage: "internaldrive.fill"
        case .permissions: "hand.raised.fill"
        case .experimental: "flask.fill"
        }
    }

    var color: Color {
        switch self {
        case .general: .gray
        case .capabilities: .purple
        case .helper: .pink
        case .upscaling: .green
        case .fleet: .teal
        case .storage: .blue
        case .permissions: .indigo
        case .experimental: .orange
        }
    }

    /// What the page's card says it is for.
    var blurb: String {
        switch self {
        case .general:
            String(localized: "This Mac, where Valtz keeps projects, models and its cache, and the project that is open.")
        case .capabilities:
            String(localized: "What each model family makes on this Mac, and every resource it runs with: download it, or use it from where it already is.")
        case .helper:
            String(localized: "The model that writes prompts with you, reads what you ask for and watches your clips: which one, and how it runs.")
        case .upscaling:
            String(localized: "How Render Upscaled restores a layer at the size it is shown — FlashVSR+ or FlashVSR 1.1 for clips, or Open-H3-Regenerate-2K to regenerate one; VOSR for pictures: which one, and the arithmetic each runs with.")
        case .fleet:
            String(localized: "The Valtz Macs of this network as one: a job this Mac cannot run, or not now, goes to a member that can, and this Mac takes theirs when it is free.")
        case .storage:
            String(localized: "What Valtz keeps on the internal SSD: models, projects and the cache, each sized.")
        case .permissions:
            String(localized: "What Valtz may ask macOS to use — the camera, the microphone, the Mac's sound, your folders — whether it is allowed now, and where Valtz needs it.")
        case .experimental:
            String(localized: "What is still being built: switched on here to try it. It may change, or go.")
        }
    }
}

/// Holds the sidebar at its width. AppKit restores the width a split
/// view last had (its autosave, in the app's defaults) over the one the
/// column asks for, and a narrower one, once saved, came back every
/// time: "Capabilities" wrapped. The divider is put back where the
/// design has it once the window is up.
private struct SidebarWidthKeeper: NSViewRepresentable {
    let width: CGFloat

    func makeNSView(context: Context) -> NSView { Probe(width: width) }
    func updateNSView(_ v: NSView, context: Context) {}

    final class Probe: NSView {
        let width: CGFloat
        init(width: CGFloat) {
            self.width = width
            super.init(frame: .zero)
        }
        required init?(coder: NSCoder) { nil }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            guard window != nil else { return }
            DispatchQueue.main.async { [weak self] in self?.settle() }
        }

        private func settle() {
            var v: NSView? = superview
            while let s = v, !(s is NSSplitView) { v = s.superview }
            guard let split = v as? NSSplitView,
                  let side = split.arrangedSubviews.first,
                  abs(side.frame.width - width) > 0.5 else { return }
            split.setPosition(width, ofDividerAt: 0)
        }
    }
}

/// System Settings' toolbar: the arrows at the page's leading edge, its
/// name beside them, in the title bar's row. A Settings scene's window
/// comes with the PREFERENCE style instead -- its items centred on a row
/// of their own under the title.
private struct UnifiedToolbar: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView { Probe() }
    func updateNSView(_ v: NSView, context: Context) {}

    final class Probe: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            // At once, before the window's first layout: set later, the
            // toolbar kept the items where the other style had them.
            guard let w = window, w.toolbarStyle != .unified else { return }
            w.toolbarStyle = .unified
            w.titleVisibility = .visible
        }
    }
}

/// The Settings window CENTRED on the editor's each time it opens -- it
/// came back where it was last left, on a large screen far from the
/// window it serves -- inside the editor's screen; and CLOSED BY ESCAPE,
/// but where Escape is something else's: an input method composing, the
/// search field's words (cleared first), a sheet over it.
private struct SettingsWindowKeeper: NSViewRepresentable {
    let model: AppModel

    func makeNSView(context: Context) -> NSView { Probe(model: model) }
    func updateNSView(_ v: NSView, context: Context) {}

    final class Probe: NSView {
        private weak var model: AppModel?
        /// Placed since it was last shown.
        private var placed = false
        private var observers: [NSObjectProtocol] = []
        private var monitor: Any?

        init(model: AppModel) {
            self.model = model
            super.init(frame: .zero)
        }

        required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            for o in observers { NotificationCenter.default.removeObserver(o) }
            observers = []
            if let monitor { NSEvent.removeMonitor(monitor) }
            monitor = nil
            guard let w = window else { return }
            placed = false
            let nc = NotificationCenter.default
            // Placed as it comes on screen (or is made key: an app in the
            // background opens it unseen), once until it closes -- not
            // again when uncovered, or clicked after being moved.
            observers = [
                nc.addObserver(forName: NSWindow.didBecomeKeyNotification,
                               object: w, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.place() }
                },
                nc.addObserver(
                    forName: NSWindow.didChangeOcclusionStateNotification,
                    object: w, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated {
                        if self?.window?.occlusionState.contains(.visible)
                            == true {
                            self?.place()
                        }
                    }
                },
                nc.addObserver(forName: NSWindow.willCloseNotification,
                               object: w, queue: .main) { [weak self] _ in
                    MainActor.assumeIsolated { self?.placed = false }
                },
            ]
            monitor = NSEvent.addLocalMonitorForEvents(matching: .keyDown) {
                [weak self] e in
                let taken = MainActor.assumeIsolated {
                    self?.escape(e) ?? false
                }
                return taken ? nil : e
            }
            if w.isKeyWindow || w.occlusionState.contains(.visible) {
                place()
            }
        }

        /// Over the editor's middle, kept on its screen.
        func place() {
            guard !placed, let w = window else { return }
            placed = true
            guard let ed = model?.editorWindow, ed.isVisible, ed !== w
            else { return }
            let e = ed.frame, f = w.frame
            var o = NSPoint(x: (e.midX - f.width / 2).rounded(),
                            y: (e.midY - f.height / 2).rounded())
            if let vis = (ed.screen ?? w.screen)?.visibleFrame {
                o.x = min(max(o.x, vis.minX), max(vis.minX, vis.maxX - f.width))
                o.y = min(max(o.y, vis.minY), max(vis.minY, vis.maxY - f.height))
            }
            w.setFrameOrigin(o)
        }

        /// Escape with no modifier, to this window: closed -- true, taken.
        private func escape(_ e: NSEvent) -> Bool {
            let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
                .subtracting([.capsLock, .numericPad, .function])
            guard e.keyCode == 53, mods.isEmpty, let w = window,
                  e.window === w, w.attachedSheet == nil else { return false }
            if let tv = w.firstResponder as? NSTextView {
                if tv.hasMarkedText() { return false }
                if tv.isFieldEditor, let f = tv.delegate as? NSSearchField,
                   !f.stringValue.isEmpty { return false }
            }
            w.performClose(nil)
            return true
        }
    }
}

/// Valtz's accent, as its asset catalog has it: what the system's own
/// accent colour, a person's choice in System Settings, does not replace.
extension Color {
    static let valtzAccent = Color("AccentColor")
}

/// A row of the sidebar: a page, its icon and name -- the selected one
/// on the accent, its name bold; selected too while a page under it is
/// shown -- or, found by a search, a page under one, set in under it, or
/// a setting found on a page, set in further, what was found in it bold.
private struct SidebarRow: View {
    let result: SettingsResult
    let query: String
    let selected: Bool
    let action: () -> Void
    @Environment(\.colorScheme) private var scheme

    private var place: SettingsPlace { result.place }

    private var below: Bool {
        if case .page = place { return false }
        return true
    }

    private var ink: Color {
        selected ? (scheme == .dark ? Color.black : Color.white)
                 : Color.primary
    }

    var body: some View {
        Button(action: action) {
            HStack(spacing: 7) {
                if let s = result.setting {
                    Spacer().frame(width: below ? 34 : 20)
                    Text(SettingsSearchText.emphasized(s, query))
                        .font(.system(size: 12))
                        .foregroundStyle(selected ? ink : Color.secondary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                } else {
                    if below {
                        Spacer().frame(width: 20)
                    } else {
                        SettingsIcon(symbol: place.page.symbol,
                                     color: place.page.color)
                    }
                    Text(verbatim: place.title)
                        .font(.system(size: 13,
                                      weight: selected ? .semibold
                                                       : .regular))
                        .foregroundStyle(ink)
                }
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 9)
            .frame(height: result.setting != nil ? 24 : below ? 28 : 32)
            .background {
                if selected {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .fill(Color.valtzAccent)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(selected ? [.isSelected] : [])
    }
}

/// A sidebar icon as System Settings draws one: a white symbol on a
/// rounded square of its colour -- 20 pt in the sidebar, larger on a
/// page's card.
struct SettingsIcon: View {
    let symbol: String
    let color: Color
    var size: CGFloat = 20

    var body: some View {
        Image(systemName: symbol)
            .font(.system(size: size * 0.55, weight: .semibold))
            .foregroundStyle(.white)
            .frame(width: size, height: size)
            .background(color.gradient,
                        in: RoundedRectangle(cornerRadius: size * 0.26,
                                             style: .continuous))
    }
}

/// A page's card, at its top, as System Settings opens a pane: its icon,
/// large, its name and what it is for.
struct SettingsHeader: View {
    let page: SettingsPage

    var body: some View {
        VStack(spacing: 8) {
            SettingsIcon(symbol: page.symbol, color: page.color, size: 48)
                .shadow(color: .black.opacity(0.12), radius: 3, y: 1)
            Text(page.title)
                .font(.system(size: 24, weight: .bold))
            Text(page.blurb)
                .font(.system(size: 13))
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .fixedSize(horizontal: false, vertical: true)
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, 18)
        .padding(.horizontal, 12)
    }
}

private struct GeneralSettings: View {
    @Bindable var model: AppModel
    /// The language chosen for Valtz ("" follows the system's).
    @State private var language = GeneralSettings.firstLanguage
    @State private var confirmRestart = false

    /// What is chosen -- in a scripted snapshot run, what
    /// VALTZ_SNAPSHOT_LANGUAGE says, nothing saved being read.
    private static var firstLanguage: String {
        let env = ProcessInfo.processInfo.environment
        if env["VALTZ_SNAPSHOT"] != nil {
            return env["VALTZ_SNAPSHOT_LANGUAGE"] ?? ""
        }
        return L10n.chosen ?? ""
    }

    /// The language Valtz will be in at its next launch.
    private var nextLanguage: String {
        language.isEmpty ? L10n.systemDefault : language
    }

    var body: some View {
        Form {
            Section { SettingsHeader(page: .general) }
            Section {
                Picker("Language", selection: $language) {
                    Text("System Default (\(L10n.name(L10n.systemDefault)))")
                        .tag("")
                    Divider()
                    ForEach(L10n.available, id: \.self) { tag in
                        Text(verbatim: L10n.name(tag)).tag(tag)
                    }
                }
                .onChange(of: language) { _, v in
                    L10n.choose(v.isEmpty ? nil : v)
                }
                .settingsMark(String(localized: "Language"))
                if nextLanguage != L10n.uiLanguage {
                    LabeledContent {
                        Button("Restart Now") {
                            if model.quitLosesWork {
                                confirmRestart = true
                            } else {
                                model.relaunch()
                            }
                        }
                    } label: {
                        Text("Valtz will be in \(L10n.name(nextLanguage)) when it opens again.")
                            .foregroundStyle(.secondary)
                    }
                }
            } header: {
                Text("Language")
            }
            .confirmationDialog("Restart Valtz?",
                                isPresented: $confirmRestart) {
                Button("Restart") { model.relaunch() }
            } message: {
                Text("The pictures made in this session are deleted when Valtz quits. Save anything you want to keep first.")
            }
            // What a result landing does (AppModel.landed, revealTasks).
            Section {
                Picker("When a generation finishes on a composition that has content",
                       selection: $model.landingOpensTimeline) {
                    Text("Switch to the Timeline").tag(true)
                    Text("Do Nothing").tag(false)
                }
                .settingsMark(String(localized: "Timeline"),
                              String(localized: "When a generation finishes on a composition that has content"))
                Group {
                    Toggle("Highlight the timeline and prompt switch when it switches by itself",
                           isOn: $model.landingFlashesSwitch)
                        .disabled(!model.landingOpensTimeline)
                        .settingsMark(String(localized: "Highlight the timeline and prompt switch when it switches by itself"))
                    Toggle("Show Assets when several tasks are queued",
                           isOn: $model.revealsTasks)
                        .settingsMark(String(localized: "Assets"),
                                      String(localized: "Show Assets when several tasks are queued"))
                    Toggle("Show Assets once more than one result has been generated",
                           isOn: $model.revealsResults)
                        .settingsMark(String(localized: "Show Assets once more than one result has been generated"))
                    Toggle("Play a generated clip as soon as it lands",
                           isOn: $model.autoplaysNewClips)
                        .settingsMark(String(localized: "Play"),
                                      String(localized: "Play a generated clip as soon as it lands"))
                    Toggle("Start it muted",
                           isOn: $model.mutesNewClips)
                        .disabled(!model.autoplaysNewClips)
                        .settingsMark(String(localized: "Muted"),
                                      String(localized: "Start it muted"))
                }
                .toggleStyle(.checkbox)
            } header: {
                Text("Generations")
                    .settingsMark(String(localized: "Generations"))
            } footer: {
                Text("The timeline shows a clip's or a sound's composition, or a still with pages.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            // Whose words the prompt box shows (AppModel.promptHistory).
            Section {
                Picker("Prompt history", selection: $model.promptHistory) {
                    Text("Each target keeps its own")
                        .tag(AppModel.PromptHistory.perTarget)
                    Text("The agent apart from generations")
                        .tag(AppModel.PromptHistory.agentApart)
                    Text("One for all targets")
                        .tag(AppModel.PromptHistory.shared)
                }
                .settingsMark(String(localized: "Prompt history"))
            } header: {
                Text("Prompt Box")
                    .settingsMark(String(localized: "Prompt Box"))
            } footer: {
                Text("Switching between Video, Image, Audio and Agent puts back the words last left there -- or an empty box. The media in the prompt's row stay.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Section {
                LabeledContent("Version") {
                    Text(verbatim: model.updater.version)
                        .monospacedDigit()
                }
                .settingsMark(String(localized: "Version"))
                if model.updater.canCheck {
                    Toggle("Check for updates automatically",
                           isOn: Binding(
                               get: { model.updater.checksAutomatically },
                               set: { model.updater.setChecksAutomatically($0) }))
                        .settingsMark(String(localized: "Check for updates automatically"))
                    LabeledContent {
                        Button("Check Now") {
                            model.updater.checkForUpdates()
                        }
                    } label: {
                        Text("Updates are downloaded from GitHub and installed when you agree.")
                            .foregroundStyle(.secondary)
                    }
                }
            } header: {
                Text("Updates")
                    .settingsMark(String(localized: "Updates"))
            }
            if let hw = model.hardware {
                Section {
                    LabeledContent("Chip", value: hw.chip)
                        .settingsMark(String(localized: "Chip"))
                    LabeledContent("Memory", value: "\(hw.ramGb) GB")
                        .settingsMark(String(localized: "Memory"))
                    LabeledContent("GPU") {
                        Text(hw.gpuMatrixCores
                             ? "\(hw.gpuCores) cores · matrix cores"
                             : "\(hw.gpuCores) cores")
                    }
                    .settingsMark(String(localized: "GPU"))
                    if let ane = hw.aneCores, ane > 0 {
                        LabeledContent("Neural Engine") {
                            Text("\(ane) cores")
                        }
                        .settingsMark(String(localized: "Neural Engine"))
                    }
                    LabeledContent("Engine", value: model.engine)
                        .settingsMark(String(localized: "Engine"))
                } header: {
                    Text("This Mac")
                        .settingsMark(String(localized: "This Mac"))
                }
            }
            // How much wired memory the local models may hold (none to
            // set without an engine).
            if let wired = model.wiredLimit, wired.max > 0,
               wired.inForce > 0 {
                WiredLimitSection(model: model, wired: wired)
            }
            if model.locations != nil {
                // Each folder put elsewhere here (StorageLocations).
                Section {
                    StorageLocationRows(model: model)
                } header: {
                    Text("Storage")
                        .settingsMark(String(localized: "Storage"))
                }
            }
            Section {
                LabeledContent("Open", value: model.projectName)
                    .settingsMark(String(localized: "Open"))
            } header: {
                Text("Project")
                    .settingsMark(String(localized: "Project"))
            }
        }
        .formStyle(.grouped)
        // What is wired moves as models load and go: read while shown.
        .task {
            while !Task.isCancelled {
                model.refreshWiredLimit()
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

}

/// Settings › General › Memory: how much WIRED memory -- memory macOS
/// cannot compress or swap out, which vpipe runs its models from -- the
/// helper, the agent and the picture, clip and sound models may hold
/// (Controller::wired_limit, DESIGN §10a). Automatic is vpipe's own; a
/// limit set is a whole number of GB up to what the GPU keeps resident,
/// taken as the slider is let go, and what is wired now is beside it.
private struct WiredLimitSection: View {
    @Bindable var model: AppModel
    let wired: WiredLimit
    /// The slider's place while it moves (an index into `stops`).
    @State private var moving: Double?
    @State private var editing = false

    private static let gb: Int64 = 1 << 30

    /// The limits offered: each whole GB from the least, then the most.
    private var stops: [Int64] {
        var s: [Int64] = []
        var g = (wired.min + Self.gb - 1) / Self.gb
        while g * Self.gb < wired.max {
            s.append(g * Self.gb)
            g += 1
        }
        s.append(wired.max)
        return s
    }

    /// The stop nearest `bytes`.
    private func index(of bytes: Int64) -> Int {
        let s = stops
        return s.indices.min(by: { abs(s[$0] - bytes) < abs(s[$1] - bytes) })
            ?? 0
    }

    private func commit() {
        guard let m = moving else { return }
        moving = nil
        let s = stops
        let v = s[Swift.min(Swift.max(Int(m.rounded()), 0), s.count - 1)]
        if v != wired.chosen { model.setWiredLimit(v) }
    }

    nonisolated static func text(_ bytes: Int64) -> String {
        ByteCountFormatter.string(fromByteCount: bytes, countStyle: .memory)
    }

    var body: some View {
        Section {
            Picker("Wired memory for models", selection: Binding(
                get: { wired.chosen == 0 },
                set: { automatic in
                    model.setWiredLimit(automatic
                        ? 0 : stops[index(of: wired.automatic)])
                })) {
                Text("Automatic (\(Self.text(wired.automatic)))").tag(true)
                Text("Set a Limit").tag(false)
            }
            .settingsMark(String(localized: "Wired memory"),
                          String(localized: "Wired memory for models"))
            if wired.chosen != 0, stops.count > 1 {
                LabeledContent("Limit") {
                    HStack(spacing: 10) {
                        Slider(value: Binding(
                                   get: { moving
                                          ?? Double(index(of: wired.chosen)) },
                                   set: {
                                       moving = $0
                                       // An arrow key moves it with no
                                       // drag to end: taken at once.
                                       if !editing { commit() }
                                   }),
                               in: 0...Double(stops.count - 1), step: 1) {
                            Text("Limit")
                        } minimumValueLabel: {
                            Text(verbatim: Self.text(stops[0]))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        } maximumValueLabel: {
                            Text(verbatim: Self.text(wired.max))
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        } onEditingChanged: { now in
                            editing = now
                            if !now { commit() }
                        }
                        .labelsHidden()
                        Text(verbatim: Self.text(moving.map {
                            stops[Swift.min(Int($0.rounded()),
                                            stops.count - 1)]
                        } ?? wired.chosen))
                            .monospacedDigit()
                            .frame(minWidth: 52, alignment: .trailing)
                    }
                }
                .settingsMark(String(localized: "Limit"))
            }
            LabeledContent("Wired now") {
                Text("\(Self.text(wired.used)) of \(Self.text(wired.inForce))")
                    .monospacedDigit()
            }
            .settingsMark(String(localized: "Wired now"))
            if wired.pending {
                Text("The lower limit applies once the work under way ends.")
                    .foregroundStyle(.secondary)
            }
        } header: {
            Text("Memory")
                .settingsMark(String(localized: "Memory"))
        } footer: {
            Text("The helper, the agent and the picture, video and sound models run from wired memory, which macOS cannot compress or swap out. A lower limit leaves more memory to other apps; past it, a model reads its weights from disk as it runs, more slowly. Automatic is three quarters of this Mac's memory, at most what its GPU keeps resident.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }
}
