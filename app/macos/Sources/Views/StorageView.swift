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
                    ForEach(r.models.items) { m in
                        sizeRow(m.names.isEmpty ? m.repo
                                    : m.names.joined(separator: ", "),
                                detail: m.repo, bytes: m.bytes)
                    }
                    if r.models.items.isEmpty {
                        Text("No models downloaded by Valtz yet.")
                            .foregroundStyle(.secondary)
                    }
                } header: {
                    header("Models", r.models.bytes, r.models.root,
                           .purple)
                }
                Section {
                    ForEach(r.projects.items) { p in
                        ProjectRow(project: p)
                    }
                } header: {
                    header("Projects", r.projects.bytes, r.projects.root,
                           .blue)
                }
                Section {
                    let budget = r.cache.budget.formatted(
                        ByteCountFormatStyle(style: .file))
                    sizeRow(String(localized: "Thumbnails and previews"),
                            detail: String(localized: "of a \(budget) budget, made again when needed"),
                            bytes: r.cache.bytes)
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

    private func header(_ title: LocalizedStringKey, _ bytes: Int64,
                        _ path: String, _ color: Color) -> some View {
        HStack {
            Circle().fill(color).frame(width: 9, height: 9)
            Text(title)
            Spacer()
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
                         bytes: Int64) -> some View {
        LabeledContent {
            Text(verbatim: bytes.formatted(ByteCountFormatStyle(style: .file)))
                .monospacedDigit()
        } label: {
            Text(verbatim: title)
            Text(verbatim: detail)
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
    @State private var open = false

    var body: some View {
        let p = project
        let style = ByteCountFormatStyle(style: .file)
        if let assets = p.assets, !assets.isEmpty {
            DisclosureGroup(isExpanded: $open) {
                ForEach(assets.sorted { $0.bytes > $1.bytes }) { a in
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
            Text(verbatim: p.path)
        }
    }
}
