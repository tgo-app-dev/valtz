import Foundation
import Observation

/// A project opened or saved lately: where its package is, its name, and
/// when it was last opened.
struct RecentProject: Codable, Identifiable, Equatable, Sendable {
    var url: URL
    var name: String
    var lastOpened: Date
    var id: String { url.standardizedFileURL.path }
}

/// The projects opened or saved lately, newest first, at most ten: the
/// sidebar's Recents and File › Open Recent read the same list. A project
/// is noted as it opens and as Save As names it; the anonymous session
/// never is (it has no file to come back to). Kept in the user defaults
/// between launches -- not by a scripted snapshot run, whose projects
/// are noted for the run alone. Valtz is not sandboxed, so a path is
/// enough to open one again; one gone from the disk is left out.
@MainActor @Observable
final class RecentProjects {
    static let shared = RecentProjects()
    static let limit = 10

    private(set) var items: [RecentProject]

    private static let key = "recentProjects"
    private static var keeps: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
    }

    private init() {
        guard Self.keeps,
              let data = UserDefaults.standard.data(forKey: Self.key),
              let list = try? JSONDecoder().decode([RecentProject].self,
                                                   from: data)
        else {
            items = []
            return
        }
        items = list
    }

    /// The ones still on the disk.
    var existing: [RecentProject] {
        items.filter { FileManager.default.fileExists(atPath: $0.url.path) }
    }

    /// `url` opened or saved now: to the top.
    func note(_ url: URL, name: String) {
        let p = RecentProject(url: url, name: name, lastOpened: Date())
        items.removeAll { $0.id == p.id }
        items.insert(p, at: 0)
        if items.count > Self.limit { items.removeLast(items.count - Self.limit) }
        store()
    }

    func remove(_ p: RecentProject) {
        items.removeAll { $0.id == p.id }
        store()
    }

    func clear() {
        items = []
        store()
    }

    private func store() {
        guard Self.keeps, let data = try? JSONEncoder().encode(items)
        else { return }
        UserDefaults.standard.set(data, forKey: Self.key)
    }
}
