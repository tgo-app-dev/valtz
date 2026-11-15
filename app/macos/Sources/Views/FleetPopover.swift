import SwiftUI

/// The Fleet meter's popover -- a click on the status bar's Fleet (DESIGN
/// §10a Status bar, §11c): each member's arithmetic as it told it (GPU
/// and Neural Engine TFLOP/s, this Mac's among them), each bar of the
/// fleet's highest; and, PAIRED over Thunderbolt (Settings › Fleet's
/// Pair), the split's link -- what this Mac sends the member and
/// receives from it, each a bar of what the cables carry that way (their
/// speeds summed: Thunderbolt is full duplex). Read with the bar's own
/// poll, every second (MachineMonitor; Controller::pair_traffic_, the
/// plugin's valtz-tp counting each exchange).
struct FleetPopover: View {
    @Bindable var model: AppModel

    private var fleet: MachineMonitor.Flops? { model.monitor.load.fleet }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            if let f = fleet {
                HStack(alignment: .firstTextBaseline) {
                    Text("Fleet").font(.headline)
                    Spacer()
                    Text("\(Self.tflops(f.total)) of \(Self.tflops(f.peak)) TFLOP/s")
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
                members(f)
                if let p = f.pair {
                    Divider()
                    link(p)
                }
            } else {
                Text("No member is connected.")
                    .foregroundStyle(.secondary)
            }
        }
        .padding(16)
        .frame(width: 380)
    }

    // MARK: - Members

    private func members(_ f: MachineMonitor.Flops) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            ForEach(Array(f.members.enumerated()), id: \.offset) { _, m in
                VStack(alignment: .leading, spacing: 4) {
                    HStack(alignment: .firstTextBaseline) {
                        Text(verbatim: m.name).lineLimit(1)
                            .truncationMode(.middle)
                        if m.isSelf {
                            Text("This Mac").font(.caption)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        if let g = m.gpu {
                            let a = m.ane ?? 0
                            Text("\(Self.tflops(g + a)) TFLOP/s")
                                .monospacedDigit()
                        } else {
                            Text("Not told").foregroundStyle(.secondary)
                        }
                    }
                    Bar(value: Self.ratio(m.gpu, f.peak),
                        behind: Self.ratio(m.gpu.map { $0 + (m.ane ?? 0) },
                                           f.peak))
                    if let g = m.gpu {
                        Text("GPU \(Self.tflops(g)) · ANE \(Self.tflops(m.ane ?? 0))")
                            .font(.caption).monospacedDigit()
                            .foregroundStyle(.secondary)
                    }
                }
            }
        }
    }

    // MARK: - The paired link

    private func link(_ p: MachineMonitor.Pair) -> some View {
        VStack(alignment: .leading, spacing: 9) {
            HStack(alignment: .firstTextBaseline) {
                Text("Thunderbolt Split").font(.headline)
                Spacer()
                Text(verbatim: linkKind(p)).font(.caption)
                    .foregroundStyle(.secondary)
            }
            direction(String(localized: "Sent to \(p.name)"), p.sent, p)
            direction(String(localized: "Received from \(p.name)"),
                      p.received, p)
            Text(verbatim: linkNote(p))
                .font(.caption).foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    private func direction(_ title: String, _ rate: Double,
                           _ p: MachineMonitor.Pair) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(alignment: .firstTextBaseline) {
                Text(verbatim: title).lineLimit(1).truncationMode(.middle)
                Spacer()
                Group {
                    if let c = p.capacity {
                        Text("\(Self.rate(rate)) of \(Self.rate(c))")
                    } else {
                        Text(verbatim: Self.rate(rate))
                    }
                }
                .monospacedDigit()
            }
            Bar(value: Self.ratio(rate, p.capacity), behind: nil)
                .opacity(p.capacity == nil ? 0.4 : 1)
        }
    }

    /// "2 cables · RDMA"
    private func linkKind(_ p: MachineMonitor.Pair) -> String {
        let cables = p.links == 1
            ? String(localized: "1 cable")
            : String(localized: "\(p.links) cables")
        let how = p.rdma ? String(localized: "RDMA")
                         : String(localized: "TCP")
        return cables + " · " + how
    }

    private func linkNote(_ p: MachineMonitor.Pair) -> String {
        if p.links == 0 {
            return String(localized: "No Thunderbolt cable to \(p.name) has been found: a clip is made here whole.")
        }
        if !p.active {
            return String(localized: "No clip is split now. While one is, its partial sums cross here, each way at most what the cables carry.")
        }
        if p.capacity == nil {
            return String(localized: "A clip is split now. The cables' speed is not known, so the bars show nothing.")
        }
        return String(localized: "A clip is split now: its partial sums, each way, of what the cables carry that way.")
    }

    // MARK: - Figures

    private static func ratio(_ v: Double?, _ of: Double?) -> Double? {
        guard let v, let of, of > 0 else { return nil }
        return v / of
    }

    /// "12.3"; whole from 100.
    static func tflops(_ t: Double) -> String {
        t.formatted(.number.precision(.fractionLength(t >= 100 ? 0 : 1)))
    }

    /// "3.21 GB/s", "640 MB/s": decimal, as a link's speed is said.
    static func rate(_ bytesPerSecond: Double) -> String {
        let b = max(0, bytesPerSecond)
        if b >= 1e9 {
            let gb = (b / 1e9).formatted(.number.precision(
                .fractionLength(b >= 1e11 ? 0 : 2)))
            return String(localized: "\(gb) GB/s")
        }
        let mb = (b / 1e6).formatted(.number.precision(.fractionLength(0)))
        return String(localized: "\(mb) MB/s")
    }

    /// A capsule bar: `value` in the accent, `behind` in grey under it.
    private struct Bar: View {
        var value: Double?
        var behind: Double?

        var body: some View {
            Capsule()
                .fill(.quaternary)
                .frame(height: 6)
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
        }
    }
}
