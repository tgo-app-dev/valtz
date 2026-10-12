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
    /// pages it finds and the pages under them it finds.
    private var rows: [SettingsPlace] {
        model.settingsSearch.isEmpty
            ? SettingsPage.allCases.map { .page($0) }
            : SettingsPlace.found(model.settingsSearch, model)
    }

    var body: some View {
        NavigationSplitView {
            ScrollView {
                VStack(spacing: 0) {
                    ForEach(rows, id: \.self) { r in
                        SidebarRow(place: r, selected: selected(r)) {
                            go(r)
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

    /// The row of where Settings is -- or of the page it is under, unless
    /// a search lists that page itself.
    private func selected(_ r: SettingsPlace) -> Bool {
        if r == place { return true }
        return r == .page(place.page) && !rows.contains(place)
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
        let i = all.firstIndex(of: place)
            ?? all.firstIndex(of: .page(place.page)) ?? 0
        go(all[min(all.count - 1, max(0, i + by))])
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
            String(localized: "How Render Upscaled restores a layer at the size it is shown — FlashVSR for clips, VOSR for pictures: the arithmetic each runs with.")
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

/// Valtz's accent, as its asset catalog has it: what the system's own
/// accent colour, a person's choice in System Settings, does not replace.
extension Color {
    static let valtzAccent = Color("AccentColor")
}

/// A row of the sidebar: a page, its icon and name -- the selected one
/// on the accent, its name bold; selected too while a page under it is
/// shown -- or, found by a search, a page under one, set in under it.
private struct SidebarRow: View {
    let place: SettingsPlace
    let selected: Bool
    let action: () -> Void
    @Environment(\.colorScheme) private var scheme

    private var below: Bool {
        if case .page = place { return false }
        return true
    }

    var body: some View {
        Button(action: action) {
            HStack(spacing: 7) {
                if below {
                    Spacer().frame(width: 20)
                } else {
                    SettingsIcon(symbol: place.page.symbol,
                                 color: place.page.color)
                }
                Text(verbatim: place.title)
                    .font(.system(size: 13,
                                  weight: selected ? .semibold : .regular))
                    .foregroundStyle(selected
                                     ? (scheme == .dark ? Color.black
                                                        : Color.white)
                                     : Color.primary)
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 9)
            .frame(height: below ? 28 : 32)
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
            Section("Language") {
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
            }
            .confirmationDialog("Restart Valtz?",
                                isPresented: $confirmRestart) {
                Button("Restart") { model.relaunch() }
            } message: {
                Text("The pictures made in this session are deleted when Valtz quits. Save anything you want to keep first.")
            }
            Section {
                LabeledContent("Version") {
                    Text(verbatim: model.updater.version)
                        .monospacedDigit()
                }
                if model.updater.canCheck {
                    Toggle("Check for updates automatically",
                           isOn: Binding(
                               get: { model.updater.checksAutomatically },
                               set: { model.updater.setChecksAutomatically($0) }))
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
            }
            if let hw = model.hardware {
                Section("This Mac") {
                    LabeledContent("Chip", value: hw.chip)
                    LabeledContent("Memory", value: "\(hw.ramGb) GB")
                    LabeledContent("GPU") {
                        Text(hw.gpuMatrixCores
                             ? "\(hw.gpuCores) cores · matrix cores"
                             : "\(hw.gpuCores) cores")
                    }
                    if let ane = hw.aneCores, ane > 0 {
                        LabeledContent("Neural Engine") {
                            Text("\(ane) cores")
                        }
                    }
                    LabeledContent("Engine", value: model.engine)
                }
            }
            if let p = model.paths {
                Section("Storage") {
                    location("Projects", p.projects)
                    location("Models", p.models)
                    location("Cache", p.cache,
                             detail: ByteCountFormatter.string(
                                fromByteCount: p.cacheBytes, countStyle: .file)
                                + " of "
                                + ByteCountFormatter.string(
                                    fromByteCount: p.cacheBudgetBytes,
                                    countStyle: .file)
                                + (p.cacheInternal ? "" : " — not on the internal SSD"))
                }
            }
            Section("Project") {
                LabeledContent("Open", value: model.projectName)
            }
        }
        .formStyle(.grouped)
    }

    private func location(_ title: String, _ path: String,
                          detail: String? = nil) -> some View {
        LabeledContent(title) {
            HStack {
                VStack(alignment: .trailing, spacing: 2) {
                    Text(path).lineLimit(1).truncationMode(.middle)
                    if let detail {
                        Text(detail).font(.caption).foregroundStyle(.secondary)
                    }
                }
                Button {
                    NSWorkspace.shared.selectFile(
                        nil, inFileViewerRootedAtPath: path)
                } label: { Image(systemName: "arrow.right.circle") }
                    .buttonStyle(.borderless)
                    .help("Show in Finder")
            }
        }
    }
}
