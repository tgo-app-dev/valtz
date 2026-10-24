import SwiftUI

/// RAM's popover -- a click on the status bar's meter (DESIGN §10a): what
/// Valtz holds in memory, drawn as Settings › Storage draws the disk. The
/// bar is the Mac's memory: each model the engine holds -- KEPT WARM
/// between jobs (the helper kept loaded; a picture's DiT, text encoder
/// and VAE kept built, DESIGN §4n), with the time it has left, or a
/// checkpoint still resident -- then the WORKING memory they run in
/// (Metal's allocations past them: scratch, caches), the rest of Valtz
/// (its footprint past Metal's), the other apps, and what is free. Read
/// from the engine every two seconds while it is open
/// (MachineMonitor.startMemory; vpipe's memory_report, DESIGN §15 0zz).
struct MemoryPopover: View {
    @Bindable var model: AppModel

    private var monitor: MachineMonitor { model.monitor }

    var body: some View {
        let load = monitor.load
        let parts = Self.parts(load: load, memory: monitor.memory)
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text("Memory").font(.headline)
                Spacer()
                if let f = load.footprint, let t = load.total {
                    Text("Valtz is using \(Self.bytes(f)) of \(Self.bytes(t))")
                        .foregroundStyle(.secondary)
                }
            }
            UsageBar(parts: parts.map {
                         UsagePart(id: $0.id, bytes: $0.bytes, color: $0.color)
                     },
                     capacity: load.total ?? 1)
            VStack(alignment: .leading, spacing: 7) {
                ForEach(parts) { row($0) }
            }
            if monitor.memory == nil {
                Text("Reading what the engine holds…")
                    .font(.caption).foregroundStyle(.secondary)
            } else if monitor.memory?.items.contains(where: \.warm) != true {
                Text("No model is kept loaded now: each job loads what it needs and lets it go when it ends.")
                    .font(.caption).foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(16)
        .frame(width: 400)
        .onAppear { monitor.startMemory() }
        .onDisappear { monitor.stopMemory() }
    }

    private func row(_ p: Part) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Circle().fill(p.color).frame(width: 8, height: 8)
                .alignmentGuide(.firstTextBaseline) { $0[VerticalAlignment.center] + 4 }
            VStack(alignment: .leading, spacing: 1) {
                Text(verbatim: p.title).lineLimit(1).truncationMode(.middle)
                if !p.detail.isEmpty {
                    Text(verbatim: p.detail)
                        .font(.caption).foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
            }
            Spacer(minLength: 8)
            Text(verbatim: (p.about ? "~" : "") + Self.bytes(p.bytes))
                .monospacedDigit().foregroundStyle(.secondary)
        }
    }

    /// One share of the bar, and its row.
    struct Part: Identifiable {
        let id: String
        let title: String
        let detail: String
        let bytes: Double
        let color: Color
        /// From its files' size: the engine counts none of it.
        var about = false
    }

    private static let palette: [Color] = [
        .purple, .blue, .teal, .indigo, .pink, .mint, .cyan, .brown,
    ]

    /// The bar's shares: the models held, largest first -- those kept
    /// warm before a checkpoint merely resident -- then the working
    /// memory, the rest of Valtz, the other apps.
    static func parts(load: MachineMonitor.Load,
                      memory: MachineMonitor.Memory?) -> [Part] {
        let items = (memory?.items ?? []).sorted {
            ($0.warm ? 1 : 0, $0.bytes + $0.kvBytes)
                > ($1.warm ? 1 : 0, $1.bytes + $1.kvBytes)
        }
        var out = items.enumerated().map { i, it in
            Part(id: it.id, title: it.name.isEmpty ? partName(it) : it.name,
                 detail: detail(it), bytes: it.bytes + it.kvBytes,
                 color: palette[i % palette.count], about: it.estimated)
        }
        let held = items.reduce(0) { $0 + $1.bytes + $1.kvBytes }
        let metal = max(memory?.gpuAllocated ?? 0, held)
        if let g = memory?.gpuAllocated, g > held {
            out.append(Part(
                id: "working", title: String(localized: "Working memory"),
                detail: String(localized: "What the models work in: scratch, caches, buffers"),
                bytes: g - held, color: .orange))
        }
        if let f = load.footprint, f > metal {
            out.append(Part(
                id: "app", title: String(localized: "Valtz itself"),
                detail: String(localized: "The app: the pictures on screen, the project, the rest"),
                bytes: f - metal, color: .gray))
        }
        if let u = load.systemUsed, let f = load.footprint, u > f {
            out.append(Part(
                id: "others", title: String(localized: "Other apps and macOS"),
                detail: "", bytes: u - f,
                color: Color.secondary.opacity(0.35)))
        }
        return out
    }

    static func partName(_ it: MachineMonitor.Memory.Item) -> String {
        switch it.part {
        case "helper": return String(localized: "Helper")
        case "dit": return String(localized: "Diffusion model")
        case "text-encoder": return String(localized: "Text encoder")
        case "vae": return String(localized: "VAE")
        case "weights": return String(localized: "Weights loaded")
        default: return it.part
        }
    }

    /// What it is, its conversations' share, how long it stays.
    static func detail(_ it: MachineMonitor.Memory.Item) -> String {
        var bits = [partName(it)]
        if it.kvBytes > 0 {
            bits.append(String(localized: "\(bytes(it.kvBytes)) of conversations"))
        }
        if let s = it.seconds {
            bits.append(s > 86_400
                ? String(localized: "kept while Valtz runs")
                : s < 60
                ? String(localized: "let go within a minute")
                : String(localized: "kept \(Duration.seconds(s.rounded(.up)).formatted(.units(allowed: [.hours, .minutes], width: .abbreviated))) more"))
        } else if it.parked {
            bits.append(String(localized: "set aside: macOS may take it back"))
        } else if !it.warm {
            bits.append(String(localized: "not kept: let go when nothing uses it"))
        }
        return bits.joined(separator: " · ")
    }

    static func bytes(_ v: Double) -> String {
        Int64(max(0, v)).formatted(.byteCount(style: .memory))
    }
}
