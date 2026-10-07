import AppKit
import ImageIO
import SwiftUI
import UniformTypeIdentifiers

// MARK: - The asset list, in the model

extension AppModel {
    /// What a drag of an asset from the list carries: this, and its id.
    static let assetPrefix = "valtz-asset:"
    /// What a drag from the stage carries (beside its flattened file).
    static let stagePrefix = "valtz-stage"

    /// The asset a drag from the list carries.
    static func draggedAsset(_ s: String) -> String? {
        s.hasPrefix(assetPrefix) ? String(s.dropFirst(assetPrefix.count))
                                 : nil
    }

    /// The assets a drop carries as URLs. A row's "valtz-asset:<id>" has a
    /// scheme, so macOS offers it as a URL as well as text, and a
    /// destination for URLs (files) is the one that gets it: each such
    /// destination asks this first, or the drop is accepted and lost.
    static func draggedAssets(_ urls: [URL]) -> [String] {
        urls.compactMap { draggedAsset($0.absoluteString) }
    }

    /// What the active project uses: the stage's asset, the prompt row's,
    /// and every asset another is made from or shows on a layer.
    var assetsInUse: Set<String> {
        var used = Set(stageAssetsHeld)
        if let s = stageStack?.id { used.insert(s) }
        for item in promptAttachments {
            if let id = assetId(of: item) { used.insert(id) }
        }
        for a in assets {
            used.formUnion(a.inputAssets)
            for l in a.layers ?? [] {
                if let s = l.source { used.insert(s) }
            }
        }
        return used
    }

    /// A GENERATION the project no longer uses -- replaced by another,
    /// nothing made from it, in no prompt: STALE. It stays until removed.
    func isStale(_ a: AssetDTO, used: Set<String>) -> Bool {
        a.isGeneration && a.head > 0 && !used.contains(a.id)
    }

    // MARK: Viewing an asset (a single click)

    /// An asset VIEWED: a single click on its row puts it on the stage --
    /// set down from above, larger and fading in, as a thing put on a
    /// desk -- while the ACTIVE one waits to come back. Another click
    /// views another; the list losing focus (Escape, a click elsewhere,
    /// on its open space) takes it off, the same way reversed, and puts
    /// the active one back, editable. A double-click (or the pencil)
    /// makes it the active one instead.
    func viewAsset(_ a: AssetDTO) {
        guard viewedAsset != a.id else { return }
        // What to come back to: the active one -- kept while one viewed
        // asset follows another.
        let back = viewedAsset == nil ? stageAssetId : viewReturn
        viewedAsset = a.id
        viewReturn = back
        if focusedReference != nil { endActivation(restore: false) }
        swapStageAnimated { [weak self] in
            self?.stageReturn = nil
            self?.putOnStage(a)
        }
    }

    /// The viewed asset off the stage, the active one back on it. Not
    /// when the stage has moved on meanwhile -- the viewed one edited
    /// (its copy made), set active, a result landed: that stays.
    func endViewing() {
        guard let viewed = viewedAsset else { return }
        let back = viewReturn
        viewedAsset = nil
        viewReturn = nil
        guard stageAssetId == viewed else { return }
        swapStageAnimated { [weak self] in
            guard let self else { return }
            if let back, let a = self.assets.first(where: { $0.id == back }) {
                self.putOnStage(a)
            } else {
                self.clearStage()
            }
        }
    }

    /// A click away from the viewing -- anywhere in the editor's window
    /// but the asset list and the stage itself (where the viewed asset is
    /// looked at, zoomed, panned) -- ends it.
    func endViewingIfOutside(_ event: NSEvent) {
        guard viewedAsset != nil, let w = event.window, !(w is NSPanel),
              let content = w.contentView else {
            return
        }
        let p = event.locationInWindow
        let at = CGPoint(x: p.x, y: content.bounds.height - p.y)
        if assetListFrame.contains(at) || stageCardFrame.contains(at) {
            return
        }
        endViewing()
    }

    /// The stage card taken off (lifted, larger, fading out), `change`,
    /// and the new one put down (from above, settling).
    private func swapStageAnimated(_ change: @escaping @MainActor () -> Void) {
        let up = Animation.easeIn(duration: 0.2 * AppModel.animationScale)
        withAnimation(stageVisible ? up : nil) {
            stageLift = 1
        } completion: { [weak self] in
            guard let self else { return }
            change()
            withAnimation(.spring(duration: 0.45 * AppModel.animationScale,
                                  bounce: 0.18)) {
                self.stageLift = 0
            }
        }
    }

    /// What a blank asset is (newBlankAsset).
    enum BlankKind { case timeline, still, sound }

    /// A new, BLANK asset of the list: a composition (a timeline at the
    /// project's frame and rate, else 832 x 480 at 24), a still
    /// composition (the project's frame, else 1024 x 1024) or audio -- a
    /// composition of sound alone, which records -- named in turn
    /// ("Audio 2"), never the project's by itself; made active.
    func newBlankAsset(_ kind: BlankKind) {
        guard let core, let projectId else { return }
        let base = switch kind {
        case .timeline: String(localized: "Composition")
        case .still: String(localized: "Still Composition")
        case .sound: String(localized: "Audio")
        }
        var name = base
        var n = 2
        while assets.contains(where: { $0.name == name }) {
            name = "\(base) \(n)"
            n += 1
        }
        // The project's frame, where it has one.
        let frame = projectViews.first.flatMap { p -> (Int, Int)? in
            guard p.kind != "audio" else { return nil }
            let f = ownFrame(of: p)
            return f.width > 0 && f.height > 0 ? (f.width, f.height) : nil
        }
        var req: [String: Any] = ["still": kind == .still, "claim": false,
                                  "name": name]
        switch kind {
        case .sound:
            req["width"] = 0
            req["height"] = 0
        case .still:
            req["width"] = frame?.0 ?? 1024
            req["height"] = frame?.1 ?? 1024
        case .timeline:
            req["width"] = frame?.0 ?? 832
            req["height"] = frame?.1 ?? 480
            if let r = projectViews.first?.compositionRate,
               projectViews.first?.kind == "video" {
                req["rate_num"] = r.num
                req["rate_den"] = r.den
            }
        }
        let r = core.assetOp(project: projectId, "new-composition", req)
        guard r.ok, let id = r["asset"] as? String else {
            flash(r.message)
            return
        }
        reloadAssets()
        if let a = assets.first(where: { $0.id == id }) { setActive(a) }
    }

    /// A new folder, "New Folder" (numbered past the ones there), its id.
    @discardableResult
    func createFolder() -> String? {
        guard let core, let projectId else { return nil }
        let base = String(localized: "New Folder")
        var name = base
        var n = 2
        while assetFolders.contains(where: { $0.name == name }) {
            name = "\(base) \(n)"
            n += 1
        }
        let r = core.assetOp(project: projectId, "create-folder",
                             ["name": name])
        guard r.ok else {
            flash(r.message)
            return nil
        }
        reloadAssets()
        return r["folder"] as? String
    }

    func renameFolder(_ id: String, _ name: String) {
        assetListOp("rename-folder", ["folder": id, "name": name])
    }

    func deleteFolder(_ id: String) {
        assetListOp("delete-folder", ["folder": id])
    }

    /// Into a folder ("" the top of the list).
    func moveAsset(_ id: String, to folder: String) {
        assetListOp("move", ["asset": id, "folder": folder])
    }

    /// Its name in the list (the core keeps one line; empty is refused).
    func renameAsset(_ id: String, _ name: String) {
        assetListOp("rename", ["asset": id, "name": name])
    }

    /// A folder of the list opened or folded.
    func toggleFolder(_ id: String) {
        withAnimation(Self.motion) {
            if closedFolders.contains(id) {
                closedFolders.remove(id)
            } else {
                closedFolders.insert(id)
            }
        }
    }

    /// Out of the project -- refused (and said why) while it is in use.
    func removeAsset(_ id: String) {
        guard !assetsInUse.contains(id) else {
            let name = assets.first { $0.id == id }?.name ?? ""
            flash(String(localized: "\(name) is in use: on the stage, in the prompt, or a part of another asset."))
            return
        }
        assetListOp("remove", ["asset": id])
    }

    /// Every stale generation, removed.
    func removeStaleAssets() {
        let used = assetsInUse
        for a in assets where isStale(a, used: used) {
            assetListOp("remove", ["asset": a.id], reload: false)
        }
        reloadAssets()
    }

    private func assetListOp(_ op: String, _ extra: [String: Any],
                             reload: Bool = true) {
        guard let core, let projectId else { return }
        let r = core.assetOp(project: projectId, op, extra)
        if !r.ok { flash(r.message) }
        if reload { reloadAssets() }
    }

    /// Assets of the list into the prompt's row -- each once -- as they
    /// are (a look of their own goes with them). Their row ids, in order.
    @discardableResult
    func addAssetReferences(_ ids: [String]) -> [UUID] {
        var list = promptAttachments
        var out: [UUID] = []
        var fresh: [PromptAttachment] = []
        for id in ids {
            // A composition with no file to stand in for it (a still
            // showing a clip's frame) is drawn, as the core draws it.
            guard let a = assets.first(where: { $0.id == id }),
                  let url = a.url ?? (a.isComposition && projectId != nil
                      ? core?.renderedPath(project: projectId!, asset: id)
                      : nil) else { continue }
            if let i = list.firstIndex(where: { assetId(of: $0) == id }) {
                out.append(list[i].id)
                continue
            }
            let kind = ["image", "video", "audio"].contains(a.kind)
                ? a.kind : "file"
            let item = PromptAttachment(id: UUID(), url: url, kind: kind,
                                        asset: id)
            list.append(item)
            fresh.append(item)
            out.append(item.id)
        }
        withAnimation(Self.motion) { promptAttachments = list }
        for item in fresh { loadAssetThumb(item) }
        promptChanged()
        return out
    }

    /// Its thumbnail as it looks (core thumbnail: a look of its own drawn).
    private func loadAssetThumb(_ item: PromptAttachment) {
        guard let core, let projectId, let a = item.asset else { return }
        let id = item.id
        let sound = item.kind == "audio"
        let url = item.url
        Task { [weak self] in
            let cg = sound
                ? await AudioWaveform.image(url, width: 240, height: 120)
                : await Task.detached(priority: .userInitiated) {
                core.thumbnail(project: projectId, asset: a, maxPixels: 240)
                    .flatMap { AppModel.loadImage($0) }
            }.value
            guard let self, let cg else { return }
            self.referenceThumbs[id] = cg
        }
    }

    /// An asset dropped on the stage: INSTANTIATED in what the stage is
    /// working on (core instantiate) -- in its selected layer when that is
    /// blank, else in a new one right above it -- its layers copied in,
    /// editable, one level deep (a layer of it that shows another asset
    /// still shows it); a capture unfrozen. Into nothing, the project.
    /// Never into itself. The layer it went to is selected.
    @discardableResult
    func dropAssetOnStage(_ id: String) -> Bool {
        guard let core, let projectId,
              let a = assets.first(where: { $0.id == id }) else {
            return false
        }
        guard a.kind == "image" || a.kind == "video" || a.kind == "audio"
        else {
            flash(String(localized: "Text does not go on the stage: put it in the prompt."))
            return false
        }
        // Onto what the stage works on -- a flat asset there through its
        // edited copy -- else the project's composition.
        let target = stageStack.map { composedTarget($0.id) }
        var extra: [String: Any] = ["asset": id]
        if let t = target {
            extra["onto"] = t
            extra["at"] = activeLayer
            // On a timeline: where the player is; a still's pages: on
            // the page shown.
            if clipOnStage { extra["offset"] = videoFrame }
            if pagedOnStage { extra["offset"] = stagePage }
        }
        let r = core.assetOp(project: projectId, "instantiate", extra)
        guard r.ok, let held = r["asset"] as? String else {
            flash(r.message)
            return false
        }
        reloadAssets()
        let landed = r["layer"] as? String ?? ""
        if target != nil, let u = assets.first(where: { $0.id == held }) {
            if stageAssetId != held { putOnStage(u) }
            layersChanged()
            revealInspector(.layers)
            selectLayer(landed)
        } else if let u = assets.first(where: { $0.id == held }) {
            putOnStage(u)
        }
        return true
    }

    /// Files dropped on the stage: imported, then each instantiated as an
    /// asset is (a picture or a clip; the first that is one).
    func dropFilesOnStage(_ urls: [URL]) -> Bool {
        guard let url = urls.first(where: {
            ["image", "video"].contains(PromptTextView.kind(of: $0))
        }) else { return false }
        if let a = asset(forFile: url) { return dropAssetOnStage(a.id) }
        importFiles([url])
        Task { @MainActor [weak self] in
            for _ in 0..<600 {
                guard let self else { return }
                if let a = self.asset(forFile: url) {
                    self.dropAssetOnStage(a.id)
                    return
                }
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
        return true
    }

    /// The stage dragged into the prompt: what it shows CAPTURED -- every
    /// layer with its adjustments, crop and turn, keys and trim, frozen --
    /// and staged in the row (a picture, as the one to edit), so the
    /// model gets it as it looks: rendered by the core before it goes
    /// in. (A picture to edit on the stage is in the row already.)
    @discardableResult
    func addStageToPrompt() -> [UUID] {
        guard let c = captureStageAsset(selectedOnly: false) else {
            return []
        }
        let ids = addAssetReferences([c])
        if let first = ids.first,
           let item = promptAttachments.first(where: { $0.id == first }),
           item.kind == "image", !item.isBase {
            toggleBase(first)
        }
        return ids
    }

    /// The stage's picture or clip, its layers -- all, or the selected
    /// ones -- captured as an asset, frozen.
    func captureStage(selectedOnly: Bool) {
        if captureStageAsset(selectedOnly: selectedOnly) != nil {
            flash(String(localized: "Captured in Assets"))
        }
    }

    /// The capture made, its id; nil with nothing to capture.
    func captureStageAsset(selectedOnly: Bool) -> String? {
        guard let core, let projectId,
              let s = stageStack ?? assets.first(where: {
                  $0.id == stageAssetId }) else { return nil }
        flushPanels()
        let name = s.op == "project" ? (projectName ?? s.name) : s.name
        var extra: [String: Any] = [
            "asset": s.id,
            "name": String(localized: "\(name), captured"),
        ]
        if selectedOnly { extra["layers"] = Array(selectedLayers) }
        let r = core.assetOp(project: projectId, "capture", extra)
        guard r.ok, let id = r["asset"] as? String else {
            flash(r.message)
            return nil
        }
        reloadAssets()
        return id
    }
}

// MARK: - The inspector's Assets

/// The inspector's ASSETS: everything the project holds -- pictures, clips
/// and sounds put into Valtz, each generation's result, the modified
/// copies the prompt's pictures were changed through, captures of the
/// stage -- newest first, in FOLDERS of the user's. It is the project's
/// history too: a generation stays as it was made (the project's
/// composition takes the changes), with the picture it was made from as
/// the model got it beside it.
///
///   Set Active      it is what every panel works on, changed in place
///                   (its button, a double-click, its menu)
///   drag a row      into the prompt (its row, or a mention in the text);
///                   onto the stage: INSTANTIATED in the active work --
///                   its layers on top, editable (a capture unfrozen);
///                   onto A or B under the stage: COMPARED there;
///                   onto a folder
///   its base        the small picture at a result's end: an edit's base,
///                   a clip's first picture, as the model got it -- onto
///                   A or B, or into the prompt
///   STALE           a generation nothing uses any more -- replaced, in
///                   no prompt, nothing made from it: marked, kept until
///                   removed (its menu, or Remove Stale)
struct AssetsSection: View {
    @Bindable var model: AppModel
    /// Its notes at the foot (the stacked inspector leaves them out).
    var notes = true
    @State private var renaming: String?
    @State private var dropFolder: String?
    @FocusState private var listFocused: Bool

    var body: some View {
        let used = model.assetsInUse
        // The project's compositions are the project, pinned on top; the
        // others -- versionless, drawn -- are listed as any asset is.
        // Being made (a task's asset, no version yet) too: T0, T1, ...
        let all = model.assets.filter {
            ($0.head > 0 || $0.isComposition || model.task(of: $0.id) != nil)
                && $0.op != "project" && !$0.isProject
        }
            .sorted { $0.created > $1.created }
        let stale = all.filter { model.isStale($0, used: used) }
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 2) {
                // Blank ones, to build up -- or a sound to record into.
                Menu {
                    Button("New Composition") { model.newBlankAsset(.timeline) }
                    Button("New Still Composition") { model.newBlankAsset(.still) }
                    Button("New Audio") { model.newBlankAsset(.sound) }
                } label: {
                    Image(systemName: "plus.rectangle.on.folder")
                        .frame(width: 22, height: 20)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .help("A new, blank asset: a composition (a timeline), a still composition, or audio to record into")
                tool("folder.badge.plus", "New folder") {
                    if let id = model.createFolder() { renaming = id }
                }
                tool("square.and.arrow.down", "Add files to the project") {
                    importFiles()
                }
                Spacer()
                if !stale.isEmpty {
                    Button("Remove Stale (\(stale.count))") {
                        model.removeStaleAssets()
                    }
                    .buttonStyle(.borderless)
                    .help("Remove every generation nothing uses any more")
                }
            }
            .padding(.horizontal, 12)
            if all.isEmpty && model.assetFolders.isEmpty
                && model.tasks.isEmpty {
                ContentUnavailableView(
                    "No assets yet", systemImage: "photo.stack",
                    description: Text("Pictures, clips and sounds put into Valtz, and everything made here, are listed here."))
                    .controlSize(.small)
                    .frame(maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 2) {
                        ForEach(model.projectViews) { v in
                            AssetRow(model: model, asset: v, stale: false,
                                     used: true)
                        }
                        if !model.projectViews.isEmpty {
                            Divider().padding(.vertical, 4)
                        }
                        // Exports queued or running: a row each until the
                        // file is written.
                        ForEach(model.tasks.filter { $0.kind == "export" }) { t in
                            ExportTaskRow(model: model, task: t)
                        }
                        // A folder is ONE item of the list -- its header
                        // and its rows, set in -- so a row's place never
                        // depends on which ForEach the lazy stack matched
                        // it to (an asset moved into a folder kept the
                        // top level's place), and folding slides the rows
                        // up under the header.
                        ForEach(model.assetFolders) { f in
                            folderBlock(f, items: all.filter {
                                $0.folder == f.id }, stale: stale, used: used)
                        }
                        ForEach(all.filter { ($0.folder ?? "").isEmpty }) { a in
                            AssetRow(model: model, asset: a,
                                     stale: stale.contains(a),
                                     used: used.contains(a.id))
                        }
                    }
                    .padding(.horizontal, 8)
                    .padding(.bottom, 8)
                    // A click on the list's open space: the viewed asset
                    // off the stage.
                    .background(Color.clear.contentShape(Rectangle())
                        .onTapGesture { model.endViewing() })
                }
                // Viewing takes the list's focus: Escape there puts the
                // active asset back (as a click away from the list and
                // the stage does: AppModel.endViewingIfOutside).
                .focusable()
                .focused($listFocused)
                .focusEffectDisabled()
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
                // Dropped on the list's open space: to its top.
                .dropDestination(for: String.self) { items, _ in
                    move(items, to: "")
                }
            }
            if notes {
                Text("Set an asset active to edit it. Drag it into the prompt to use it, onto the stage to place its layers in what is there, onto A or B under the stage to compare it, or onto a folder. A generation nothing uses any more is marked stale; it stays until you remove it.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 12)
            }
        }
        // Files from the Finder: into the project. (A row of its own
        // arrives as a URL too: to the top of the list.)
        .dropDestination(for: URL.self) { urls, _ in
            let ids = AppModel.draggedAssets(urls)
            if !ids.isEmpty {
                for id in ids { model.moveAsset(id, to: "") }
                return true
            }
            let files = urls.filter(\.isFileURL)
            guard !files.isEmpty else { return false }
            model.importFiles(files)
            return true
        }
    }

    /// A folder: its header, and -- open -- its assets set in under it.
    /// Folded, they slide up under the header as the block closes.
    private func folderBlock(_ f: FolderDTO, items: [AssetDTO],
                             stale: [AssetDTO],
                             used: Set<String>) -> some View {
        VStack(spacing: 2) {
            folderRow(f, count: items.count)
                .background(Color.valtzPanel)
                .zIndex(1)
            if !model.closedFolders.contains(f.id) {
                VStack(spacing: 2) {
                    ForEach(items) { a in
                        AssetRow(model: model, asset: a,
                                 stale: stale.contains(a),
                                 used: used.contains(a.id))
                    }
                }
                .padding(.leading, 18)
                .transition(.move(edge: .top).combined(with: .opacity))
            }
        }
        .clipped()
    }

    private func folderRow(_ f: FolderDTO, count: Int) -> some View {
        let open = !model.closedFolders.contains(f.id)
        return HStack(spacing: 6) {
            Image(systemName: "chevron.right")
                .font(.system(size: 9, weight: .semibold))
                .rotationEffect(.degrees(open ? 90 : 0))
                .foregroundStyle(.secondary)
                .frame(width: 10)
            Image(systemName: open ? "folder" : "folder.fill")
                .foregroundStyle(.secondary)
            if renaming == f.id {
                NameField(name: f.name) { name in
                    renaming = nil
                    if !name.isEmpty && name != f.name {
                        model.renameFolder(f.id, name)
                    }
                }
            } else {
                Text(verbatim: f.name)
                    .lineLimit(1)
                    .onTapGesture(count: 2) { renaming = f.id }
            }
            Spacer(minLength: 0)
            Text(verbatim: "\(count)")
                .foregroundStyle(.tertiary)
                .monospacedDigit()
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 5)
        .background(RoundedRectangle(cornerRadius: 6)
            .fill(dropFolder == f.id ? Color.accentColor.opacity(0.18)
                                     : .clear))
        .contentShape(Rectangle())
        .onTapGesture {
            guard renaming != f.id else { return }
            model.toggleFolder(f.id)
        }
        .dropDestination(for: String.self) { items, _ in
            move(items, to: f.id)
        } isTargeted: { dropFolder = $0 ? f.id : nil }
        .contextMenu {
            Button("Rename Folder") { renaming = f.id }
            Button("Delete Folder") { model.deleteFolder(f.id) }
        }
        .help("Its assets stay in the project when the folder is deleted")
    }

    private func move(_ items: [String], to folder: String) -> Bool {
        let ids = items.compactMap { AppModel.draggedAsset($0) }
        for id in ids { model.moveAsset(id, to: folder) }
        return !ids.isEmpty
    }

    private func importFiles() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = [.image, .movie, .audio]
        if panel.runModal() == .OK { model.importFiles(panel.urls) }
    }

    private func tool(_ symbol: String, _ help: LocalizedStringKey,
                      action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .frame(width: 22, height: 20)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .help(help)
    }
}

/// A folder's or an asset's name, being typed: Return or clicking away
/// keeps it, Escape leaves it as it was.
private struct NameField: View {
    @State var name: String
    let done: (String) -> Void
    @FocusState private var focused: Bool

    @State private var ended = false

    var body: some View {
        TextField("Name", text: $name)
            .textFieldStyle(.roundedBorder)
            .focused($focused)
            .onSubmit { end(name) }
            .onExitCommand { end("") }
            .onAppear { focused = true }
            .onChange(of: focused) { _, f in
                if !f { end(name) }
            }
    }

    /// Once: Return, then the focus leaving, would say it twice.
    private func end(_ text: String) {
        guard !ended else { return }
        ended = true
        done(text.trimmingCharacters(in: .whitespacesAndNewlines))
    }
}

/// One asset: its picture, its name, what it is -- and STALE when it is a
/// generation nothing uses any more.
private struct AssetRow: View {
    @Bindable var model: AppModel
    let asset: AssetDTO
    let stale: Bool
    let used: Bool
    @State private var image: CGImage?
    @State private var hovering = false

    var body: some View {
        let a = asset
        HStack(spacing: 8) {
            ZStack {
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color.primary.opacity(0.06))
                if let image {
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .clipShape(RoundedRectangle(cornerRadius: 4))
                } else {
                    Image(systemName: symbol)
                        .foregroundStyle(.tertiary)
                }
            }
            .frame(width: 36, height: 36)
            .overlay {
                // Being made: its latest preview, or a spinner before one.
                if let t = task {
                    if let thumb = model.taskThumbs[a.id] {
                        Image(decorative: thumb, scale: 1)
                            .resizable()
                            .aspectRatio(contentMode: .fit)
                            .clipShape(RoundedRectangle(cornerRadius: 4))
                    } else if t.running {
                        ProgressView().controlSize(.small)
                    }
                }
            }
            .overlay(alignment: .bottomTrailing) {
                if a.kind == "video" {
                    Image(systemName: "video.fill")
                        .font(.system(size: 7))
                        .foregroundStyle(.white)
                        .padding(2)
                        .background(Circle().fill(.black.opacity(0.55)))
                        .padding(1)
                }
            }
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    if renaming {
                        NameField(name: a.name) { name in
                            model.renamingAsset = nil
                            if !name.isEmpty && name != a.name {
                                model.renameAsset(a.id, name)
                            }
                        }
                    } else {
                        Text(verbatim: project ? String(localized: "Project")
                                               : a.name)
                            .fontWeight(project ? .semibold : .regular)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
                    if active {
                        Text("Active")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundStyle(Color.accentColor)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 1)
                            .background(Capsule().fill(
                                Color.accentColor.opacity(0.15)))
                            .help("On the stage: every panel works on it")
                    }
                    if stale {
                        Text("Stale")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundStyle(.orange)
                            .padding(.horizontal, 4)
                            .padding(.vertical, 1)
                            .background(Capsule().fill(
                                Color.orange.opacity(0.15)))
                            .help("Nothing uses this generation any more: not the stage, the prompt or another asset. It stays until you remove it.")
                    }
                }
                Text(verbatim: subtitle)
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
            sides
            if let base {
                BaseThumb(model: model, entry: base)
            }
            if let t = task {
                TaskBadge(task: t)
            }
            if task != nil {
                EmptyView()
            } else if hovering && a.isPrompt {
                // A prompt goes into the box, its media named by position.
                Button {
                    model.usePrompt(a)
                } label: {
                    Image(systemName: "text.insert")
                        .font(.system(size: 13))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.borderless)
                .help("Use in the prompt: its references bind to the row's media by position")
            } else if hovering && a.isComposition
                        && (!active || model.viewedAsset == a.id) {
                Button {
                    model.viewedAsset = nil
                    model.viewReturn = nil
                    model.setActive(a)
                } label: {
                    Image(systemName: "pencil.circle")
                        .font(.system(size: 14))
                        .contentShape(Rectangle())
                }
                .buttonStyle(.borderless)
                .help("Set active: edit it on the stage")
            }
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(RoundedRectangle(cornerRadius: 6)
            .fill(active ? Color.accentColor.opacity(0.10)
                  : Color.primary.opacity(hovering ? 0.06 : 0)))
        .opacity(stale ? 0.7 : 1)
        .overlay {
            // Viewed: on the stage now, the active one waiting.
            if model.viewedAsset == a.id {
                RoundedRectangle(cornerRadius: 6)
                    .strokeBorder(Color.accentColor, lineWidth: 1.5)
            }
        }
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture(count: 2) {
            guard !renaming else { return }
            // Another kind than text: the Prompt Editor goes.
            if a.kind != "text" { model.retreatFromEditor() }
            if let t = task {
                model.watchTask(t.job)
            } else if a.isPrompt {
                model.usePrompt(a)
            } else if a.isComposition {
                model.viewedAsset = nil
                model.viewReturn = nil
                model.setActive(a)
            } else {
                // Only a composition is made active: a flat one is viewed.
                model.viewAsset(a)
            }
        }
        // A single click VIEWS it on the stage -- a task's, its preview
        // and progress.
        .onTapGesture(count: 1) {
            guard !renaming else { return }
            // Text: in the Prompt Editor, beside the prompt, to look at.
            // Anything else: the editor goes, and it is viewed.
            if a.kind == "text" {
                model.openTextAsset(a)
                return
            }
            model.retreatFromEditor()
            if let t = task {
                model.watchTask(t.job)
            } else {
                model.viewAsset(a)
            }
        }
        .onDrag {
            NSItemProvider(object: (AppModel.assetPrefix + a.id) as NSString)
        } preview: {
            ZStack {
                if let image {
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                }
            }
            .frame(width: 64, height: 64)
        }
        .contextMenu {
            if let t = task {
                Button("Show on Stage") { model.watchTask(t.job) }
                Button(t.running ? "Stop" : "Remove from Queue") {
                    model.cancel(job: t.job)
                }
                Divider()
                Button("Rename") { model.renamingAsset = a.id }
            } else if a.isPrompt {
                Button("Use in Prompt") { model.usePrompt(a) }
            } else {
                if a.isComposition {
                    Button("Set Active") { model.setActive(a) }
                } else if a.kind == "image" || a.kind == "video"
                    || a.kind == "audio" {
                    // Only a composition is edited: a flat asset through
                    // its edited copy.
                    Button("Edit a Copy") {
                        model.viewedAsset = nil
                        model.viewReturn = nil
                        model.editCopy(a)
                    }
                    .help("A composition of one layer showing it, made active: what it is changed in")
                }
                if a.kind == "image" || a.kind == "video"
                    || a.kind == "audio", !project {
                    Button("Place on the Stage") {
                        model.dropAssetOnStage(a.id)
                    }
                }
                if a.isComposition && !project {
                    Button("Use as Project") { model.useAsProject(a) }
                }
                if a.isDrawn || a.assetClass == "generated" {
                    Button("Flatten") { model.flattenAsset(a) }
                        .help("A flat asset of it as it looks: a picture, a movie or a sound")
                }
                // A clip or a song made here, as it was made.
                if a.isGeneration && (a.kind == "video" || a.kind == "audio") {
                    Button("Show on Stage") { model.showMadeOnStage(a) }
                }
                Button("Add to Prompt") { model.addAssetReferences([a.id]) }
                if a.kind == "image" {
                    Divider()
                    Button("Compare as A") { model.compareAsset(a, in: .a) }
                        .disabled(!model.stageTakesCompare)
                    Button("Compare as B") { model.compareAsset(a, in: .b) }
                        .disabled(!model.stageTakesCompare)
                }
                if let base {
                    Menu("Base as Sent") {
                        Button("Compare as A") { model.compare(base, in: .a) }
                            .disabled(!model.stageTakesCompare)
                        Button("Compare as B") { model.compare(base, in: .b) }
                            .disabled(!model.stageTakesCompare)
                        Button("Show in Finder") {
                            NSWorkspace.shared.activateFileViewerSelecting(
                                [base.url])
                        }
                    }
                }
            }
            if let url = a.url, !project, a.kind != "text" {
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                }
            }
            // The project itself is not named, filed or removed here.
            if !project && task == nil {
                Button("Rename") { model.renamingAsset = a.id }
                Menu("Move To") {
                    Button("Top of the List") { model.moveAsset(a.id, to: "") }
                        .disabled((a.folder ?? "").isEmpty)
                    if !model.assetFolders.isEmpty { Divider() }
                    ForEach(model.assetFolders) { f in
                        Button(f.name) { model.moveAsset(a.id, to: f.id) }
                            .disabled(a.folder == f.id)
                    }
                }
                Divider()
                Button("Remove from Project") { model.removeAsset(a.id) }
                    .disabled(used)
            }
        }
        .help(used ? Text("In use: on the stage, in the prompt, or a part of another asset")
                   : Text(verbatim: a.name))
        .task(id: "\(a.id)-\(a.modified)-\(a.head)") {
            guard let core = model.core, let project = model.projectId
            else { return }
            if a.kind == "audio", let url = a.url {
                image = await AudioWaveform.image(url, width: 192, height: 96)
                return
            }
            // A prompt is words: its symbol is its picture.
            if a.kind == "text" { return }
            let id = a.id
            image = await Task.detached(priority: .utility) {
                core.thumbnail(project: project, asset: id, maxPixels: 96)
                    .flatMap { AppModel.loadImage($0) }
            }.value
        }
    }

    /// The task making it, while it is queued or running.
    private var task: TaskDTO? { model.task(of: asset.id) }

    /// Its name being typed (the menu's Rename).
    private var renaming: Bool {
        model.renamingAsset == asset.id && !project
    }

    /// The ACTIVE composition: what every panel works on, and where a
    /// generation lands (a flat asset is never active, only viewed).
    private var active: Bool {
        model.activeComposition?.id == asset.id
    }

    /// The picture it was made from, as the model got it.
    private var base: HistoryEntryDTO? { model.baseEntry(of: asset) }

    /// "A" / "B" while the compare shows it on that side.
    @ViewBuilder
    private var sides: some View {
        let inA = model.stage.aCompared == asset.id
        let inB = model.stage.bCompared == asset.id
        if inA || inB {
            HStack(spacing: 2) {
                if inA { SideBadge(side: "A") }
                if inB { SideBadge(side: "B") }
            }
        }
    }

    /// The project's own composition: its row is the project.
    private var project: Bool { asset.op == "project" }

    private var symbol: String {
        switch asset.kind {
        case "video": "film"
        case "audio": "waveform"
        case "image": "photo"
        case "text": "text.quote"
        default: "doc"
        }
    }

    /// "Prompt · made 3 · kept as it is": what a prompt has made; once
    /// anything, it is not changed in place.
    private var promptSubtitle: String {
        let n = asset.uses ?? 0
        guard n > 0 else {
            return String(localized: "Prompt · nothing made from it yet")
        }
        let made = String(n)
        return String(localized: "Prompt · made \(made) · kept as it is")
    }

    /// What the project's layer 0 shows.
    private var layer0Name: String {
        asset.layerStack.first { $0.id.isEmpty }?.source
            .flatMap { id in model.assets.first { $0.id == id }?.name } ?? "—"
    }

    /// When it was made: "12:38" today, else "Oct 3, 12:38".
    private var made: String {
        let d = Date(timeIntervalSince1970: Double(asset.created) / 1000)
        return Calendar.current.isDateInToday(d)
            ? d.formatted(date: .omitted, time: .shortened)
            : d.formatted(.dateTime.month(.abbreviated).day().hour()
                .minute())
    }

    /// "Generated 12:38 · 832 × 480", "Edited copy · …", "Capture · 2
    /// layers".
    private var subtitle: String {
        let a = asset
        // Being made: what it is doing, or how many run before it.
        if let t = task {
            if t.running {
                if let p = model.jobs[t.job]?.phase {
                    return p.caption(kind: a.kind == "video" ? .video
                                     : a.kind == "audio" ? .audio : .image)
                }
                return String(localized: "Preparing…")
            }
            return t.position == 1
                ? String(localized: "Queued · next")
                : String(localized: "Queued · \(String(t.position)) ahead")
        }
        let what: String = switch a.op {
        case _ where a.isProject:
            String(localized: "Layer 0: \(layer0Name)")
        case _ where a.isMarkupAsset: String(localized: "Markup")
        case _ where a.isComposition:
            a.kind == "audio" ? String(localized: "Composition · sound")
                : a.isTimeline ? String(localized: "Composition")
                : String(localized: "Still composition")
        case _ where a.from?.asset != nil:
            String(localized: "Flattened")
        case "generate-image", "generate-video", "generate-audio",
             "generate-speech":
            String(localized: "Generated \(made)")
        case "edit-image": String(localized: "Edited by a model \(made)")
        default:
            a.isPrompt ? promptSubtitle
                : a.kind == "audio" ? String(localized: "Sound")
                : String(localized: "Imported")
        }
        var parts = [what]
        if a.isComposition, a.kind != "audio", let f = a.ownFrame {
            parts.append("\(f.w) × \(f.h)")
        } else if !a.isComposition, let f = a.info?.frame, f.w > 0 {
            parts.append("\(f.w) × \(f.h)")
        }
        // A timeline's own length at its own rate -- not its stand-in's
        // (a guide of a clip's last 2.3 s showed the whole clip's 5.2 s).
        if a.isTimeline, a.kind == "video", let n = a.timeline ?? a.length,
           let r = a.compositionRate, n > 0 {
            parts.append(String(format: "%.1f", Double(n) / r.fps) + " s")
        } else if a.kind == "video", let n = a.info?.frames,
           let r = a.info?.frameRate, n > 0 {
            parts.append(String(format: "%.1f", Double(n) / r.fps) + " s")
        }
        let n = (a.layers ?? []).count
        if n > 1 {
            parts.append(String(localized: "\(String(n)) layers"))
        }
        return parts.joined(separator: " · ")
    }
}

/// A task's place in the queue -- T0 the one running, T1 next, ... --
/// on the right of its row.
private struct TaskBadge: View {
    let task: TaskDTO

    var body: some View {
        Text(verbatim: task.badge)
            .font(.system(size: 9, weight: .bold).monospacedDigit())
            .foregroundStyle(task.running ? Color.white : Color.accentColor)
            .padding(.horizontal, 5)
            .frame(height: 16)
            .background(Capsule().fill(task.running
                ? Color.accentColor : Color.accentColor.opacity(0.15)))
            .help(task.running
                  ? Text("Running now. Click the row to watch it on the stage.")
                  : Text("In the queue: it runs when the tasks before it are done."))
    }
}

/// An export in the task queue: the file it writes, until it is written.
private struct ExportTaskRow: View {
    @Bindable var model: AppModel
    let task: TaskDTO
    @State private var hovering = false

    var body: some View {
        HStack(spacing: 8) {
            ZStack {
                RoundedRectangle(cornerRadius: 4)
                    .fill(Color.primary.opacity(0.06))
                Image(systemName: "square.and.arrow.up")
                    .foregroundStyle(.secondary)
            }
            .frame(width: 36, height: 36)
            VStack(alignment: .leading, spacing: 1) {
                Text(verbatim: file)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(verbatim: subtitle)
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer(minLength: 0)
            TaskBadge(task: task)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(RoundedRectangle(cornerRadius: 6)
            .fill(model.exportShow?.job == task.job
                  ? Color.accentColor.opacity(0.10)
                  : Color.primary.opacity(hovering ? 0.06 : 0)))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture { model.watchTask(task.job) }
        .contextMenu {
            Button("Show on Stage") { model.watchTask(task.job) }
            Button(task.running ? "Stop" : "Remove from Queue") {
                model.cancel(job: task.job)
            }
        }
        .help(Text(verbatim: task.destination ?? file))
    }

    private var file: String {
        ((task.destination ?? task.title) as NSString).lastPathComponent
    }

    private var subtitle: String {
        if task.running {
            if let p = model.jobs[task.job]?.phase {
                return p.caption(kind: .video)
            }
            return String(localized: "Export · preparing…")
        }
        return String(localized: "Export · queued")
    }
}

/// A or B: where the compare shows an asset.
private struct SideBadge: View {
    let side: String

    var body: some View {
        Text(verbatim: side)
            .font(.system(size: 9, weight: .bold))
            .frame(width: 15, height: 15)
            .background(Circle().fill(Color.accentColor.opacity(0.2)))
            .help(side == "A" ? "Shown on the stage as A"
                              : "Shown on the stage as B")
    }
}

/// The picture a result was made from, as the model got it -- an edit's
/// base, a clip's first picture -- small, at the end of its row: dragged
/// onto A or B under the stage to compare it, or into the prompt.
private struct BaseThumb: View {
    let model: AppModel
    let entry: HistoryEntryDTO
    @State private var image: CGImage?

    var body: some View {
        let inA = model.stage.aCompared == entry.id
        let inB = model.stage.bCompared == entry.id
        ZStack {
            RoundedRectangle(cornerRadius: 3)
                .fill(Color.primary.opacity(0.06))
            if let image {
                Image(decorative: image, scale: 1)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .clipShape(RoundedRectangle(cornerRadius: 3))
            }
        }
        .frame(width: 24, height: 24)
        .overlay {
            if inA || inB {
                RoundedRectangle(cornerRadius: 3)
                    .strokeBorder(Color.accentColor, lineWidth: 1.5)
            }
        }
        .overlay(alignment: .topTrailing) {
            if inA || inB {
                Text(verbatim: inA ? "A" : "B")
                    .font(.system(size: 7, weight: .bold))
                    .foregroundStyle(.white)
                    .frame(width: 10, height: 10)
                    .background(Circle().fill(Color.accentColor))
                    .offset(x: 4, y: -4)
            }
        }
        .help(entry.rendered
              ? "Made from this, as the model got it (adjusted or cropped). Drag it onto A or B to compare."
              : "Made from this. Drag it onto A or B to compare.")
        .accessibilityLabel(Text("Base as sent"))
        .draggable(entry.url) {
            ZStack {
                if let image {
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                }
            }
            .frame(width: 64, height: 64)
        }
        .task(id: entry.id) {
            let u = entry.url
            image = await Task.detached(priority: .utility) {
                guard let src = CGImageSourceCreateWithURL(u as CFURL, nil)
                else { return nil }
                return CGImageSourceCreateThumbnailAtIndex(src, 0, [
                    kCGImageSourceCreateThumbnailFromImageAlways: true,
                    kCGImageSourceCreateThumbnailWithTransform: true,
                    kCGImageSourceThumbnailMaxPixelSize: 64,
                ] as CFDictionary)
            }.value
        }
    }
}
