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
/// TFLOPS is the same: the arithmetic the GPU achieves (in front) and the
/// Neural Engine's on top of it (grey), of the highest the two have
/// reached together this run -- the bar's full length. In a fleet with a
/// member connected, Fleet reads the whole fleet's so.
///
/// Where the window is too narrow for them all, the ANE's note goes
/// first, then the thermal row's word and the Mac's memory figure.
///
/// Its context menu checks which items show (remembered). A click on RAM
/// shows what Valtz holds, model by model (MemoryPopover).
///
/// Each figure keeps the width of the widest it can show -- a template
/// of it, every digit as wide as any other in monospaced digits -- so a
/// reading that grows or shrinks never moves the meters after it.
struct StatusBar: View {
    @Bindable var model: AppModel

    private var monitor: MachineMonitor { model.monitor }
    /// Its height: the Prompt Editor leaves as much under the prompt card.
    static let height: CGFloat = 24

    /// How much each meter says: everything, then less as the window
    /// narrows.
    private enum Detail { case full, compact, minimal }

    var body: some View {
        ViewThatFits(in: .horizontal) {
            row(.full)
            row(.compact)
            row(.minimal)
        }
        .font(.caption)
        .lineLimit(1)
        .padding(.horizontal, 14)
        .frame(maxWidth: .infinity)
        .frame(height: Self.height)
        .contentShape(Rectangle())
        .contextMenu {
            ForEach(StatusBarItem.allCases, id: \.self) { item in
                Toggle(item.title, isOn: Binding(
                    get: { model.showsStatusItem(item) },
                    set: { model.setStatusItem(item, shown: $0) }))
            }
            Divider()
            Button("Hide Status Bar") {
                withAnimation(AppModel.motion) {
                    model.showsStatusBar = false
                }
            }
        }
        .background(.bar)
        .overlay(alignment: .top) { Divider() }
        .onAppear { monitor.start() }
        .onDisappear {
            monitor.stop()
            // Its popover goes with it, not to come back with the bar.
            model.memoryPopoverOpen = false
        }
    }

    private func row(_ detail: Detail) -> some View {
        let load = monitor.load
        return HStack(spacing: detail == .minimal ? 12 : 18) {
            if model.showsStatusItem(.gpu) {
                meter("GPU", value: load.gpu.map { $0 / 100 }) {
                    slot(percent(load.gpu), widest: "100.0%")
                }
                .help("The GPU's utilization, all apps")
            }
            if model.showsStatusItem(.ram) {
                ramMeter(load, detail)
            }
            if model.showsStatusItem(.ane) {
                aneMeter(load, detail)
            }
            if model.showsStatusItem(.tflops) {
                flopsMeter("TFLOPS", load.flops)
                    .help(flopsHelp(load.flops))
            }
            if model.showsStatusItem(.fleet), let fleet = load.fleet {
                flopsMeter("Fleet", fleet)
                    .help(fleetHelp(fleet))
            }
            Spacer(minLength: 8)
            if model.showsStatusItem(.thermal) {
                thermal(detail)
            }
        }
    }

    private func ramMeter(_ load: MachineMonitor.Load,
                          _ detail: Detail) -> some View {
        meter("RAM", value: ratio(load.footprint, load.total),
              behind: ratio(load.systemUsed, load.total)) {
            HStack(spacing: 8) {
                slot(bytes(load.footprint), widest: widestBytes(load))
                if detail != .minimal {
                    slot(systemMemory(load),
                         widest: widestBytes(load) + " / "
                             + bytes(load.total),
                         style: .secondary)
                }
            }
        }
        .help("The first figure and the bar in front: the memory Valtz is using, models included (its footprint, as Activity Monitor shows it). The second figure and the grey bar behind it: the memory the whole Mac is using (Activity Monitor's Memory Used), of all it has. Click it for what Valtz holds, model by model.")
        // A click: what Valtz holds, model by model.
        .contentShape(Rectangle())
        .onTapGesture { model.memoryPopoverOpen.toggle() }
        .popover(isPresented: $model.memoryPopoverOpen, arrowEdge: .top) {
            MemoryPopover(model: model)
        }
    }

    private func aneMeter(_ load: MachineMonitor.Load,
                          _ detail: Detail) -> some View {
        meter("ANE", value: load.ane.map { $0 / 100 }) {
            if detail == .full {
                slot(percent(load.ane) + aneDetail(load),
                     widest: "100.0% (88.8 GB/s)")
            } else {
                slot(percent(load.ane), widest: "100.0%")
            }
        }
        .help("The Neural Engine, estimated from its power where the counter is live, else from how long its fabric sat above idle")
    }

    /// The thermal state: its glyph, and its word as there is room.
    private func thermal(_ detail: Detail) -> some View {
        HStack(spacing: 5) {
            Image(systemName: monitor.thermalSymbol)
                .foregroundStyle(monitor.thermalTint)
            switch detail {
            case .full:
                Text("Thermal: \(monitor.thermalLabel)")
                    .fixedSize()
                    .foregroundStyle(thermalStyle)
            case .compact:
                Text(verbatim: monitor.thermalLabel)
                    .fixedSize()
                    .foregroundStyle(thermalStyle)
            case .minimal:
                EmptyView()
            }
        }
        .help(monitor.thermalHelp)
    }

    private var thermalStyle: AnyShapeStyle {
        monitor.thermalAlerting ? AnyShapeStyle(monitor.thermalTint)
                                : AnyShapeStyle(.secondary)
    }

    /// "TFLOPS ▬▬▭ 12.3 + 1.2 / 31.2": the GPU's (the bar in front),
    /// the ANE's on top of it (the grey bar), of the highest yet.
    private func flopsMeter(_ name: LocalizedStringKey,
                            _ f: MachineMonitor.Flops?) -> some View {
        meter(name, value: ratio(f?.gpu, f?.peak),
              behind: ratio(f?.total, f?.peak)) {
            HStack(spacing: 6) {
                slot(tflops(f?.gpu), widest: "88.8")
                slot(f.map { "+ " + tflops($0.ane) + " / "
                                 + tflops($0.peak) } ?? "",
                     widest: "+ 88.8 / 88.8", style: .secondary)
            }
        }
    }

    /// "12.3"; whole from 100; "—" with no reading.
    private func tflops(_ t: Double?) -> String {
        guard let t else { return "—" }
        return t.formatted(.number.precision(
            .fractionLength(t >= 100 ? 0 : 1)))
    }

    private func flopsHelp(_ f: MachineMonitor.Flops?) -> String {
        let what = String(localized: "The arithmetic Valtz achieves, in trillions of operations a second (TFLOP/s). The first figure and the bar in front: the GPU's. The second figure and the grey bar after it: the Neural Engine's. The last figure, the bar's full length: the highest the two have reached together since Valtz started. Counted as the models that report it count it: the picture and video models' transformers, not yet their decoders or text encoders.")
        guard f == nil else { return what }
        return what + "\n\n"
            + String(localized: "This engine does not count it.")
    }

    /// The fleet's, and each member's as it told them.
    private func fleetHelp(_ f: MachineMonitor.Flops) -> String {
        var s = String(localized: "The fleet's arithmetic: every member's GPU and Neural Engine TFLOP/s, this Mac's included, added up, of the highest the fleet has reached since Valtz started.")
        let lines = f.members.map { m -> String in
            let name = m.isSelf
                ? String(localized: "\(m.name) (this Mac)") : m.name
            guard let g = m.gpu else {
                return String(localized: "\(name): not told")
            }
            let gpu = tflops(g)
            let ane = tflops(m.ane ?? 0)
            return String(localized: "\(name): GPU \(gpu), ANE \(ane)")
        }
        if !lines.isEmpty { s += "\n\n" + lines.joined(separator: "\n") }
        return s
    }

    /// "GPU ▬▬▭ 95.0%": its bar -- with `behind`, a second reading drawn
    /// under the first, in grey -- and its figures.
    private func meter<Figures: View>(
        _ name: LocalizedStringKey, value: Double?, behind: Double? = nil,
        @ViewBuilder figures: () -> Figures) -> some View {
        HStack(spacing: 6) {
            // Never shortened: a row that does not fit whole gives way
            // to a narrower one (ViewThatFits).
            Text(name).foregroundStyle(.secondary).fixedSize()
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

/// What the status bar can show, each checked or not in its context
/// menu (AppModel.hiddenStatusItems).
enum StatusBarItem: String, CaseIterable, Sendable {
    case gpu, ram, ane, tflops, fleet, thermal

    /// As the bar labels it.
    var title: LocalizedStringKey {
        switch self {
        case .gpu: "GPU"
        case .ram: "RAM"
        case .ane: "ANE"
        case .tflops: "TFLOPS"
        case .fleet: "Fleet"
        case .thermal: "Thermal"
        }
    }
}
