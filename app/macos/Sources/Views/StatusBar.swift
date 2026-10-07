import SwiftUI

/// The status bar along the window's bottom (View › Show Status Bar):
/// the GPU's, memory's and the ANE's load, each a figure and a small
/// meter -- the ANE's last, its figure the one that grows a note (its
/// watts or GB/s) -- and on the right whether the Mac is being held
/// back: vpipe's web UI status bar and its macOS app's thermal row, read
/// the same way (MachineMonitor). The monitor polls only while the bar
/// is shown.
///
/// Memory is two readings in one meter: Valtz's own footprint (the bar in
/// front, the accent's, and the first figure) and the whole Mac's use
/// (the grey bar behind it, and the second figure, of the Mac's total).
///
/// Each figure keeps the width of the widest it can show -- a template
/// of it, every digit as wide as any other in monospaced digits -- so a
/// reading that grows or shrinks never moves the meters after it.
struct StatusBar: View {
    @Bindable var model: AppModel

    private var monitor: MachineMonitor { model.monitor }
    /// Its height: the Prompt Editor leaves as much under the prompt card.
    static let height: CGFloat = 24

    var body: some View {
        let load = monitor.load
        HStack(spacing: 18) {
            meter("GPU", value: load.gpu.map { $0 / 100 }) {
                slot(percent(load.gpu), widest: "100.0%")
            }
            .help("The GPU's utilization, all apps")
            meter("RAM", value: ratio(load.footprint, load.total),
                  behind: ratio(load.systemUsed, load.total)) {
                HStack(spacing: 8) {
                    slot(bytes(load.footprint), widest: widestBytes(load))
                    slot(systemMemory(load),
                         widest: widestBytes(load) + " / "
                             + bytes(load.total),
                         style: .secondary)
                }
            }
            .help("The first figure and the bar in front: the memory Valtz is using, models included (its footprint, as Activity Monitor shows it). The second figure and the grey bar behind it: the memory the whole Mac is using (Activity Monitor's Memory Used), of all it has.")
            meter("ANE", value: load.ane.map { $0 / 100 }) {
                slot(percent(load.ane) + aneDetail(load),
                     widest: "100.0% (88.8 GB/s)")
            }
            .help("The Neural Engine, estimated from its power where the counter is live, else from how long its fabric sat above idle")
            Spacer(minLength: 8)
            HStack(spacing: 5) {
                Image(systemName: monitor.thermalSymbol)
                    .foregroundStyle(monitor.thermalTint)
                Text("Thermal: \(monitor.thermalLabel)")
                    .foregroundStyle(monitor.thermalAlerting
                                     ? AnyShapeStyle(monitor.thermalTint)
                                     : AnyShapeStyle(.secondary))
            }
            .help(monitor.thermalHelp)
        }
        .font(.caption)
        .lineLimit(1)
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity)
        .frame(height: Self.height)
        .background(.bar)
        .overlay(alignment: .top) { Divider() }
        .onAppear { monitor.start() }
        .onDisappear { monitor.stop() }
    }

    /// "GPU ▬▬▭ 95.0%": its bar -- with `behind`, a second reading drawn
    /// under the first, in grey -- and its figures.
    private func meter<Figures: View>(
        _ name: LocalizedStringKey, value: Double?, behind: Double? = nil,
        @ViewBuilder figures: () -> Figures) -> some View {
        HStack(spacing: 6) {
            Text(name).foregroundStyle(.secondary)
            Capsule()
                .fill(.quaternary)
                .frame(width: 36, height: 5)
                .overlay(alignment: .leading) {
                    GeometryReader { g in
                        ZStack(alignment: .leading) {
                            if let behind {
                                Capsule()
                                    .fill(Color.primary.opacity(0.5))
                                    .frame(width: g.size.width
                                           * min(1, max(0, behind)))
                            }
                            Capsule()
                                .fill(Color.accentColor)
                                .frame(width: g.size.width
                                       * min(1, max(0, value ?? 0)))
                        }
                    }
                }
                .opacity(value == nil && behind == nil ? 0.4 : 1)
            figures()
        }
    }

    /// A figure in a place of its own: as wide as `widest` whatever it
    /// reads, leading-aligned, so the meters after it stay put.
    private func slot(_ text: String, widest: String,
                      style: HierarchicalShapeStyle = .primary)
        -> some View {
        ZStack(alignment: .leading) {
            Text(verbatim: widest).hidden()
            Text(verbatim: text).foregroundStyle(style)
        }
        .monospacedDigit()
        .fixedSize()
    }

    /// "12.3%", as the web UI writes it; "—" with no reading.
    private func percent(_ p: Double?) -> String {
        guard let p else { return "—" }
        return (p / 100).formatted(.percent.precision(
            .fractionLength(p >= 100 ? 0 : 1)))
    }

    /// The measured quantity behind the ANE's estimate, as the web UI
    /// adds it: its watts where the energy counter is live, else its
    /// fabric bandwidth -- but not on an idle ANE, whose histogram sits
    /// wholly in its lowest bin and would read as ~1 GB/s of nothing.
    private func aneDetail(_ l: MachineMonitor.Load) -> String {
        if let w = l.aneWatts {
            return " (" + String(format: "%.2f W", w) + ")"
        }
        if let bw = l.aneBandwidth, let p = l.ane, p >= 1 {
            return " (" + String(format: "%.1f GB/s", bw) + ")"
        }
        return ""
    }

    private func ratio(_ used: Double?, _ total: Double?) -> Double? {
        guard let used, let total, total > 0 else { return nil }
        return used / total
    }

    /// "8.43 GB"; "—" with no reading.
    private func bytes(_ b: Double?) -> String {
        guard let b else { return "—" }
        return Int64(b).formatted(ByteCountFormatStyle(style: .memory))
    }

    /// The widest figure this Mac's memory can read, as the formatter
    /// writes it: its decimals vary by size ("952.7 MB", "9.3 GB",
    /// "93.04 GB", "931.31 GB"), so the longest of those it would.
    private func widestBytes(_ l: MachineMonitor.Load) -> String {
        var probes: [Int64] = [999_000_000, 99_900_000_000]
        if (l.total ?? 0) >= 99e9 { probes.append(999_990_000_000) }
        let style = ByteCountFormatStyle(style: .memory)
        return probes.map { $0.formatted(style) }
            .max { $0.count < $1.count } ?? ""
    }

    /// "15.2 GB / 24 GB": the whole Mac's use, of all it has.
    private func systemMemory(_ l: MachineMonitor.Load) -> String {
        let used = bytes(l.systemUsed)
        guard l.total != nil else { return used }
        return used + " / " + bytes(l.total)
    }
}
