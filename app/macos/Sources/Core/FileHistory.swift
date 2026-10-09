import AppKit

/// Where the file panels open (DESIGN §10a): the folder each KIND of file
/// was last opened or saved in, kept apart -- a project's (open, new,
/// Save As), media attached or imported, an export -- so attaching from
/// Downloads or exporting to the Desktop does not move where the next
/// project is saved. With none yet (or one gone), Valtz's projects folder.
enum FileHistory: String, CaseIterable {
    case project, attach, export

    var key: String { "files.lastFolder." + rawValue }
}

extension AppModel {
    /// The folder a panel for `kind` opens in.
    func panelFolder(_ kind: FileHistory) -> URL {
        if let d = rememberedFolder(kind), Self.isFolder(d) { return d }
        return defaultPanelFolder
    }

    /// The folder of what a panel for `kind` chose -- a file's, a
    /// project's (a package is a file here) -- or the folder itself.
    func rememberPanel(_ kind: FileHistory, chose url: URL?) {
        guard let url else { return }
        let dir = url.pathExtension.isEmpty && Self.isFolder(url)
            ? url : url.deletingLastPathComponent()
        if keepsPanelFolder(kind) {
            Self.setKeptFolder(dir.path, kind.key)
        } else {
            sessionPanelFolders[kind.rawValue] = dir
        }
    }

    /// Valtz's projects folder (~/Movies/Valtz), where a panel opens with
    /// no history.
    var defaultPanelFolder: URL {
        if let p = paths?.projects, !p.isEmpty {
            return URL(fileURLWithPath: p, isDirectory: true)
        }
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Movies/Valtz", isDirectory: true)
    }

    /// Kept between launches: a project's folder always; an anonymous
    /// session's attachments and exports are its own, forgotten with it
    /// (Start Over, another project).
    private func keepsPanelFolder(_ kind: FileHistory) -> Bool {
        kind == .project || !isAnonymous
    }

    private func rememberedFolder(_ kind: FileHistory) -> URL? {
        if keepsPanelFolder(kind) {
            return Self.keptFolder(kind.key)
                .map { URL(fileURLWithPath: $0, isDirectory: true) }
        }
        return sessionPanelFolders[kind.rawValue]
    }

    /// The folders kept between launches -- in the defaults; a scripted
    /// snapshot run's in memory, so it leaves the person's alone.
    private static var scripted: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] != nil
    }
    @MainActor private static var scriptedKept: [String: String] = [:]

    private static func keptFolder(_ key: String) -> String? {
        scripted ? scriptedKept[key] : UserDefaults.standard.string(forKey: key)
    }

    private static func setKeptFolder(_ path: String, _ key: String) {
        if scripted {
            scriptedKept[key] = path
        } else {
            UserDefaults.standard.set(path, forKey: key)
        }
    }

    private static func isFolder(_ url: URL) -> Bool {
        var dir: ObjCBool = false
        return FileManager.default.fileExists(atPath: url.path,
                                              isDirectory: &dir)
            && dir.boolValue
    }
}
