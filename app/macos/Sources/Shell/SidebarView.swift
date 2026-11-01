import AppKit
import SwiftUI

/// The window's left column, the system's sidebar:
///
///   the project   its name -- the anonymous session's renamed here or in
///                 the title -- what it is, and its menu (Save, Save As…,
///                 Show in Finder, Start Over…)
///   Project       the Stage, the Atlas, the Log: picked, the stage
///                 column shows it
///   Recents       the projects opened or saved lately: picked, opened
///                 (the one open asked about first)
///   Assets        everything the project holds (AssetsSection): picked,
///                 viewed on the stage, its row lit while it is there;
///                 double-clicked, opened (made active, or a prompt put
///                 in the box)
///
/// Its search filters Assets by name; ⌘F (Edit › Find) puts the keyboard
/// in it, showing the sidebar if it was hidden.
struct SidebarView: View {
    @Bindable var model: AppModel
    @FocusState private var listFocused: Bool
    @FocusState private var searchFocused: Bool

    var body: some View {
        List(selection: Binding<SidebarItem?>(
            get: { model.sidebarPick },
            set: { if let i = $0 { model.select(i) } })) {
            ProjectHeader(model: model)
                .listRowInsets(EdgeInsets(top: 4, leading: 4, bottom: 8,
                                          trailing: 4))
                .selectionDisabled()
            Section("Project") {
                ForEach(Screen.all) { s in
                    Label(s.label, systemImage: s.symbol)
                        .tag(s.item)
                }
            }
            RecentsSection(model: model)
            AssetsSection(model: model)
        }
        .listStyle(.sidebar)
        // A double-click on an asset's row (its menus are the rows' own).
        .contextMenu(forSelectionType: SidebarItem.self) { _ in
        } primaryAction: { items in
            guard items.count == 1, case .asset(let id) = items.first
            else { return }
            model.assetOpened(id)
        }
        .searchable(text: $model.assetFilter, placement: .sidebar,
                    prompt: Text("Filter Assets"))
        .searchFocused($searchFocused)
        .onChange(of: model.findRequest) {
            if model.chrome.sidebar == .detailOnly && !model.viewOnly {
                withAnimation(AppModel.motion) { model.chrome.sidebar = .all }
            }
            searchFocused = true
        }
        // A viewing takes the list's focus: Escape there puts the active
        // asset back (as a click away from the list and the stage does:
        // AppModel.endViewingIfOutside).
        .focused($listFocused)
        .onChange(of: listFocused) { _, f in model.assetListFocused = f }
        .onDisappear { model.assetListFocused = false }
        .onKeyPress(.escape) {
            guard model.viewedAsset != nil else { return .ignored }
            model.endViewing()
            return .handled
        }
        .onChange(of: model.viewedAsset) { _, v in
            if v != nil { listFocused = true }
        }
        .onGeometryChange(for: CGRect.self) {
            $0.frame(in: .global)
        } action: { model.assetListFrame = $0 }
        // Media from outside -- the Finder's files, a browser's picture,
        // an app's bytes, a link, words -- into the project; a row of
        // Assets: to the top of the list. The one destination over the
        // list but a folder's row: AppKit hands a drag to the front one.
        .onDrop(of: DroppedMedia.dropTypes,
                delegate: AssetsDrop(model: model, folder: "",
                                     targeted: .constant(false)))
    }

    /// The Project section's rows.
    private struct Screen: Identifiable {
        let item: SidebarItem
        let label: LocalizedStringKey
        let symbol: String
        var id: String { symbol }

        static var all: [Screen] { [
            Screen(item: .stage, label: "Stage", symbol: "photo.on.rectangle"),
            Screen(item: .atlas, label: "Atlas",
                   symbol: "point.3.connected.trianglepath.dotted"),
            Screen(item: .log, label: "Log", symbol: "text.alignleft"),
        ] }
    }
}

/// The project at the sidebar's top: its name, what it is, its menu.
private struct ProjectHeader: View {
    @Bindable var model: AppModel
    @State private var draft = ""
    @FocusState private var editing: Bool

    /// An agent at work holds the project: not renamed meanwhile.
    private var renamable: Bool {
        model.isAnonymous && !model.viewOnly && !model.agentHolds
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: VZ.Space.s) {
            VStack(alignment: .leading, spacing: VZ.Space.xxs) {
                if renamable {
                    // Return, or the keyboard going elsewhere, keeps the
                    // name; Escape or a blank one keeps the old.
                    TextField("Name", text: $draft)
                        .textFieldStyle(.plain)
                        .font(.vzHeading)
                        .focused($editing)
                        .onSubmit { model.renameSession(draft) }
                        .onExitCommand {
                            draft = model.anonymousName
                            editing = false
                        }
                        .onChange(of: editing) { _, e in
                            if !e { model.renameSession(draft) }
                        }
                        .help("Click to rename")
                        .accessibilityLabel(Text("Project name"))
                } else {
                    Text(verbatim: model.windowTitle)
                        .font(.vzHeading)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                Text(kind)
                    .font(.vzMeta)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
            ProjectMenu(model: model)
        }
        .onAppear { draft = model.anonymousName }
        .onChange(of: model.anonymousName) { _, n in draft = n }
    }

    /// What the project is: anonymous, or its composition's kind.
    private var kind: LocalizedStringKey {
        if model.isAnonymous { return "Anonymous Project" }
        guard let v = model.projectViews.first else { return "Project" }
        if v.kind == "audio" { return "Sound Project" }
        return v.isTimeline ? "Timeline Project" : "Still Project"
    }
}

/// The project's menu: Save, Save As…, Show in Finder (a saved project),
/// Start Over… (the anonymous session).
private struct ProjectMenu: View {
    @Bindable var model: AppModel

    var body: some View {
        Menu {
            Button("Save") { model.saveDocument() }
                .disabled(model.projectId == nil
                          || model.closingTasks != nil)
            Button("Save As…") { model.saveDocumentAs() }
                .disabled(model.projectId == nil
                          || model.closingTasks != nil)
            if !model.isAnonymous && !model.projectPath.isEmpty {
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting(
                        [URL(fileURLWithPath: model.projectPath)])
                }
            }
            if model.isAnonymous {
                Divider()
                Button("Start Over…") { model.startOverPressed() }
                    .disabled(model.isBusy || model.closingTasks != nil)
            }
        } label: {
            Label("Project", systemImage: "ellipsis.circle")
                .labelStyle(.iconOnly)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help("Save, save as, or start over")
    }
}

/// The projects opened or saved lately (RecentProjects): picked, opened.
private struct RecentsSection: View {
    @Bindable var model: AppModel

    var body: some View {
        let recents = RecentProjects.shared.existing
        if !recents.isEmpty {
            Section("Recents") {
                ForEach(recents) { p in
                    Label {
                        Text(verbatim: p.name)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    } icon: {
                        Image(systemName: "doc")
                    }
                    .help(p.url.path)
                    .tag(SidebarItem.recent(p.url.path))
                    .contextMenu {
                        Button("Remove from Recents") {
                            RecentProjects.shared.remove(p)
                        }
                        Button("Clear Recents") {
                            RecentProjects.shared.clear()
                        }
                    }
                }
            }
        }
    }
}
