import SwiftUI

/// The toolbar's Activity item (DESIGN-SYSTEM: one place for what runs):
/// a ring that fills as the task the stage watches comes along -- or as
/// the first running task does -- with the count of what is queued
/// beside it; a gauge while nothing runs, the count beside it for what
/// waits. It is on every screen, and stays while the window is given
/// over to a fleet job (the popover's own buttons go). Its popover lists
/// the tasks, each with Stop, the export under way, and the machine: its
/// GPU, memory, Neural Engine, arithmetic and thermal state
/// (MachineMonitor polls only while it is open), with what Valtz holds
/// in memory under a fold (MemorySection).
struct ActivityButton: View {
    @Bindable var model: AppModel
    @State private var hovering = false

    private var count: Int {
        model.tasks.count + (model.exportShow == nil ? 0 : 1)
    }
    /// Something is under way -- not merely queued, or waiting for a
    /// download: the ring turns only then.
    private var running: Bool {
        model.tasks.contains(where: \.running) || model.exportShow != nil
    }
    private var busy: Bool { count > 0 }
    /// The phase's count: the watched task's, else the first running one's.
    private var fraction: Double? {
        if let f = model.generationPhase?.fraction { return f }
        return model.tasks.first(where: \.running)
            .flatMap { model.jobs[$0.job]?.phase?.fraction }
    }

    var body: some View {
        Button {
            model.activityOpen.toggle()
        } label: {
            HStack(spacing: VZ.Space.xs) {
                if running {
                    ActivityRing(fraction: fraction, diameter: 16)
                } else {
                    Image(systemName: "gauge.with.dots.needle.bottom.50percent")
                        .font(.system(size: VZ.Size.toolbarSymbol))
                }
                if showsCount {
                    Text(verbatim: "\(count)")
                        .font(.vzCaption.monospacedDigit().weight(.semibold))
                        .foregroundStyle(.secondary)
                }
            }
            .frame(minWidth: VZ.Size.toolbarButton)
            .frame(height: VZ.Size.toolbarButton)
            .padding(.horizontal, showsCount ? VZ.Space.s : 0)
            .background {
                Capsule().fill(Color.primary.opacity(
                    model.activityOpen ? 0.14 : hovering ? 0.06 : 0))
            }
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .animation(VZ.Motion.quick, value: hovering)
        .accessibilityLabel(Text("Activity"))
        .accessibilityValue(Text(verbatim: accessibilityValue))
        .accessibilityAddTraits(model.activityOpen ? .isSelected : [])
        .help(busy ? "Activity: what is running, and the machine's load"
                   : "Activity: nothing running; the machine's load")
        .popover(isPresented: $model.activityOpen, arrowEdge: .bottom) {
            ActivityPopover(model: model)
        }
    }

    /// The count, past the one the ring stands for; every one when
    /// nothing runs yet.
    private var showsCount: Bool { count > (running ? 1 : 0) }

    private var accessibilityValue: String {
        guard busy else { return String(localized: "Nothing running") }
        var s = running ? model.generationCaption : ""
        if s.isEmpty {
            s = running ? String(localized: "Running")
                        : String(localized: "Waiting")
        }
        if count > 1 {
            s += ", " + String(localized: "\(String(count)) tasks")
        }
        return s
    }
}

/// What the Activity item opens.
struct ActivityPopover: View {
    @Bindable var model: AppModel

    private var monitor: MachineMonitor { model.monitor }

    var body: some View {
        VStack(alignment: .leading, spacing: VZ.Space.l) {
            Text("Activity").font(.vzHeading)
            tasks
            Divider()
            machine
            DisclosureGroup("Memory") {
                MemorySection(model: model, showsTitle: false)
                    .padding(.top, VZ.Space.s)
            }
        }
        .padding(VZ.Space.l)
        .frame(width: 420)
        .onAppear { monitor.start() }
        .onDisappear { monitor.stop() }
    }

    @ViewBuilder
    private var tasks: some View {
        if model.tasks.isEmpty, model.exportShow == nil {
            EmptyState.NoActivity()
                .frame(height: 120)
        } else {
            VStack(alignment: .leading, spacing: VZ.Space.s) {
                ForEach(model.tasks) { t in
                    taskRow(t)
                }
                if let e = model.exportShow,
                   !model.tasks.contains(where: { $0.job == e.job }) {
                    exportRow(e)
                }
            }
        }
    }

    /// A task: a button that shows it on the stage (so VoiceOver and the
    /// keyboard reach it), and its Stop beside.
    private func taskRow(_ t: TaskDTO) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: VZ.Space.s) {
            Button {
                model.showTask(t.job)
            } label: {
                HStack(alignment: .firstTextBaseline, spacing: VZ.Space.s) {
                    Text(verbatim: t.badge)
                        .font(.vzCaption.weight(.semibold).monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: VZ.Size.badgeColumn, alignment: .leading)
                    VStack(alignment: .leading, spacing: 1) {
                        Text(verbatim: t.title.isEmpty ? t.op : t.title)
                            .lineLimit(1).truncationMode(.middle)
                        Text(verbatim: caption(t))
                            .font(.vzMeta).foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text(verbatim: t.title.isEmpty ? t.op : t.title))
            .accessibilityValue(Text(verbatim: caption(t)))
            .accessibilityHint(Text("Shows it on the stage"))
            .help("Click to watch it on the stage")
            Button(t.running ? "Stop" : "Remove") {
                model.cancel(job: t.job)
            }
            .controlSize(.small)
            .disabled(model.windowGivenOver)
        }
    }

    private func exportRow(_ e: AppModel.ExportShow) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: VZ.Space.s) {
            Image(systemName: "square.and.arrow.up")
                .foregroundStyle(.secondary)
                .frame(width: VZ.Size.badgeColumn, alignment: .leading)
            VStack(alignment: .leading, spacing: 1) {
                Text(verbatim: e.file).lineLimit(1).truncationMode(.middle)
                Text(verbatim: model.jobs[e.job]?.phase?.caption(kind: .video)
                     ?? String(localized: "Exporting…"))
                    .font(.vzMeta).foregroundStyle(.secondary)
            }
            Spacer(minLength: VZ.Space.s)
            Button("Stop") { model.stopExport() }
                .controlSize(.small)
                .disabled(model.windowGivenOver)
        }
    }

    /// What a task is doing, or how many run before it -- and, sent to a
    /// fleet member, where (DESIGN §11).
    private func caption(_ t: TaskDTO) -> String {
        let doing: String
        if t.running {
            if let p = model.jobs[t.job]?.phase {
                let kind: Modality = t.op.contains("video") ? .video
                    : t.op.contains("audio") || t.op.contains("speech")
                    ? .audio : .image
                doing = p.caption(kind: kind)
            } else {
                doing = String(localized: "Preparing…")
            }
        } else if t.waits {
            return AppModel.waitingCaption(t)
        } else if t.runner != "local" && t.position == 0 {
            return String(localized: "Sent to \(t.runner)")
        } else {
            doing = t.position <= 1
                ? String(localized: "Queued · next")
                : String(localized: "Queued · \(String(t.position)) ahead")
        }
        return t.runner == "local" ? doing
            : String(localized: "\(doing) · on \(t.runner)")
    }

    /// The machine's load: each a meter, as vpipe's own front ends read
    /// them (MachineMonitor).
    private var machine: some View {
        let load = monitor.load
        return VStack(alignment: .leading, spacing: VZ.Space.s) {
            Grid(alignment: .leading, horizontalSpacing: VZ.Space.m,
                 verticalSpacing: VZ.Space.s) {
                GridRow {
                    Meter(name: "GPU", value: load.gpu.map { $0 / 100 }) {
                        FixedFigure(text: MeterFormat.percent(load.gpu),
                                    widest: "100.0%")
                    }
                    .help("The GPU's utilization, all apps")
                    Meter(name: "ANE", value: load.ane.map { $0 / 100 }) {
                        FixedFigure(text: MeterFormat.percent(load.ane)
                                        + aneDetail(load),
                                    widest: "100.0% (88.8 GB/s)")
                    }
                    .help("The Neural Engine, estimated from its power where the counter is live, else from how long its fabric sat above idle")
                }
                GridRow {
                    Meter(name: "RAM",
                          value: MeterFormat.ratio(load.footprint, load.total),
                          behind: MeterFormat.ratio(load.systemUsed, load.total)) {
                        HStack(spacing: VZ.Space.s) {
                            FixedFigure(
                                text: MeterFormat.bytes(load.footprint),
                                widest: MeterFormat.widestBytes(total: load.total))
                            FixedFigure(
                                text: systemMemory(load),
                                widest: MeterFormat.widestBytes(total: load.total)
                                    + " / " + MeterFormat.bytes(load.total),
                                style: .secondary)
                        }
                    }
                    .gridCellColumns(2)
                    .help("The first figure and the bar in front: the memory Valtz is using, models included (its footprint, as Activity Monitor shows it). The second figure and the grey bar behind it: the memory the whole Mac is using (Activity Monitor's Memory Used), of all it has.")
                }
                GridRow {
                    flopsMeter("TFLOPS", load.flops)
                        .help(flopsHelp(load.flops))
                    if let fleet = load.fleet {
                        flopsMeter("Fleet", fleet)
                            .help(fleetHelp(fleet))
                    } else {
                        Color.clear.frame(height: 1)
                    }
                }
            }
            .font(.vzCaption)
            HStack(spacing: 5) {
                Image(systemName: monitor.thermalSymbol)
                    .foregroundStyle(monitor.thermalTint)
                Text("Thermal: \(monitor.thermalLabel)")
                    .foregroundStyle(monitor.thermalAlerting
                                     ? AnyShapeStyle(monitor.thermalTint)
                                     : AnyShapeStyle(.secondary))
            }
            .font(.vzCaption)
            .help(monitor.thermalHelp)
        }
    }

    /// "TFLOPS ▬▬▭ 12.3 + 1.2 / 31.2": the GPU's (the bar in front), the
    /// ANE's on top of it (the grey bar), of the highest yet.
    private func flopsMeter(_ name: LocalizedStringKey,
                            _ f: MachineMonitor.Flops?) -> some View {
        Meter(name: name, value: MeterFormat.ratio(f?.gpu, f?.peak),
              behind: MeterFormat.ratio(f?.total, f?.peak)) {
            HStack(spacing: 6) {
                FixedFigure(text: MeterFormat.tflops(f?.gpu), widest: "88.8")
                FixedFigure(text: f.map { "+ " + MeterFormat.tflops($0.ane)
                                          + " / " + MeterFormat.tflops($0.peak) }
                                  ?? "",
                            widest: "+ 88.8 / 88.8", style: .secondary)
            }
        }
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
            let gpu = MeterFormat.tflops(g)
            let ane = MeterFormat.tflops(m.ane ?? 0)
            return String(localized: "\(name): GPU \(gpu), ANE \(ane)")
        }
        if !lines.isEmpty { s += "\n\n" + lines.joined(separator: "\n") }
        return s
    }

    /// The measured quantity behind the ANE's estimate: its watts where
    /// the energy counter is live, else its fabric bandwidth -- but not on
    /// an idle ANE, whose histogram sits wholly in its lowest bin.
    private func aneDetail(_ l: MachineMonitor.Load) -> String {
        if let w = l.aneWatts {
            return " (" + String(format: "%.2f W", w) + ")"
        }
        if let bw = l.aneBandwidth, let p = l.ane, p >= 1 {
            return " (" + String(format: "%.1f GB/s", bw) + ")"
        }
        return ""
    }

    /// "15.2 GB / 24 GB": the whole Mac's use, of all it has.
    private func systemMemory(_ l: MachineMonitor.Load) -> String {
        let used = MeterFormat.bytes(l.systemUsed)
        guard l.total != nil else { return used }
        return used + " / " + MeterFormat.bytes(l.total)
    }
}
