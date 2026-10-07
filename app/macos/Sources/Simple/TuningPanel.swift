import SwiftUI

/// Favor's Custom: the model family's acceleration and quality options
/// (core models/tuning.h) at values of one's own. It opens on what the
/// preset Custom started from settles -- the state it changes -- and
/// every change is settled by the core with the rest: HyperFlow takes a
/// LoRA slot (not the Turbo LoRA's) and runs its own 8 steps, VDN's
/// branch replaces Sol-Attn. An option this Mac or install cannot have is
/// shown, off, with why. Weights dropped on it -- .safetensors files or
/// folders -- join one list: the LoRAs (the preset's Turbo LoRA among
/// them; two run at a time), then the DiT and VAE checkpoints (one of
/// each in place of the model's own).
struct TuningPanel: View {
    @Bindable var model: AppModel
    /// Inside another popover (the Prompt Editor's settings): its rows,
    /// at its width, which that popover scrolls.
    var embedded = false
    @State private var dropTargeted = false
    @State private var contentHeight: CGFloat = 0

    var body: some View {
        if embedded {
            content
                .frame(width: 640, alignment: .leading)
                .modifier(WeightsDrop(model: model, targeted: $dropTargeted))
        } else {
            panel
        }
    }

    private var panel: some View {
        // As tall as it is, up to a cap -- then it scrolls: the LoRA list
        // can grow, and a popover taller than the screen is cut off.
        ScrollView(.vertical) {
            content
                .onGeometryChange(for: CGFloat.self) { $0.size.height }
                    action: { contentHeight = $0 }
        }
        .scrollBounceBehavior(.basedOnSize)
        .frame(width: 640, height: contentHeight > 0
                   ? min(contentHeight, 620) : nil)
        .modifier(WeightsDrop(model: model, targeted: $dropTargeted))
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(verbatim: Preference.custom.label).font(.headline)
                Text(verbatim: model.runningModelInfo?.name ?? "")
                    .foregroundStyle(.secondary)
                Spacer()
                Menu("Start from") {
                    ForEach(Preference.presets) { p in
                        Button(p.label) { model.resetTuning(to: p) }
                    }
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .help("Set every option as a preset has it")
            }
            if let info = model.tuningInfo {
                Grid(alignment: .leading, horizontalSpacing: 12,
                     verticalSpacing: 7) {
                    ForEach(TuningGroup.allCases) { g in
                        let opts = info.options.filter {
                            TuningGroup.of($0) == g
                        }
                        if !opts.isEmpty {
                            GridRow {
                                Text(g.title)
                                    .font(.caption.weight(.semibold))
                                    .foregroundStyle(.secondary)
                                    .gridCellColumns(3)
                                    .padding(.top, g == .steps ? 0 : 6)
                            }
                            if g == .weights {
                                weightRows(info)
                            } else {
                                ForEach(opts) { o in optionRow(o, info) }
                            }
                        }
                    }
                }
            } else {
                Text("No options for this model.")
                    .foregroundStyle(.secondary)
            }
        }
        .controlSize(.small)
        .padding(16)
    }

    @ViewBuilder
    private func optionRow(_ o: TuningOption, _ info: TuningInfo) -> some View {
        let t = info.tuning
        let block = blocker(o, info)
        GridRow {
            Text(Self.title(o))
                .gridColumnAlignment(.trailing)
                .foregroundStyle(block == nil ? .primary : .secondary)
            Group {
                if o.type == "bool" {
                    Toggle(Self.title(o), isOn: Binding(
                        get: { t.flags[o.key] ?? false },
                        set: { model.setTuning(o.key, flag: $0) }))
                        .labelsHidden()
                        .toggleStyle(.switch)
                } else {
                    TuningNumberField(
                        value: t.numbers[o.key] ?? 0, option: o
                    ) { model.setTuning(o.key, number: $0) }
                }
            }
            .disabled(block != nil)
            .gridColumnAlignment(.leading)
            Text(block ?? o.declaredNote ?? Self.note(o.key))
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 340, alignment: .leading)
        }
        .help(o.declaredHelp ?? Self.help(o.key))
    }

    /// The weights, one list numbered through: the LoRAs, a row each --
    /// { its on box and number, its strength, its name, remove } -- two
    /// on at a time (one with HyperFlow, which takes the other slot);
    /// then the DiT and VAE checkpoints, their part where a LoRA's
    /// strength is, one of each on in place of the model's own. The name
    /// comes last, where a long one only takes the room left; then how to
    /// add more.
    @ViewBuilder
    private func weightRows(_ info: TuningInfo) -> some View {
        let loras = info.tuning.loras
        let parts = checkpointEntries(info.tuning)
        ForEach(Array(loras.enumerated()), id: \.element.id) { n, l in
            loraRow(n + 1, l, info)
        }
        ForEach(Array(parts.enumerated()), id: \.element.id) { n, e in
            checkpointRow(loras.count + n + 1, e)
        }
        GridRow {
            Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
            Text("Drop LoRA, DiT or VAE weights on this panel: .safetensors files or folders.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .gridCellColumns(2)
        }
    }

    /// A DiT or VAE checkpoint, as its part's list holds it.
    private struct PartEntry: Identifiable {
        let list: String  // "dits" | "vaes"
        let checkpoint: TuningCheckpoint
        var id: String { list + ":" + checkpoint.path }
    }

    private func checkpointEntries(_ t: Tuning) -> [PartEntry] {
        ["dits", "vaes"].flatMap { list in
            (t.checkpoints[list] ?? []).map {
                PartEntry(list: list, checkpoint: $0)
            }
        }
    }

    private func loraRow(_ number: Int, _ l: TuningLoRA,
                         _ info: TuningInfo) -> some View {
        let turbo = l.path == info.turboLoRA
        let blocked = turbo && (info.tuning.flags["hyperflow"] == true ||
                                info.tuning.flags["taomate"] == true)
        return weightRow(
            number: number, name: info.name(l), path: l.path,
            on: Binding(get: { l.on },
                        set: { model.setLoRA(l.path, on: $0) }),
            missing: l.missing, blocked: blocked,
            onHelp: blocked ? (info.tuning.flags["taomate"] == true
                                   ? String(localized: "TaoMate runs in its place")
                                   : String(localized: "HyperFlow runs in its place"))
                            : String(localized: "Two LoRAs run at a time"),
            fast: turbo,
            remove: { model.removeLoRA(l.path) }) {
            TuningNumberField(value: l.scale, option: Self.strength) {
                model.setLoRA(l.path, scale: $0)
            }
            .help("Strength")
        }
    }

    private func checkpointRow(_ number: Int, _ e: PartEntry) -> some View {
        let c = e.checkpoint
        return weightRow(
            number: number, name: c.name, path: c.path,
            on: Binding(get: { c.on },
                        set: { model.setCheckpoint(e.list, c.path, on: $0) }),
            missing: c.missing, blocked: false,
            onHelp: String(localized: "Runs in place of the model's own (one at a time)"),
            fast: false,
            remove: { model.removeCheckpoint(e.list, c.path) }) {
            Text(verbatim: e.list == "dits" ? "DiT" : "VAE")
                .font(.caption.weight(.medium))
                .foregroundStyle(.secondary)
                .padding(.horizontal, 6)
                .padding(.vertical, 1)
                .background(.quaternary, in: Capsule())
        }
    }

    /// One row of the weights list: { its on box and number, `part`, its
    /// name, remove }.
    private func weightRow<Part: View>(
        number: Int, name: String, path: String, on: Binding<Bool>,
        missing: Bool, blocked: Bool, onHelp: String, fast: Bool,
        remove: @escaping () -> Void,
        @ViewBuilder part: () -> Part
    ) -> some View {
        GridRow {
            HStack(spacing: 6) {
                Toggle(name, isOn: on)
                    .labelsHidden()
                    .toggleStyle(.checkbox)
                    .disabled(missing || blocked)
                    .help(onHelp)
                Text(verbatim: String(number)).monospacedDigit()
            }
            .gridColumnAlignment(.trailing)
            part()
                .gridColumnAlignment(.leading)
            HStack(spacing: 6) {
                if fast {
                    Image(systemName: "hare")
                        .foregroundStyle(.secondary)
                        .help("The model's few-step LoRA: turned on or off, the steps are its preset's")
                }
                Text(verbatim: name)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .foregroundStyle(missing ? .secondary : .primary)
                    .strikethrough(missing)
                    .help(missing
                          ? String(localized: "File not found: \(path)")
                          : path)
                Spacer(minLength: 0)
                Button(action: remove) {
                    Image(systemName: "minus.circle")
                }
                .buttonStyle(.borderless)
                .help("Remove from the list")
            }
            .frame(maxWidth: 340, alignment: .leading)
        }
    }

    /// A LoRA's strength, as the number field takes it.
    private static let strength = TuningOption([
        "key": "strength", "type": "real", "available": true,
        "min": 0.0, "max": 2.0, "step": 0.05,
    ])

    /// Why `o` cannot be changed now, or nil when it can: this Mac or
    /// install cannot have it, another option on has taken its place,
    /// fixed it, or it refines one that is off.
    private func blocker(_ o: TuningOption, _ info: TuningInfo) -> String? {
        if !o.available { return Self.why(o.why) }
        let t = info.tuning
        for other in info.options where t.flags[other.key] == true {
            if other.excludes.contains(o.key) {
                return String(localized: "Replaced by \(Self.title(other))")
            }
            if o.key == "steps", let n = other.fixedSteps {
                let steps = String(n)
                return String(localized: "\(Self.title(other)) runs its own \(steps)")
            }
        }
        if let needs = o.needs, t.flags[needs] != true {
            let name = info.options.first { $0.key == needs }
                .map(Self.title) ?? Self.label(needs)
            return String(localized: "With \(name) on")
        }
        return nil
    }

    /// What an option goes by: an extension's own text, else Valtz's.
    static func title(_ o: TuningOption) -> String {
        o.declaredLabel ?? label(o.key)
    }

    static func label(_ key: String) -> String {
        switch key {
        case "steps": String(localized: "Steps")
        case "hyperflow": String(localized: "HyperFlow")
        case "taomate": String(localized: "TaoMate")
        case "vdn": String(localized: "VDN branch")
        case "sol_attn": String(localized: "Sol-Attn")
        case "sol_tau": String(localized: "Sol threshold")
        case "sage_attn": String(localized: "SageAttention")
        case "i8_gemm": String(localized: "int8 GEMM")
        case "ane_ffn": String(localized: "ANE FFN")
        case "ane_qkv": String(localized: "ANE QKV")
        case "motion_cache": String(localized: "MotionCache")
        case "video_shift": String(localized: "Video shift")
        case "audio_shift": String(localized: "Audio shift")
        case "shift": String(localized: "Shift")
        default: key
        }
    }

    /// The short note beside an option that can be changed.
    static func note(_ key: String) -> String {
        switch key {
        case "steps": String(localized: "Denoising steps")
        case "hyperflow": String(localized: "8-step flow-map adapter; each step ~9% slower")
        case "taomate": String(localized: "3-step streaming method; its cache on disk")
        case "vdn": String(localized: "Linear attention for long, large clips")
        case "sol_attn": String(localized: "Attends only the key blocks that matter")
        case "sol_tau": String(localized: "Higher keeps fewer blocks: faster, less exact")
        case "sage_attn": String(localized: "int8 QK in attention, about 1.2× (M5)")
        case "i8_gemm": String(localized: "int8 block matrix products, about 2×")
        case "ane_ffn": String(localized: "Feed-forward on the Neural Engine, beside the GPU")
        case "ane_qkv": String(localized: "q|k|v projection on the Neural Engine too")
        case "motion_cache": String(localized: "Reuses steps it can predict (16+ steps)")
        case "video_shift": String(localized: "Sigma shift of the video schedule")
        case "audio_shift": String(localized: "Sigma shift of the audio schedule")
        case "shift": String(localized: "Time shift of the noise schedule")
        default: ""
        }
    }

    static func help(_ key: String) -> String {
        switch key {
        case "hyperflow": String(localized: "Video Rebirth's flow-map adapter brings its own 8-step grid. It takes a LoRA slot, in place of the Turbo LoRA; keep the shifts at 12 and 3, which it was trained at.")
        case "taomate": String(localized: "TaoMate-H3's streaming method, not a LoRA: the base model writes the soundtrack, then its adapter writes the video in chunks, 3 steps each, against a cache of the chunks before. It takes a LoRA slot in place of the Turbo LoRA. Words to a clip only, made in whole 5-second requests. That cache is kept on disk (in 8 bits with int8 GEMM) and read back a block at a time. An alternative to the Turbo LoRA: no Favor preset turns it on.")
        case "vdn": String(localized: "VideoDeltaNet: a windowed softmax over nearby frames plus a linear recurrence over the rest, a second checkpoint beside the model. Its advantage grows with the clip's length and size; it replaces Sol-Attn.")
        case "sol_attn": String(localized: "Training-free sparse attention: key blocks a cheap proxy scores low are folded in as their centroid. Saves about a quarter to a third of the denoise on a long clip.")
        case "sol_tau": String(localized: "In standard deviations of the routing score's spread. 1.0 is the published setting.")
        case "sage_attn": String(localized: "SageAttention computes QK in int8 with exact key smoothing. Needs the M5's matrix cores.")
        case "i8_gemm": String(localized: "Dynamic int8 for the block's large matrix products, at int8 quality. Needs the M5's matrix cores.")
        case "ane_ffn": String(localized: "Runs each block's feed-forward on the Apple Neural Engine at the same time as the GPU, its rows split between the two (fp16). Most worthwhile where the GPU has no int8 path (M4); the plan decides whether its module fits.")
        case "ane_qkv": String(localized: "Adds the block's q|k|v projection to the Neural Engine as a second module. Needs ANE FFN: the two are granted together, and the pair costs more memory than the feed-forward alone.")
        case "motion_cache": String(localized: "Before each step, predicts how far the model's answer moved since the last one that ran; when little, reuses it. Little to gain on short schedules.")
        default: ""
        }
    }

    static func why(_ code: String) -> String {
        switch code {
        case "not-installed": String(localized: "Not installed — download it in Models")
        case "no-adapter": String(localized: "This model has no few-step adapter")
        case "needs-matrix-cores": String(localized: "Needs an M5 or later")
        case "not-for-model": String(localized: "Not available for this model")
        case "own-schedule": String(localized: "This model sets its own schedule")
        default: String(localized: "Not available")
        }
    }
}

/// How the panel groups the options.
private enum TuningGroup: CaseIterable, Identifiable {
    case steps, weights, attention, compute, schedule
    var id: Self { self }

    var title: String {
        switch self {
        case .steps: String(localized: "Steps")
        case .weights: String(localized: "LoRAs and checkpoints")
        case .attention: String(localized: "Attention")
        case .compute: String(localized: "Compute")
        case .schedule: String(localized: "Schedule")
        }
    }

    static func of(_ o: TuningOption) -> TuningGroup {
        // An extension's option says where it goes.
        switch o.declaredGroup {
        case "steps": return .steps
        case "attention": return .attention
        case "compute": return .compute
        case .some: return .schedule
        case nil: return of(o.key)
        }
    }

    static func of(_ key: String) -> TuningGroup {
        switch key {
        case "steps", "hyperflow", "taomate": .steps
        case "loras", "dits", "vaes": .weights
        case "vdn", "sol_attn", "sol_tau", "sage_attn": .attention
        case "i8_gemm", "ane_ffn", "ane_qkv", "motion_cache": .compute
        default: .schedule
        }
    }
}

/// A number option: typed (taken on Return or when left, held to its
/// range by the core) or stepped.
private struct TuningNumberField: View {
    let value: Double
    let option: TuningOption
    let commit: (Double) -> Void
    @State private var text = ""
    /// What it last showed: text unlike it is being typed.
    @State private var shown = ""
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 4) {
            TextField(TuningPanel.label(option.key), text: $text)
                .labelsHidden()
                .textFieldStyle(.roundedBorder)
                .multilineTextAlignment(.trailing)
                .font(.callout.monospacedDigit())
                .frame(width: 56)
                .focused($focused)
                .onSubmit(take)
                // ↑ / ↓ step it, as the arrows beside it do.
                .onKeyPress(.upArrow) { nudge(1); return .handled }
                .onKeyPress(.downArrow) { nudge(-1); return .handled }
            Stepper(TuningPanel.label(option.key),
                    onIncrement: { nudge(1) }, onDecrement: { nudge(-1) })
                .labelsHidden()
        }
        .onAppear { show(display(value)) }
        // What the core settled shows, focused or not, unless a value is
        // being typed in it.
        .onChange(of: value) { _, v in
            if text == shown { show(display(v)) }
        }
        .onChange(of: focused) { _, f in
            if !f { take() }
        }
    }

    /// One step from what the field holds -- typed and not yet taken, or
    /// the value -- shown at once and committed, focused or not.
    private func nudge(_ dir: Double) {
        let base = typed ?? value
        let v = Swift.min(option.max,
                          Swift.max(option.min, base + dir * option.step))
        show(display(v))
        if v != value { commit(v) }
    }

    private func show(_ s: String) {
        text = s
        shown = s
    }

    private var typed: Double? {
        Double(text.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ",", with: "."))
    }

    private func take() {
        if let v = typed, v != value { commit(v) }
        show(display(value))
    }

    /// Whole steps; a shift or threshold to its step's precision.
    private func display(_ v: Double) -> String {
        if option.type == "int" { return String(Int(v.rounded())) }
        let digits = option.step < 0.1 ? 2 : 1
        return String(format: "%.\(digits)f", v)
    }
}

/// Weights dropped anywhere on the panel: each filed by what it is -- a
/// LoRA, a DiT or a VAE checkpoint.
private struct WeightsDrop: ViewModifier {
    let model: AppModel
    @Binding var targeted: Bool

    func body(content: Content) -> some View {
        content
            .dropDestination(for: URL.self) { urls, _ in
                model.addWeights(urls)
            } isTargeted: { targeted = $0 }
            .overlay {
                if targeted {
                    RoundedRectangle(cornerRadius: 10, style: .continuous)
                        .strokeBorder(Color.accentColor,
                                      style: StrokeStyle(lineWidth: 2,
                                                         dash: [7, 5]))
                        .padding(4)
                        .allowsHitTesting(false)
                }
            }
    }
}
