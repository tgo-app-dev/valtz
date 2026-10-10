import AppKit
import SwiftUI

/// Settings › Storage, as macOS shows a disk: one bar for the internal
/// SSD -- what Valtz keeps on it, by category, then everything else, then
/// what is free -- and under it each category in detail: every model in
/// the folder Valtz manages, every project (an open one's assets each),
/// the managed cache (Controller::storage_report).
struct StorageView: View {
    @Bindable var model: AppModel
    @State private var report: StorageReport?
    @State private var loading = false
    /// Each list's order, kept between launches (a snapshot run's in
    /// memory alone).
    @State private var modelSort = StorageSort.kept("models")
    @State private var projectSort = StorageSort.kept("projects")

    var body: some View {
        Form {
            Section { SettingsHeader(page: .storage) }
            if let r = report {
                Section {
                    VolumeBar(report: r) {
                        Task { await load() }
                    }
                    .disabled(loading)
                }
                Section {
                    ForEach(modelSort.order(r.models.items,
                                            size: \.bytes,
                                            created: { $0.created ?? 0 },
                                            name: { Self.modelName($0) })) { m in
                        sizeRow(Self.modelName(m), detail: m.repo,
                                bytes: m.bytes, created: m.created)
                    }
                    if r.models.items.isEmpty {
                        Text("No models downloaded by Valtz yet.")
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    header("Models", r.models.bytes, r.models.root,
                           .purple, sort: $modelSort, list: "models")
                }
                Section {
                    ForEach(projectSort.order(r.projects.items,
                                              size: \.bytes,
                                              created: { $0.created ?? 0 },
                                              name: \.name)) { p in
                        ProjectRow(project: p, sort: projectSort)
                    }
                } header: {
                    header("Projects", r.projects.bytes, r.projects.root,
                           .blue, sort: $projectSort, list: "projects")
                }
                Section {
                    let budget = r.cache.budget.formatted(
                        ByteCountFormatStyle(style: .file))
                    sizeRow(String(localized: "Thumbnails and previews"),
                            detail: String(localized: "of a \(budget) budget, made again when needed"),
                            bytes: r.cache.bytes, created: nil)
                } header: {
                    header("Cache", r.cache.bytes, r.cache.root, .orange)
                }
            } else {
                ProgressView().frame(maxWidth: .infinity)
            }
        }
        .formStyle(.grouped)
        .task { await load() }
    }

    private func load() async {
        guard let core = model.core else { return }
        loading = true
        report = await Task.detached(priority: .userInitiated) {
            DTO.decode(StorageReport.self, core.storageReportJSON())
        }.value
        loading = false
    }

    /// A model by its catalog names, else its repository.
    static func modelName(_ m: StorageReport.ModelItem) -> String {
        m.names.isEmpty ? m.repo : m.names.joined(separator: ", ")
    }

    private func header(_ title: LocalizedStringKey, _ bytes: Int64,
                        _ path: String, _ color: Color,
                        sort: Binding<StorageSort>? = nil,
                        list: String = "") -> some View {
        HStack {
            Circle().fill(color).frame(width: 9, height: 9)
            Text(title)
            Spacer()
            if let sort {
                SortMenu(sort: sort, list: list)
            }
            Text(verbatim: bytes.formatted(ByteCountFormatStyle(style: .file)))
                .monospacedDigit()
            Button {
                NSWorkspace.shared.selectFile(nil,
                                              inFileViewerRootedAtPath: path)
            } label: { Image(systemName: "folder") }
                .buttonStyle(.borderless)
                .help("Show in Finder")
        }
    }

    private func sizeRow(_ title: String, detail: String,
                         bytes: Int64, created: Int64?) -> some View {
        LabeledContent {
            Text(verbatim: bytes.formatted(ByteCountFormatStyle(style: .file)))
                .monospacedDigit()
        } label: {
            Text(verbatim: title)
            Text(verbatim: StorageSort.detail(detail, created: created))
        }
    }
}

/// The disk in one bar: Valtz's models, projects and cache, everything
/// else used, and what is free -- with each one's share named below.
private struct VolumeBar: View {
    let report: StorageReport
    let refresh: () -> Void

    var body: some View {
        let r = report
        let used = max(0, r.volume.capacity - r.volume.free)
        let valtz = r.models.bytes + r.projects.bytes + r.cache.bytes
        let parts: [(String, Int64, Color)] = [
            (String(localized: "Models"), r.models.bytes, .purple),
            (String(localized: "Projects"), r.projects.bytes, .blue),
            (String(localized: "Cache"), r.cache.bytes, .orange),
            (String(localized: "Other"), max(0, used - valtz), .gray),
        ]
        let style = ByteCountFormatStyle(style: .file)
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Internal SSD").font(.headline)
                Spacer()
                Text("\(r.volume.free.formatted(style)) available of \(r.volume.capacity.formatted(style))")
                    .foregroundStyle(.secondary)
                Button(action: refresh) {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .help("Measure again")
            }
            GeometryReader { g in
                HStack(spacing: 1) {
                    ForEach(parts.indices, id: \.self) { i in
                        let share = Double(parts[i].1)
                            / Double(max(1, r.volume.capacity))
                        // A sliver still shows.
                        Rectangle().fill(parts[i].2)
                            .frame(width: parts[i].1 > 0
                                   ? max(2, g.size.width * share) : 0)
                    }
                    Rectangle().fill(Color.primary.opacity(0.08))
                }
            }
            .frame(height: 22)
            .clipShape(RoundedRectangle(cornerRadius: 5))
            HStack(spacing: 16) {
                ForEach(parts.indices, id: \.self) { i in
                    HStack(spacing: 5) {
                        Circle().fill(parts[i].2).frame(width: 8, height: 8)
                        Text(verbatim: parts[i].0)
                        Text(verbatim: parts[i].1.formatted(style))
                            .foregroundStyle(.secondary)
                            .monospacedDigit()
                    }
                }
            }
            .font(.caption)
        }
        .padding(.vertical, 4)
    }
}

/// A project: its size -- and, open, its assets each, largest first, and
/// what the rest of the package holds.
private struct ProjectRow: View {
    let project: StorageReport.Project
    /// Its assets in the Projects list's order.
    let sort: StorageSort
    @State private var open = false

    var body: some View {
        let p = project
        let style = ByteCountFormatStyle(style: .file)
        if let assets = p.assets, !assets.isEmpty {
            DisclosureGroup(isExpanded: $open) {
                ForEach(sort.order(assets, size: \.bytes,
                                   created: { $0.created ?? 0 },
                                   name: \.name)) { a in
                    LabeledContent {
                        Text(verbatim: a.linked
                             ? String(localized: "linked")
                             : a.bytes.formatted(style))
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    } label: {
                        Label {
                            Text(verbatim: a.name)
                                .lineLimit(1)
                                .truncationMode(.middle)
                        } icon: {
                            Image(systemName: a.kind == "video" ? "film"
                                                                : "photo")
                        }
                    }
                }
                if let other = p.other, other > 0 {
                    LabeledContent {
                        Text(verbatim: other.formatted(style))
                            .monospacedDigit()
                            .foregroundStyle(.secondary)
                    } label: {
                        Text("Records and earlier markup")
                    }
                }
            } label: {
                title(p, style)
            }
        } else {
            title(p, style)
        }
    }

    private func title(_ p: StorageReport.Project,
                       _ style: ByteCountFormatStyle) -> some View {
        LabeledContent {
            Text(verbatim: p.bytes.formatted(style)).monospacedDigit()
        } label: {
            HStack(spacing: 6) {
                Text(verbatim: p.name)
                if p.ephemeral {
                    Text("This session").font(.caption)
                        .foregroundStyle(.secondary)
                } else if p.open {
                    Text("Open").font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Text(verbatim: StorageSort.detail(p.path, created: p.created))
        }
    }
}

/// How a Storage list is ordered: by size on disk, by when it was made,
/// or by name -- ascending or descending; kept per list ("models",
/// "projects") between launches.
struct StorageSort: Equatable {
    enum Key: String, CaseIterable {
        case size, created, name
    }
    var key: Key = .size
    var ascending = false

    /// `items` in this order; ties by name, then as they came.
    func order<T>(_ items: [T], size: (T) -> Int64, created: (T) -> Int64,
                  name: (T) -> String) -> [T] {
        func byName(_ a: T, _ b: T) -> ComparisonResult {
            name(a).localizedStandardCompare(name(b))
        }
        let sorted = items.enumerated().sorted { x, y in
            let a = x.element, b = y.element
            var c: ComparisonResult
            switch key {
            case .size:
                c = size(a) == size(b) ? .orderedSame
                    : size(a) < size(b) ? .orderedAscending : .orderedDescending
            case .created:
                c = created(a) == created(b) ? .orderedSame
                    : created(a) < created(b) ? .orderedAscending
                    : .orderedDescending
            case .name:
                c = byName(a, b)
            }
            if c == .orderedSame, key != .name { c = byName(a, b) }
            if c == .orderedSame { return x.offset < y.offset }
            return ascending ? c == .orderedAscending
                             : c == .orderedDescending
        }
        return sorted.map(\.element)
    }

    /// A row's detail with the date it was made after it.
    static func detail(_ text: String, created: Int64?) -> String {
        guard let ms = created, ms > 0 else { return text }
        let d = Date(timeIntervalSince1970: Double(ms) / 1000)
        return String(localized: "\(text) · created \(d.formatted(date: .abbreviated, time: .omitted))")
    }

    // Kept in the defaults -- a snapshot run's in memory, so it leaves the
    // person's alone (and VALTZ_SNAPSHOT_STORAGE_SORT sets it:
    // "models=name:asc,projects=created:desc").
    private static var scripted: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] != nil
    }

    static func kept(_ list: String) -> StorageSort {
        let spec = scripted
            ? ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT_STORAGE_SORT"]?
                .split(separator: ",")
                .first { $0.hasPrefix(list + "=") }
                .map { String($0.dropFirst(list.count + 1)) }
            : UserDefaults.standard.string(forKey: "storage.sort." + list)
        guard let spec else { return StorageSort() }
        let p = spec.split(separator: ":")
        var s = StorageSort()
        s.key = Key(rawValue: String(p.first ?? "")) ?? .size
        s.ascending = p.count > 1 && p[1] == "asc"
        return s
    }

    func keep(_ list: String) {
        guard !Self.scripted else { return }
        UserDefaults.standard.set("\(key.rawValue):\(ascending ? "asc" : "desc")",
                                  forKey: "storage.sort." + list)
    }
}

/// A list's sort, in its header: by size, date created or name, and
/// which way.
private struct SortMenu: View {
    @Binding var sort: StorageSort
    let list: String

    var body: some View {
        Menu {
            Picker("Sort By", selection: Binding(
                get: { sort.key },
                set: { sort.key = $0; sort.keep(list) })) {
                Text("Size").tag(StorageSort.Key.size)
                Text("Date Created").tag(StorageSort.Key.created)
                Text("Name").tag(StorageSort.Key.name)
            }
            .pickerStyle(.inline)
            Picker("Order", selection: Binding(
                get: { sort.ascending },
                set: { sort.ascending = $0; sort.keep(list) })) {
                Text("Ascending").tag(true)
                Text("Descending").tag(false)
            }
            .pickerStyle(.inline)
        } label: {
            HStack(spacing: 3) {
                Text(label)
                Image(systemName: sort.ascending ? "chevron.up"
                                                 : "chevron.down")
                    .font(.system(size: 8, weight: .semibold))
            }
            .font(.caption)
            .foregroundStyle(.secondary)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .help("Sort this list")
    }

    private var label: String {
        switch sort.key {
        case .size: String(localized: "Size")
        case .created: String(localized: "Date Created")
        case .name: String(localized: "Name")
        }
    }
}
