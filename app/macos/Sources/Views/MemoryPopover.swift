import SwiftUI

/// RAM's popover -- a click on the status bar's meter (DESIGN §10a): what
/// Valtz holds in memory, drawn as Settings › Storage draws the disk. The
/// bar is the Mac's memory: each model the engine holds, once -- KEPT
/// WARM between jobs (the helper kept loaded; a picture's DiT, text
/// encoder and VAE kept built, DESIGN §4n), with the time it has left,
/// a checkpoint still resident, or RUNNING -- and what it holds by use:
/// its weights, the blocks it streams, its expert cache, its
/// conversations' K/V, its scratch (vpipe's `owned`, DESIGN §15 0zzz;
/// all of that read as working memory before, 2026-11-05); then the
/// WORKING memory no model claims, the rest of Valtz (its footprint past
/// Metal's), the other apps, and what is free. Read from the engine
/// every two seconds while it is open (MachineMonitor.startMemory;
/// vpipe's memory_report, DESIGN §15 0zz).
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
            } else if monitor.memory?.models.contains(where: \.warm) != true {
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
                if !p.status.isEmpty {
                    Text(verbatim: p.status)
                        .font(.caption).foregroundStyle(.tertiary)
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
        /// How it is held: kept, set aside, let go after.
        var status = ""
        let bytes: Double
        let color: Color
        /// From its files' size: the engine counts none of it.
        var about = false
    }

    private static let palette: [Color] = [
        .purple, .blue, .teal, .indigo, .pink, .mint, .cyan, .brown,
    ]

    /// The bar's shares: the models held, largest first, then the
    /// working memory no model claims, the rest of Valtz, the other apps.
    static func parts(load: MachineMonitor.Load,
                      memory: MachineMonitor.Memory?) -> [Part] {
        let models = (memory?.models ?? []).sorted { $0.bytes > $1.bytes }
        var out = models.enumerated().map { i, m in
            Part(id: m.id, title: m.name.isEmpty ? partList(m) : m.name,
                 detail: uses(m), status: status(m), bytes: m.bytes,
                 color: palette[i % palette.count], about: m.estimated)
        }
        let held = models.reduce(0) { $0 + $1.bytes }
        let metal = max(memory?.gpuAllocated ?? 0, held)
        if let g = memory?.gpuAllocated, g > held {
            out.append(Part(
                id: "working", title: String(localized: "Working memory"),
                detail: String(localized: "What no model claims: buffers the engine shares, work between models"),
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

    static func partName(_ part: String) -> String {
        switch part {
        case "helper": return String(localized: "Helper")
        case "dit": return String(localized: "Diffusion model")
        case "text-encoder": return String(localized: "Text encoder")
        case "vae": return String(localized: "VAE")
        case "weights": return String(localized: "Weights loaded")
        default: return part
        }
    }

    /// Its kept parts, named: "Diffusion model, VAE".
    static func partList(_ m: MachineMonitor.Memory.Model) -> String {
        m.parts.map(partName).joined(separator: ", ")
    }

    /// What it holds, by use, largest first: "Scratch 40.5 GB · Streamed
    /// blocks 30.2 GB · Weights 371 MB".
    static func uses(_ m: MachineMonitor.Memory.Model) -> String {
        m.uses.filter { $0.value >= 1_048_576 }
            .sorted { $0.value > $1.value }
            .map { use, v in
                let b = bytes(v)
                switch use {
                case "weights": return String(localized: "Weights \(b)")
                case "blocks": return String(localized: "Streamed blocks \(b)")
                case "experts": return String(localized: "Expert cache \(b)")
                case "kv": return String(localized: "Conversations \(b)")
                case "scratch": return String(localized: "Scratch \(b)")
                default: return "\(use) \(b)"
                }
            }
            .joined(separator: " · ")
    }

    /// How it is held: its kept parts and how long they stay, set aside,
    /// or let go when nothing uses it.
    static func status(_ m: MachineMonitor.Memory.Model) -> String {
        var bits: [String] = []
        if !m.parts.isEmpty, !m.name.isEmpty {
            bits.append(partList(m))
        }
        if let s = m.seconds {
            bits.append(s > 86_400
                ? String(localized: "kept while Valtz runs")
                : s < 60
                ? String(localized: "let go within a minute")
                : String(localized: "kept \(Duration.seconds(s.rounded(.up)).formatted(.units(allowed: [.hours, .minutes], width: .abbreviated))) more"))
        } else if m.parked {
            bits.append(String(localized: "set aside: macOS may take it back"))
        } else {
            bits.append(String(localized: "not kept: let go when nothing uses it"))
        }
        return bits.joined(separator: " · ")
    }

    static func bytes(_ v: Double) -> String {
        Int64(max(0, v)).formatted(.byteCount(style: .memory))
    }
}
