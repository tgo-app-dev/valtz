import AppKit
import SwiftUI

/// The project's FILE, as Preview's title bar has a document's: the name
/// hovered shows a ⌄; clicked, a panel under it -- the name, typed over
/// (the file renamed: Controller::move_project), its Finder TAGS, WHERE
/// it is (moved elsewhere), and LOCKED (the Finder's Locked: every change
/// then refused, and asked about -- Unlock, Duplicate, Cancel). An
/// anonymous session's panel names it and offers Save.

struct ProjectFileInfo: Equatable, Sendable {
    var path = ""
    var folder = ""
    var tags: [String] = []
    var locked = false
}

extension AppModel {
    /// The file as it is now (the panel opening, a change made).
    func refreshProjectFile() {
        guard let core, let projectId, !isAnonymous, !document.untitled
        else {
            projectFile = nil
            return
        }
        let r = core.projectFile(projectId, ["op": "info"])
        projectFile = Self.fileInfo(r)
    }

    private static func fileInfo(_ r: CoreReply) -> ProjectFileInfo? {
        guard r.ok, let f = r["file"] as? [String: Any],
              let path = f["path"] as? String else { return nil }
        return ProjectFileInfo(path: path,
                               folder: f["folder"] as? String ?? "",
                               tags: f["tags"] as? [String] ?? [],
                               locked: f["locked"] as? Bool ?? false)
    }

    private func fileOp(_ req: [String: Any]) -> Bool {
        guard let core, let projectId else { return false }
        let r = core.projectFile(projectId, req)
        guard r.ok else {
            if r["key"] as? String != "core.project_locked" {
                flash(r.message, level: .error)
            }
            return false
        }
        projectFile = Self.fileInfo(r)
        refreshDocument()
        return true
    }

    /// The name typed in the panel: an anonymous session's renamed; a
    /// saved project's FILE renamed where it is.
    func renameProjectFile(_ name: String) {
        let n = name.trimmingCharacters(in: .whitespacesAndNewlines)
            .replacingOccurrences(of: "/", with: "-")
        guard !n.isEmpty else { return }
        if isAnonymous || document.untitled {
            renameSession(n)
            return
        }
        guard n != projectName, let f = projectFile else { return }
        let to = URL(fileURLWithPath: f.folder)
            .appendingPathComponent(n + ".valtz")
        _ = fileOp(["op": "move", "path": to.path])
    }

    /// Moved to `folder`, its name kept.
    func moveProjectFile(to folder: URL) {
        guard let f = projectFile else { return }
        let name = URL(fileURLWithPath: f.path).lastPathComponent
        _ = fileOp(["op": "move",
                    "path": folder.appendingPathComponent(name).path])
    }

    /// Where: another folder chosen.
    func chooseProjectFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = String(localized: "Move")
        panel.message = String(localized: "Choose where to move “\(projectName)”.")
        if let f = projectFile {
            panel.directoryURL = URL(fileURLWithPath: f.folder)
        }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        moveProjectFile(to: url)
    }

    func setProjectTags(_ tags: [String]) {
        _ = fileOp(["op": "tags", "tags": tags])
    }

    /// Locked or not (refused locked with changes not saved).
    func setProjectLocked(_ on: Bool) {
        _ = fileOp(["op": "lock", "locked": on])
    }

    /// A change refused because the file is locked: asked, as Preview
    /// asks -- Unlock (then made again), Duplicate (a copy beside it, in
    /// a window of its own, to change), Cancel.
    func projectLockedRefused(_ words: String) {
        lockRefusedText = words
        guard !lockAlertUp, document.locked else { return }
        lockAlertUp = true
        let alert = NSAlert()
        alert.messageText = String(localized: "“\(projectName)” is locked")
        alert.informativeText = String(localized: "Unlock it to make changes, or duplicate it to change a copy.")
        alert.addButton(withTitle: String(localized: "Unlock"))
        alert.addButton(withTitle: String(localized: "Duplicate"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        let answer: (NSApplication.ModalResponse) -> Void = { [weak self] a in
            guard let self else { return }
            self.lockAlertUp = false
            // (the refusal's words kept a moment: its caller flashes them
            // as it returns, which the alert has said)
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .seconds(1))
                self?.lockRefusedText = nil
            }
            switch a {
            case .alertFirstButtonReturn:
                if self.fileOp(["op": "lock", "locked": false]) {
                    self.flash(String(localized: "Unlocked: make the change again."))
                }
            case .alertSecondButtonReturn:
                self.duplicateProject()
            default: break
            }
        }
        // (a snapshot run: answered as told, the alert not shown)
        if let a = ProcessInfo.processInfo.environment[
            "VALTZ_SNAPSHOT_LOCK_ANSWER"] {
            FileHandle.standardError.write(Data(
                "snapshot: lock-alert title=\(alert.messageText) answer=\(a)\n".utf8))
            answer(a == "unlock" ? .alertFirstButtonReturn
                   : a == "duplicate" ? .alertSecondButtonReturn
                   : .alertThirdButtonReturn)
            return
        }
        if let w = editorWindow {
            alert.beginSheetModal(for: w, completionHandler: answer)
        } else {
            answer(alert.runModal())
        }
    }

    /// Duplicate: a copy of the saved file beside it ("<name> copy"), a
    /// project of its own (Controller::duplicate_project), unlocked,
    /// opened in a window of its own. (A locked file has no changes not
    /// saved: the copy is all of it.)
    func duplicateProject() {
        guard let f = projectFile ?? {
            refreshProjectFile()
            return projectFile
        }() else { return }
        let src = URL(fileURLWithPath: f.path)
        let folder = src.deletingLastPathComponent()
        let base = src.deletingPathExtension().lastPathComponent
        var to = folder.appendingPathComponent(
            String(localized: "\(base) copy") + ".valtz")
        var n = 2
        while FileManager.default.fileExists(atPath: to.path) {
            to = folder.appendingPathComponent(
                String(localized: "\(base) copy \(String(n))") + ".valtz")
            n += 1
        }
        // A project of its own (a new id: the core's), unlocked.
        guard let core, let projectId else { return }
        let r = core.projectFile(projectId, ["op": "duplicate",
                                             "path": to.path])
        guard r.ok else {
            flash(r.message, level: .error)
            return
        }
        EditorWindows.shared.open(to)
    }
}

/// The title's panel, under the name.
struct ProjectFilePanel: View {
    @Bindable var model: AppModel
    @State private var name = ""
    /// Typed into since the panel opened: only then is it a rename (the
    /// field shows the name, which a rename made elsewhere changes).
    @State private var typed = false
    @FocusState private var naming: Bool

    private var saved: Bool { !model.isAnonymous && !model.document.untitled }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            TextField("Name", text: $name)
                .textFieldStyle(.roundedBorder)
                .font(.body.weight(.semibold))
                .focused($naming)
                .onSubmit { commitName() }
                .disabled(model.document.locked || model.agentHolds)
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 8,
                 verticalSpacing: 8) {
                GridRow {
                    Text("Tags").foregroundStyle(.secondary)
                        .gridColumnAlignment(.trailing)
                    TagsField(tags: model.projectFile?.tags ?? [],
                              enabled: saved) { model.setProjectTags($0) }
                        .frame(height: 22)
                }
                GridRow {
                    Text("Where").foregroundStyle(.secondary)
                    if saved, let f = model.projectFile {
                        whereMenu(f)
                    } else {
                        HStack {
                            Text("Not saved").foregroundStyle(.secondary)
                            Spacer()
                            Button("Save…") {
                                model.titlePanelOpen = false
                                model.saveDocumentAs()
                            }
                        }
                    }
                }
            }
            Divider()
            Toggle(isOn: Binding(
                get: { model.document.locked },
                set: { model.setProjectLocked($0) })) {
                Label("Locked", systemImage: model.document.locked
                      ? "lock.fill" : "lock.open")
            }
            .toggleStyle(.checkbox)
            .disabled(!saved || (model.document.dirty && !model.document.locked))
            .help(!saved ? String(localized: "Save the project first.")
                  : model.document.dirty && !model.document.locked
                  ? String(localized: "It has changes not saved: save them first, then lock it.")
                  : String(localized: "Locked, the file is not changed: Valtz asks first, as the Finder's Locked does."))
        }
        .padding(14)
        .frame(width: 300)
        .controlSize(.small)
        .onAppear {
            model.refreshProjectFile()
            name = model.windowTitle
        }
        .onChange(of: model.windowTitle) { _, t in
            if !naming {
                name = t
                typed = false
            }
        }
        .onChange(of: name) { _, _ in if naming { typed = true } }
        .onChange(of: naming) { _, on in if !on { commitName() } }
        .onDisappear { commitName() }
    }

    private func commitName() {
        guard typed else { return }
        typed = false
        if name != model.windowTitle { model.renameProjectFile(name) }
    }

    private func whereMenu(_ f: ProjectFileInfo) -> some View {
        let here = URL(fileURLWithPath: f.folder)
        let places: [URL] = [
            FileManager.default.urls(for: .desktopDirectory,
                                     in: .userDomainMask).first,
            FileManager.default.urls(for: .documentDirectory,
                                     in: .userDomainMask).first,
            FileManager.default.urls(for: .moviesDirectory,
                                     in: .userDomainMask).first
                .map { $0.appendingPathComponent("Valtz") },
        ].compactMap { $0 }.filter {
            $0.standardizedFileURL != here.standardizedFileURL
        }
        return Menu {
            Button {
                NSWorkspace.shared.activateFileViewerSelecting(
                    [URL(fileURLWithPath: f.path)])
            } label: {
                Label("Show in Finder", systemImage: "folder")
            }
            Divider()
            ForEach(places, id: \.self) { u in
                Button(FileManager.default.displayName(atPath: u.path)) {
                    model.moveProjectFile(to: u)
                }
            }
            Divider()
            Button("Other…") { model.chooseProjectFolder() }
        } label: {
            Label(FileManager.default.displayName(atPath: f.folder),
                  systemImage: "folder.fill")
        }
        .disabled(model.document.locked || model.agentHolds)
        .help(f.folder)
    }
}

/// Finder tags as tokens (NSTokenField), each one typed and committed
/// with Return, a comma or the field left.
struct TagsField: NSViewRepresentable {
    let tags: [String]
    let enabled: Bool
    let changed: ([String]) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSTokenField {
        let f = NSTokenField()
        f.delegate = context.coordinator
        f.tokenStyle = .rounded
        f.placeholderString = String(localized: "Add Tags")
        f.font = .systemFont(ofSize: NSFont.smallSystemFontSize)
        f.objectValue = tags
        return f
    }

    func updateNSView(_ f: NSTokenField, context: Context) {
        context.coordinator.parent = self
        f.isEnabled = enabled
        if f.currentEditor() == nil,
           (f.objectValue as? [String]) != tags {
            f.objectValue = tags
        }
    }

    final class Coordinator: NSObject, NSTokenFieldDelegate {
        var parent: TagsField
        init(_ p: TagsField) { parent = p }

        func controlTextDidEndEditing(_ n: Notification) {
            guard let f = n.object as? NSTokenField else { return }
            let now = (f.objectValue as? [String] ?? [])
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty }
            if now != parent.tags { parent.changed(now) }
        }
    }
}
