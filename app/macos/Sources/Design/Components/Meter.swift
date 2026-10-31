import SwiftUI

/// "GPU ▬▬▭ 95.0%": a reading's name, its bar -- with `behind`, a second
/// reading drawn under the first, in grey -- and its figures. The
/// Activity popover's machine rows.
struct Meter<Figures: View>: View {
    let name: LocalizedStringKey
    var value: Double?
    var behind: Double?
    var width: CGFloat = VZ.Size.meter
    @ViewBuilder var figures: () -> Figures

    var body: some View {
        HStack(spacing: 6) {
            Text(name).foregroundStyle(.secondary).fixedSize()
            Capsule()
                .fill(.quaternary)
                .frame(width: width, height: VZ.Size.meterBar)
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
}

/// A figure in a place of its own: as wide as `widest` whatever it reads,
/// leading-aligned, so what follows it stays put as the reading changes.
struct FixedFigure: View {
    let text: String
    let widest: String
    var style: HierarchicalShapeStyle = .primary

    var body: some View {
        ZStack(alignment: .leading) {
            Text(verbatim: widest).hidden()
            Text(verbatim: text).foregroundStyle(style)
        }
        .monospacedDigit()
        .fixedSize()
    }
}

/// How the meters write their readings.
enum MeterFormat {
    /// "12.3%", as vpipe's web UI writes it; "—" with no reading.
    static func percent(_ p: Double?) -> String {
        guard let p else { return "—" }
        return (p / 100).formatted(.percent.precision(
            .fractionLength(p >= 100 ? 0 : 1)))
    }

    /// "8.43 GB"; "—" with no reading.
    static func bytes(_ b: Double?) -> String {
        guard let b else { return "—" }
        return Int64(b).formatted(ByteCountFormatStyle(style: .memory))
    }

    /// "12.3"; whole from 100; "—" with no reading.
    static func tflops(_ t: Double?) -> String {
        guard let t else { return "—" }
        return t.formatted(.number.precision(
            .fractionLength(t >= 100 ? 0 : 1)))
    }

    static func ratio(_ used: Double?, _ total: Double?) -> Double? {
        guard let used, let total, total > 0 else { return nil }
        return used / total
    }

    /// The widest figure a Mac's memory can read, as the formatter writes
    /// it: its decimals vary by size ("952.7 MB", "9.3 GB", "93.04 GB",
    /// "931.31 GB"), so the longest of those it would.
    static func widestBytes(total: Double?) -> String {
        var probes: [Int64] = [999_000_000, 99_900_000_000]
        if (total ?? 0) >= 99e9 { probes.append(999_990_000_000) }
        let style = ByteCountFormatStyle(style: .memory)
        return probes.map { $0.formatted(style) }
            .max { $0.count < $1.count } ?? ""
    }
}
