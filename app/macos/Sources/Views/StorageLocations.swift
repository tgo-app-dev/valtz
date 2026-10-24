import AppKit
import SwiftUI

/// The PROJECTS, MODELS and CACHE folders put elsewhere (Settings ›
/// General › Storage; Controller::set_location, base/paths.h). A folder
/// chosen is asked about first: off this Mac's internal drive, the models'
/// and the cache's -- read as Valtz works, a model's weights streamed --
/// are slowed by the drive's link, said with what it takes (USB4 at
/// least); then what becomes of what is there: the models moved or kept
/// where they are (still used), the cache moved, left or deleted. A move
/// is a task of the core's, its progress on the row, Stop beside it.
extension AppModel {
    func refreshLocations() {
        guard let core else { return }
        locations = DTO.decode(LocationsInfo.self, core.locationsJSON())
        warnMissingLocations()
    }

    /// A folder chosen that was not there as Valtz started (its drive not
    /// connected): said once.
    private func warnMissingLocations() {
        guard !locationsWarned, let l = locations else { return }
        locationsWarned = true
        if l.models.missingAtLaunch {
            let name = Self.folderName(l.models.path)
            flash(String(localized: "The models folder “\(name)” is not there — is its drive connected? Its models cannot run until it is."),
                  level: .warning)
        } else if l.cache.missingAtLaunch {
            let name = Self.folderName(l.cache.path)
            flash(String(localized: "The cache folder “\(name)” is not there — is its drive connected? Valtz keeps its cache in the default folder meanwhile."),
                  level: .warning)
        }
    }

    nonisolated static func folderName(_ path: String) -> String {
        URL(fileURLWithPath: path).lastPathComponent
    }

    /// On this Mac's internal drive: not an external one, not a network
    /// share.
    nonisolated static func onInternalDrive(_ url: URL) -> Bool {
        let v = try? url.resourceValues(forKeys: [.volumeIsInternalKey,
                                                  .volumeIsLocalKey])
        return (v?.volumeIsInternal ?? true) && (v?.volumeIsLocal ?? true)
    }

    /// What a folder chosen for `what` asks before it is used, in turn:
    /// "external" -- the models or the cache off the internal drive --
    /// then "how" -- what becomes of what is there (nothing to ask with
    /// nothing there, nor for the projects, which move no file).
    func locationQuestions(_ what: String, _ url: URL) -> [String] {
        guard what != "projects" else { return [] }
        var q: [String] = []
        if !Self.onInternalDrive(url) { q.append("external") }
        let held = what == "models" ? locations?.models.bytes
                                    : locations?.cache.bytes
        if (held ?? 0) > 0 { q.append("how") }
        return q
    }

    /// The answers to "how", in the alert's order.
    static func locationHows(_ what: String) -> [String] {
        what == "models" ? ["move", "keep"] : ["move", "keep", "drop"]
    }

    /// Change…: a folder chosen for `what` in the open panel, from where
    /// it is now.
    func chooseLocation(_ what: String) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.allowsMultipleSelection = false
        panel.prompt = String(localized: "Use This Folder")
        panel.message = what == "models"
            ? String(localized: "Choose where Valtz keeps its models.")
            : what == "cache"
                ? String(localized: "Choose where Valtz keeps its cache.")
                : String(localized: "Choose where new projects are saved.")
        if let now = place(what)?.path {
            panel.directoryURL = URL(fileURLWithPath: now)
                .deletingLastPathComponent()
        }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        changeLocation(what, to: url)
    }

    /// Use the Default Folder.
    func defaultLocation(_ what: String) {
        guard let d = place(what)?.defaultPath, !d.isEmpty else { return }
        changeLocation(what, to: URL(fileURLWithPath: d))
    }

    func place(_ what: String) -> LocationsInfo.Place? {
        switch what {
        case "projects": locations?.projects
        case "models": locations?.models
        default: locations?.cache
        }
    }

    /// `what` into `url`, asked about first (locationQuestions); Cancel at
    /// any question leaves everything as it is.
    func changeLocation(_ what: String, to url: URL) {
        var how = "move"
        for q in locationQuestions(what, url) {
            let alert = q == "external" ? externalAlert(what, url)
                                        : howAlert(what, url)
            let r = alert.runModal()
            let k = r.rawValue
                - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
            if q == "external" {
                guard k == 0 else { return }
            } else {
                let hows = Self.locationHows(what)
                guard k >= 0, k < hows.count else { return }
                how = hows[k]
            }
        }
        applyLocation(what, url, how: how)
    }

    /// The core's change; refused, said in Settings.
    func applyLocation(_ what: String, _ url: URL, how: String) {
        guard let core else { return }
        let r = core.setLocation(what, path: url.path, how: how)
        if !r.ok {
            locationError = r.message
            return
        }
        paths = DTO.decode(PathsInfo.self, core.pathsJSON())
        refreshLocations()
        if r.job == nil, what != "projects" {
            refreshMachine()
        }
    }

    /// Off the internal drive: what that costs, and what it takes.
    private func externalAlert(_ what: String, _ url: URL) -> NSAlert {
        let a = NSAlert()
        a.alertStyle = .warning
        let name = url.lastPathComponent
        a.messageText = String(localized: "“\(name)” is not on this Mac’s internal drive")
        a.informativeText = what == "models"
            ? String(localized: "Valtz streams a model’s weights from the models folder while it generates, so a drive slower than the internal one limits how fast Valtz works. Connect the drive by USB4 or Thunderbolt at least: USB 3 and Ethernet carry 10 Gb/s at most, which cannot keep up with a model’s streaming.")
            : String(localized: "Valtz reads and writes its cache as it works — thumbnails, renderings, the encodings it keeps — so a drive slower than the internal one limits how fast Valtz works. Connect the drive by USB4 or Thunderbolt at least: USB 3 and Ethernet carry 10 Gb/s at most, which cannot keep up with a model’s streaming.")
        a.addButton(withTitle: String(localized: "Use This Folder"))
        a.addButton(withTitle: String(localized: "Cancel"))
        return a
    }

    /// What becomes of what is there.
    private func howAlert(_ what: String, _ url: URL) -> NSAlert {
        let a = NSAlert()
        let name = url.lastPathComponent
        if what == "models" {
            let size = (locations?.models.bytes ?? 0)
                .formatted(.byteCount(style: .file))
            a.messageText = String(localized: "Move the models to “\(name)”?")
            a.informativeText = String(localized: "\(size) of models are in Valtz’s models folders now. Moved, they are copied into the new folder and then deleted where they were — from another drive, that takes a while. Kept where they are, they are still used, and new downloads go to the new folder.")
            a.addButton(withTitle: String(localized: "Move the Models"))
            a.addButton(withTitle: String(localized: "Keep Them Where They Are"))
        } else {
            let size = (locations?.cache.bytes ?? 0)
                .formatted(.byteCount(style: .file))
            a.messageText = String(localized: "What becomes of the cache?")
            a.informativeText = String(localized: "\(size) of thumbnails, renderings and kept encodings are in the cache now. Moved, they are in “\(name)” at once; left where they are, they stay on disk, unused; deleted, Valtz makes them again as it needs them.")
            a.addButton(withTitle: String(localized: "Move the Cache"))
            a.addButton(withTitle: String(localized: "Leave It Where It Is"))
            a.addButton(withTitle: String(localized: "Delete It"))
        }
        a.addButton(withTitle: String(localized: "Cancel"))
        return a
    }

    /// A folder being moved now, for `what`: its job and progress.
    func relocation(of what: String) -> (job: String, info: JobInfo?)? {
        guard let m = locations?.moving, m.what == what else { return nil }
        return (m.job, jobs[m.job])
    }

    func stopRelocation(_ what: String) {
        guard let job = relocation(of: what)?.job else { return }
        _ = core?.cancel(job: job)
    }

    /// A move ended: where things are now; done, said; failed, said in
    /// Settings.
    func relocationEnded(_ ev: CoreEvent) {
        refreshLocations()
        if let core { paths = DTO.decode(PathsInfo.self, core.pathsJSON()) }
        let what = ev.payload["what"] as? String ?? ""
        switch ev.kind {
        case "job.finished":
            let name = Self.folderName(ev.payload["path"] as? String ?? "")
            flash(what == "models"
                  ? String(localized: "The models are in “\(name)” now.")
                  : String(localized: "The cache is in “\(name)” now."))
            refreshMachine()
        case "job.failed":
            locationError = L10n.coreMessage(ev.payload)
        default:
            break
        }
    }
}

/// Settings › General › Storage's rows: each folder, where it is and what
/// of it is said -- off the internal drive, not there, the models folders
/// kept -- Show in Finder, Change…, Use the Default Folder; a move under
/// way, its bar and Stop.
struct StorageLocationRows: View {
    @Bindable var model: AppModel

    var body: some View {
        Group {
            row("projects", "Projects")
                .settingsMark(String(localized: "Projects"))
            row("models", "Models")
                .settingsMark(String(localized: "Models"))
            row("cache", "Cache")
                .settingsMark(String(localized: "Cache"))
        }
        .alert("The folder was not changed",
               isPresented: Binding(
                   get: { model.locationError != nil },
                   set: { if !$0 { model.locationError = nil } })) {
            Button("OK", role: .cancel) { model.locationError = nil }
        } message: {
            Text(verbatim: model.locationError ?? "")
        }
    }

    private func row(_ what: String, _ title: LocalizedStringKey)
        -> some View {
        let place = model.place(what)
        let moving = model.relocation(of: what)
        return LabeledContent {
            HStack(alignment: .center, spacing: 6) {
                VStack(alignment: .trailing, spacing: 2) {
                    Text(verbatim: place?.path ?? "")
                        .lineLimit(1).truncationMode(.middle)
                        .help(Text(verbatim: place?.path ?? ""))
                    ForEach(notes(what, place), id: \.self) { n in
                        Text(verbatim: n)
                            .font(.caption).foregroundStyle(.secondary)
                            .lineLimit(2).multilineTextAlignment(.trailing)
                    }
                    if let moving { progress(what, moving) }
                }
                Button {
                    if let p = place?.path {
                        NSWorkspace.shared.selectFile(
                            nil, inFileViewerRootedAtPath: p)
                    }
                } label: { Image(systemName: "arrow.right.circle") }
                    .buttonStyle(.borderless)
                    .help("Show in Finder")
                    .disabled(place?.there != true)
                Menu {
                    Button("Change…") { model.chooseLocation(what) }
                    Button("Use the Default Folder") {
                        model.defaultLocation(what)
                    }
                    .disabled(place?.isDefault ?? true)
                } label: {
                    Text("Change")
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .disabled(model.locations?.moving != nil)
                .help(what == "projects"
                      ? "Where new projects are saved"
                      : what == "models"
                        ? "Where Valtz keeps its models: moved there, or kept where they are"
                        : "Where Valtz keeps its cache: moved there, left or deleted")
            }
        } label: {
            Text(title)
        }
    }

    /// What is said of a folder under its path.
    private func notes(_ what: String, _ p: LocationsInfo.Place?)
        -> [String] {
        guard let p else { return [] }
        var out: [String] = []
        if !p.there {
            out.append(String(localized: "Not there: is its drive connected?"))
        } else if !p.internal {
            out.append(String(localized: "Not on the internal drive: it may limit Valtz’s speed"))
        }
        switch what {
        case "projects":
            out.append(String(localized: "New projects are saved here"))
        case "models":
            if let b = p.bytes, b > 0 {
                out.append(b.formatted(.byteCount(style: .file)))
            }
            for k in p.kept ?? [] {
                out.append(String(localized: "Also used, kept where it was: \(k.path)"))
            }
        default:
            if let paths = model.paths {
                let used = ByteCountFormatter.string(
                    fromByteCount: paths.cacheBytes, countStyle: .file)
                let budget = ByteCountFormatter.string(
                    fromByteCount: paths.cacheBudgetBytes, countStyle: .file)
                out.append(String(localized: "\(used) of \(budget)"))
            }
            if let inUse = p.inUse, inUse != p.path {
                out.append(String(localized: "In use meanwhile: \(inUse)"))
            }
        }
        return out
    }

    /// A move's bar, what it is doing, and Stop while it can.
    private func progress(_ what: String,
                          _ m: (job: String, info: JobInfo?)) -> some View {
        let phase = m.info?.phase
        let done = Int64(phase?.done ?? 0), total = Int64(phase?.total ?? 0)
        let caption: String = {
            switch phase?.phase {
            case "waiting":
                return String(localized: "Waiting for the work under way to finish…")
            case "deleting":
                return String(localized: "Deleting the old files…")
            default:
                guard total > 0 else {
                    return String(localized: "Moving…")
                }
                let d = done.formatted(.byteCount(style: .file))
                let t = total.formatted(.byteCount(style: .file))
                return String(localized: "Moving: \(d) of \(t)")
            }
        }()
        return HStack(spacing: 6) {
            ProgressView(value: total > 0 ? Double(done) / Double(total) : 0)
                .frame(width: 140)
            Text(verbatim: caption)
                .font(.caption).foregroundStyle(.secondary)
            if phase?.phase != "deleting" {
                Button("Stop") { model.stopRelocation(what) }
                    .controlSize(.small)
                    .help("Stop the move: what was copied goes, the folders as they were")
            }
        }
    }
}
