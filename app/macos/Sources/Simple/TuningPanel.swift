import SwiftUI

/// Favor's Custom: the model family's acceleration and quality options
/// (core models/tuning.h) at values of one's own. It opens on what the
/// preset Custom started from settles -- the state it changes -- and
/// every change is settled by the core with the rest: HyperFlow takes a
/// LoRA slot (not the Turbo LoRA's) and runs its own 8 steps, VDN's
/// branch replaces Sol-Attn. An option this Mac or install cannot have is
/// shown, off, with why. Weights dropped on it -- .safetensors files or
/// folders -- join one list: the LoRAs (the preset's Turbo LoRA among
/// them; eight run at a time), then the DiT and VAE checkpoints (one of
/// each in place of the model's own).
struct TuningPanel: View {
    /// Whose values: the card's (AppModel), a generate node's.
    var model: any TuningHost
    /// Inside another popover (the Prompt Editor's settings): its rows,
    /// at its width, which that popover scrolls.
    var embedded = false
    @State private var dropTargeted = false
    @State private var contentSize: CGSize = .zero
    /// The popover holding it, which it tells its size.
    @Environment(\.popoverFit) private var fit

    var body: some View {
        if embedded {
            // At least its usual width; wider rows widen the popover
            // (which scrolls) rather than spill out of a fixed one.
            content
                .frame(minWidth: 640, alignment: .leading)
                .modifier(WeightsDrop(model: model, targeted: $dropTargeted))
        } else {
            panel
        }
    }

    private var panel: some View {
        // As large as it is, up to its popover's room -- then it scrolls:
        // the LoRA list can grow, a clip regenerated adds the refine's
        // options, a long name widens the Acceleration pop-up, and a
        // popover larger than the screen is cut off. At 640 points wide
        // at least (it was 640 always: rows wider than that were cut on
        // both sides), as tall as it lays out at that width, 620 at most.
        // The POPOVER takes that size (appKitPopover(fitted:)); the panel
        // fills what it is given. It framed itself at what it measured,
        // which AppKit's popover followed inside its own layout: on macOS
        // 27.2 opening it hung, each frame given asking for another
        // constraints pass, until AppKit raised and Valtz crashed
        // (reported with MiniMax H3 Ref2VA).
        let wide = contentSize.width > (fit?.room.width ?? .infinity)
        return ScrollView(wide ? [.vertical, .horizontal] : .vertical) {
            AtIdealWidth(minWidth: 640) { content }
                .onGeometryChange(for: CGSize.self) { $0.size }
                    action: { s in
                        contentSize = s
                        fit?.fit(CGSize(width: s.width,
                                        height: min(s.height, 620)))
                    }
        }
        .scrollBounceBehavior(.basedOnSize)
        .modifier(WeightsDrop(model: model, targeted: $dropTargeted))
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(alignment: .firstTextBaseline) {
                Text(verbatim: Preference.custom.label).font(.headline)
                Text(verbatim: model.tuningTitle)
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
                        // The refine's options while the clip is
                        // regenerated (past 768P, its refiner here).
                        if !opts.isEmpty,
                           g != .regenerate || model.tuningRegenerates {
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
                                if g == .regenerate {
                                    regenerateNote
                                }
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

    /// What the Regenerate group's options are about: the refine at the
    /// size asked, after the draft -- and where they come from out of
    /// Custom.
    private var regenerateNote: some View {
        GridRow {
            Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
            Text("The refine at the size asked, after the draft: Open-H3-Regenerate-2K's three steps, its latent upscaler and its decoder. Out of Custom its Sol, Sage and int8 are Favor's preset's, the rest as Settings › Upscaling sets it.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 480, alignment: .leading)
                .gridCellColumns(2)
        }
    }

    @ViewBuilder
    private func optionRow(_ o: TuningOption, _ info: TuningInfo) -> some View {
        let t = info.tuning
        let block = Self.blocker(o, info)
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
                } else if o.type == "choice" {
                    Picker(Self.title(o), selection: Binding(
                        get: { t.choices[o.key] ?? o.choices.first ?? "" },
                        set: { model.setTuning(o.key, choice: $0) })) {
                        ForEach(o.choices, id: \.self) { c in
                            Text(Self.choiceLabel(o.key, c)).tag(c)
                                .selectionDisabled(
                                    Self.isWithheld(o, c, t))
                        }
                    }
                    .labelsHidden()
                    .fixedSize()
                } else {
                    TuningNumberField(
                        value: t.numbers[o.key] ?? 0, option: o,
                        schedule: t.schedules[o.key],
                        commit: { model.setTuning(o.key, number: $0) },
                        commitSchedule: {
                            model.setTuning(o.key, schedule: $0)
                        })
                }
            }
            .disabled(block != nil)
            .gridColumnAlignment(.leading)
            Text(block ?? o.declaredNote
                 ?? (o.type == "choice"
                     ? Self.choiceNote(o, info)
                     : Self.note(o)))
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(maxWidth: 340, alignment: .leading)
        }
        .help(o.declaredHelp ?? Self.help(o))
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
        if let fast = info.loraOption?.fast, !fast.isEmpty {
            fastRow(fast, info)
        }
        ForEach(Array(loras.enumerated()), id: \.element.id) { n, l in
            loraRow(n + 1, l, info)
        }
        ForEach(Array(parts.enumerated()), id: \.element.id) { n, e in
            checkpointRow(loras.count + n + 1, e)
        }
        GridRow {
            Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
            Text("Drop LoRA, DiT or VAE weights on this panel: .safetensors files or folders, or a Draw Things .ckpt of a DiT.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .gridCellColumns(2)
        }
    }

    /// The quick pick of the model's ACCELERATION LoRAs -- its few-step
    /// adapters as Capabilities lists them, not HyperFlow's, Pika H3
    /// Flex's or TaoMate's
    /// (their own switches) -- or none: the one picked goes into the list
    /// on, with its own steps and shifts. One not downloaded is listed,
    /// not offered.
    private func fastRow(_ fast: [FastLoRA], _ info: TuningInfo) -> some View {
        let on = Set(info.tuning.loras.filter(\.on).map(\.path))
        let current = fast.first { $0.installed && on.contains($0.path) }
        return GridRow {
            Text("Acceleration")
                .gridColumnAlignment(.trailing)
            Picker("Acceleration", selection: Binding(
                get: { current?.id ?? "" },
                set: { model.setFastLoRA($0.isEmpty ? nil : $0) })) {
                Text("None").tag("")
                Divider()
                ForEach(fast) { f in
                    Text(f.installed
                         ? f.name
                         : String(localized: "\(f.name) — not downloaded"))
                        .tag(f.id)
                        .selectionDisabled(!f.installed)
                }
            }
            .labelsHidden()
            .fixedSize()
            .gridColumnAlignment(.leading)
            Text(current.map { f in
                String(localized: "\(String(f.steps)) steps, its own shifts")
            } ?? String(localized: "The model's own steps"))
                .font(.caption)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 340, alignment: .leading)
        }
        .help("A few-step LoRA distilled from the model: picked, it runs at the steps and shifts it was made for. HyperFlow, Pika H3 Flex and TaoMate have switches of their own.")
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
                                info.tuning.flags["pika_flex"] == true ||
                                info.tuning.flags["taomate"] == true)
        return weightRow(
            number: number, name: info.name(l), path: l.path,
            on: Binding(get: { l.on },
                        set: { model.setLoRA(l.path, on: $0) }),
            missing: l.missing, blocked: blocked,
            onHelp: blocked ? (info.tuning.flags["taomate"] == true
                                   ? String(localized: "TaoMate runs in its place")
                                   : info.tuning.flags["pika_flex"] == true
                                   ? String(localized: "Pika H3 Flex runs in its place")
                                   : String(localized: "HyperFlow runs in its place"))
                            : String(localized: "Eight LoRAs run at a time"),
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
                // Its file moved or deleted: said on the row, not only in
                // its tooltip; Start leaves it out and says so. Kept on
                // as it was, it runs again once the file is back; (-)
                // takes it off the list.
                if missing {
                    Text("Not found")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.red)
                        .padding(.horizontal, 6)
                        .padding(.vertical, 1)
                        .background(Color.red.opacity(0.14), in: Capsule())
                        .fixedSize()
                        .help(String(localized: "File not found: \(path)"))
                }
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
    /// install cannot have it, a choice in force sets it (the ANE's int8
    /// the int8 tier), another option on has taken its place, fixed it,
    /// or it refines one that is off. Settings › Upscaling's too.
    static func blocker(_ o: TuningOption, _ info: TuningInfo) -> String? {
        if !o.available { return why(o.why) }
        let t = info.tuning
        if let by = info.setBy[o.key] {
            let name = info.options.first { $0.key == by }
                .map(title) ?? label(by)
            let value = choiceLabel(by, t.choices[by] ?? "")
            return String(localized: "Follows \(name): \(value)")
        }
        for other in info.options where t.flags[other.key] == true {
            if other.excludes.contains(o.key) {
                return String(localized: "Replaced by \(title(other))")
            }
            if o.key == "steps", let n = other.fixedSteps {
                let steps = String(n)
                return String(localized: "\(title(other)) runs its own \(steps)")
            }
            // Any other value it decides (TaoMate's shifts), shown as set.
            if o.key != "steps", let v = other.fixes[o.key] {
                let value = v == v.rounded() ? String(Int(v)) : String(v)
                return String(localized: "\(title(other)) sets it to \(value)")
            }
        }
        if let needs = o.needs, t.flags[needs] != true {
            let name = info.options.first { $0.key == needs }
                .map(title) ?? label(needs)
            return String(localized: "With \(name) on")
        }
        return nil
    }

    /// A choice's value a switch on withholds now (the int8 tier's Fast
    /// while ANE FFN is on): listed, not to be picked.
    static func isWithheld(_ o: TuningOption, _ value: String,
                           _ t: Tuning) -> Bool {
        o.withheld.contains { t.flags[$0.key] == true
                              && $0.value.contains(value) }
    }

    /// The note beside a choice: its value's -- or, while it sets
    /// others (the int8 tier the ANE's int8), that it does.
    static func choiceNote(_ o: TuningOption, _ info: TuningInfo) -> String {
        if info.setBy.values.contains(o.key) {
            return String(localized: "The Neural Engine's int8 runs it too")
        }
        return choiceNote(o.key, info.tuning.choices[o.key] ?? "")
    }

    /// What an option goes by: an extension's own text, else Valtz's.
    /// Valtz's own words where it knows the key -- an option a built-in
    /// model declares in the catalog too (an upscaler's colour fix) --
    /// else the declaring manifest's.
    static func title(_ o: TuningOption) -> String {
        let own = label(o.key)
        return own != o.key ? own : (o.declaredLabel ?? own)
    }

    static func label(_ key: String) -> String {
        switch own(key) {
        case "steps": String(localized: "Steps")
        case "hyperflow": String(localized: "HyperFlow")
        case "pika_flex": String(localized: "Pika H3 Flex")
        case "taomate": String(localized: "TaoMate")
        case "taomate_lora": String(localized: "As a LoRA only")
        case "vdn": String(localized: "VDN branch")
        case "vdn_range": String(localized: "VDN softmax range")
        case "sol_attn": String(localized: "Sol-Attn")
        case "sol_threshold": String(localized: "Sol flavour")
        case "sol_tau": String(localized: "Sol threshold")
        case "sage_attn": String(localized: "SageAttention")
        case "sage_pv8": String(localized: "int8 P·V")
        case "sage_pv8_all_rows": String(localized: "Prompt and sound rows too")
        case "i8_gemm": String(localized: "int8 GEMM")
        case "i8_gemm_tier": String(localized: "int8 tier")
        case "vae_i8_gemm": String(localized: "VAE int8")
        case "vae_i8_gemm_tier": String(localized: "VAE int8 tier")
        case "ane_ffn": String(localized: "ANE FFN")
        case "ane_qkv": String(localized: "ANE QKV")
        case "ane_int8": String(localized: "ANE int8")
        case "ane_int8_tier": String(localized: "ANE int8 tier")
        case "motion_cache": String(localized: "MotionCache")
        case "video_shift": String(localized: "Video shift")
        case "audio_shift": String(localized: "Audio shift")
        case "shift": String(localized: "Shift")
        case "color_fix": String(localized: "Colour fix")
        case "guidance_scale": String(localized: "Guidance")
        case "fast_decoder": String(localized: "Fast decoder")
        case "refine_start": String(localized: "Refine")
        case "tiles": String(localized: "Tiles")
        case "tile_overlap": String(localized: "Tile overlap")
        case "flow_scale": String(localized: "Flow scale")
        case "scene_threshold": String(localized: "Scene cut")
        default: key
        }
    }

    /// The short note beside an option that can be changed -- a
    /// schedule's saying it takes one.
    static func note(_ o: TuningOption) -> String {
        // VOSR's colour fix is a choice (FlashVSR's a switch).
        if o.key == "color_fix", o.type == "choice" {
            return String(localized: "The restored picture's colour, as the picture it restored")
        }
        guard o.schedule else { return note(o.key) }
        switch own(o.key) {
        case "sol_tau":
            return String(localized: "Higher keeps fewer blocks; a list sets each step's")
        case "vdn_range":
            return String(localized: "Chunks either side kept exact; a list sets each step's")
        default:
            return note(o.key)
        }
    }

    /// ...and its help.
    static func help(_ o: TuningOption) -> String {
        if o.key == "color_fix", o.type == "choice" {
            return String(localized: "VOSR 2.0's release corrects what it restored against the picture it restored. Wavelet, its default, keeps the restored detail and takes the low frequencies — a soft blur of the picture — from the original, so colours and tones stay as they were. AdaIN moves each colour channel's mean and spread to the original's. None hands back the restorer's own colours, which can drift.")
        }
        guard o.schedule else { return help(o.key) }
        switch own(o.key) {
        case "sol_tau":
            return help(o.key) + " " + String(localized: "A list sets it step by step: 1.0, 1.5, 1.75 runs the first denoising step at 1.0, the second at 1.5, the third and any after at 1.75 (the last value is kept). Separate the values with commas; write decimals with a point. Later steps carry less noise, so they can keep fewer blocks for the same error.")
        case "vdn_range":
            return help(o.key) + " " + String(localized: "A list sets it step by step: 2, 1, 1 runs the first denoising step with 2 chunks either side, the second and any after with 1 (the last value is kept). Separate whole numbers with commas. The first steps decide the clip's layout and motion; later ones carry less noise and can hand more to the linear half.")
        default:
            return help(o.key)
        }
    }

    static func note(_ key: String) -> String {
        switch own(key) {
        case "steps": String(localized: "Denoising steps")
        case "hyperflow": String(localized: "8-step flow-map adapter; each step ~9% slower")
        case "pika_flex": String(localized: "Pika Labs' 6-step adapter, on its own schedule")
        case "taomate": String(localized: "3-step streaming method; its cache on disk")
        case "taomate_lora": String(localized: "Its adapter for 3 steps; no streaming, no cache")
        case "vdn": String(localized: "Linear attention for long, large clips")
        case "vdn_range": String(localized: "Chunks either side kept exact; the rest linear")
        case "sol_attn": String(localized: "Attends only the key blocks that matter")
        case "sol_tau": String(localized: "Higher keeps fewer blocks: faster, less exact")
        case "sage_attn": String(localized: "int8 QK in attention, about 1.2× (M5)")
        case "sage_pv8": String(localized: "Its P·V in int8 too: attention about 1.18×")
        case "sage_pv8_all_rows": String(localized: "int8 P·V on the prompt's and soundtrack's rows as well")
        case "i8_gemm": String(localized: "int8 block matrix products, about 2×")
        case "vae_i8_gemm": String(localized: "int8 convolutions in the VAE, its decode 1.15-1.26×")
        case "ane_ffn": String(localized: "Feed-forward on the Neural Engine, beside the GPU")
        case "ane_qkv": String(localized: "q|k|v projection on the Neural Engine too")
        case "ane_int8": String(localized: "The Neural Engine's modules in int8: faster, lossy")
        case "motion_cache": String(localized: "Reuses steps it can predict (16+ steps)")
        case "video_shift": String(localized: "Sigma shift of the video schedule")
        case "audio_shift": String(localized: "Sigma shift of the audio schedule")
        case "shift": String(localized: "Time shift of the noise schedule")
        case "tiles": String(localized: "Restored in tiles at the size it was trained at, or at once")
        case "tile_overlap": String(localized: "Pixels neighbouring tiles share, blended")
        case "color_fix": String(localized: "Each frame's colour kept to its source's")
        case "scene_threshold": String(localized: "Frames less alike than this are a cut: not blended")
        case "guidance_scale": String(localized: "How closely it follows the prompt (CFG)")
        case "fast_decoder": String(localized: "The release's own decoder: about 3.6× faster, lossy")
        default: ""
        }
    }

    static func help(_ key: String) -> String {
        switch own(key) {
        case "hyperflow": String(localized: "Video Rebirth's flow-map adapter brings its own 8-step grid. It takes a LoRA slot, in place of the Turbo LoRA; keep the shifts at 12 and 3, which it was trained at.")
        case "pika_flex": String(localized: "Pika Labs' six-step post-training of MiniMax H3, an adapter for each partition (FL2VA's and Ref2VA's). Its manifest states how it must be sampled: its own grid of six steps at shifts 12 and 3, the clip's estimate put back at the next step's noise with fresh noise each step, which a plain six-step run is not. The steps and shifts are its own; it takes a LoRA slot in place of the Turbo LoRA, and HyperFlow, TaoMate and MotionCache are off with it. A clip that opens or ends on a picture shows that picture to the prompt encoder too, as Pika's release does. Made for 480p and 768p, 5 to 15 seconds; no Favor preset turns it on.")
        case "taomate": String(localized: "TaoMate-H3's streaming method, not a LoRA: the base model writes the soundtrack, then its adapter writes the video in chunks, 3 steps each, against a cache of the chunks before. It takes a LoRA slot in place of the Turbo LoRA. Words to a clip only, made in whole 5-second requests. That cache is kept on disk (in 8 bits with int8 GEMM) and read back a block at a time. An alternative to the Turbo LoRA: no Favor preset turns it on.")
        case "taomate_lora": String(localized: "TaoMate-H3's adapter run as an ordinary LoRA: 3 steps over the whole clip, at the shifts TaoMate's own schedule uses (video 12, audio 3). No soundtrack pass by the base model, no chunks and no cache, so no disk traffic, and a clip may open on a picture. Not the method the adapter was trained for: judge its results by eye.")
        case "vdn": String(localized: "VideoDeltaNet: a windowed softmax over nearby frames plus a linear recurrence over the rest, a second checkpoint beside the model. Its advantage grows with the clip's length and size; it replaces Sol-Attn.")
        case "vdn_range": String(localized: "How far VDN's softmax half reaches: the query's chunk of 5 latent frames (about 0.7 s of video) and this many chunks either side; the linear half carries every frame beyond. Wider is more of the original attention, more exact and slower; a window over the whole clip turns the linear half off. Narrower hands more to the linear half. The branch was trained at 1, so another value is outside its training: judge the results by eye.")
        case "sol_attn": String(localized: "Training-free sparse attention: key blocks a cheap proxy scores low are folded in as their centroid. Saves about a quarter to a third of the denoise on a long clip.")
        case "sol_tau": String(localized: "In standard deviations of the routing score's spread. 1.0 is the published setting.")
        case "sol_threshold": String(localized: "How Sol-Attn estimates the spread of a block's routing scores, which its threshold counts in. Diagonal reads the key blocks' variance channel by channel, as Sol has run in Valtz until now. Exact reads their full covariance, as the published Sol-Attn does (Open-H3-Regenerate-2K's own refine runs it): about 5-10% less error at the same share of blocks kept. The two keep different shares at one threshold, so a threshold chosen for one does not carry over to the other.")
        case "sage_attn": String(localized: "SageAttention computes QK in int8 with exact key smoothing. Needs the M5's matrix cores.")
        case "sage_pv8": String(localized: "Runs attention's second product, P·V, in int8 as well: the product is kept in 32-bit integers across the key blocks, the probabilities on an 8-bit grid. For a clip, the first quarter of the steps keep SageAttention alone; a picture runs it on every step. On an M5 Ultra, a 1344×768 clip of 362 frames denoised about 8% sooner than with SageAttention alone (its attention 1.18×). Needs SageAttention; off in every Favor preset.")
        case "sage_pv8_all_rows": String(localized: "With int8 P·V on, a clip's prompt and soundtrack rows, under 1% of its rows, keep SageAttention alone unless this is on.")
        case "i8_gemm": String(localized: "Dynamic int8 for the block's large matrix products, at int8 quality. Needs the M5's matrix cores.")
        case "i8_gemm_tier": String(localized: "Which int8 matrix product runs with int8 GEMM on. Accurate quantizes the weights and the activations in groups of 64 at every call. Fast keeps the block's weights as int8 in groups of 512, each group turned by a Hadamard rotation on both sides so that one large channel does not set its scale. Per Token gives each weight row and each token one scale across the whole product, after a rotation in blocks of 256: the fastest (Qwen-Image 2.1 on an M5: 1.4-1.7× Accurate). Per Token, Unrotated skips the rotation: a little faster again, its pictures further from the bf16 model's. Faster down the list, less exact. A full-precision model's blocks take it only while kept in memory: those it streams, on a smaller Mac, stay Accurate (an 8-bit compressed copy's take it either way). With ANE FFN on, Fast is not offered, the Neural Engine having no int8 of its groups, and Per Token or Per Token, Unrotated sets the Neural Engine's int8 to the same tier.")
        case "vae_i8_gemm": String(localized: "Runs the VAE's 3×3 convolutions and its attention's matrix products in int8 on the M5's matrix cores: the decode that makes the picture, and the encode of an edit's pictures. Each channel of a convolution's input gets a scale of its own. On an M5 Pro, a 1024×1024 picture decodes about 0.15-0.26 s sooner, 50-60 dB from the full-precision decode. Off in every Favor preset: the VAE is the last step before the pixels, with nothing after it to absorb an error.")
        case "vae_i8_gemm_tier": String(localized: "Which of the VAE's convolutions run in int8 with VAE int8 on. Accurate takes those inside its residual blocks, whose inputs are normalized: about 1.15× the decode. Fast takes every 3×3 convolution, the upsampling ones too: about 1.26×, a little further from the full-precision picture.")
        case "ane_ffn": String(localized: "Runs each block's feed-forward on the Apple Neural Engine at the same time as the GPU, its rows split between the two (fp16). Most worthwhile where the GPU has no int8 path (M4); the plan decides whether its module fits.")
        case "ane_qkv": String(localized: "Adds the block's q|k|v projection to the Neural Engine as a second module. Needs ANE FFN: the two are granted together, and the pair costs more memory than the feed-forward alone.")
        case "ane_int8": String(localized: "Runs the Neural Engine's modules (the feed-forward, and q|k|v with ANE QKV) in int8, weights and activations, instead of fp16: the arithmetic of the GPU's per-token int8 tiers on the Neural Engine's rows. On an M4 Pro, MiniMax H3's blocks ran 1.36× the GPU alone with the feed-forward in int8 against 1.26× in fp16, and 1.65× against 1.41× with q|k|v too; each lands as far from the exact result as the GPU tier of its name. While int8 GEMM runs Per Token or Per Token, Unrotated, this is on at that tier. Needs ANE FFN and macOS 26; a module that cannot run int8 is built in fp16. Off in every Favor preset.")
        case "ane_int8_tier": String(localized: "Which int8 the Neural Engine runs with ANE int8 on. Per Token turns each weight row and each projection's input by a Hadamard rotation in blocks of 256, then gives each row and each token one scale, so that one large channel does not set it. Per Token, Unrotated skips the rotation: about the same speed, and in a MiniMax H3 clip about 1.7× the error. While int8 GEMM runs one of these tiers, this is the same one.")
        case "motion_cache": String(localized: "Before each step, predicts how far the model's answer moved since the last one that ran; when little, reuses it. Little to gain on short schedules.")
        case "flow_scale": String(localized: "The resolution RIFE reads the motion at, against the frame's: the release's --scale. Auto follows its advice: the frame's own, or half past 2560 pixels (4K). Lower is faster and follows large, fast motion better; higher keeps fine motion. Its makers suggest half for 4K footage.")
        case "scene_threshold": String(localized: "Two frames whose likeness -- their structural similarity at 32 × 32 -- is under this are a scene cut: the frames between them repeat the first instead of morphing one shot into the next. 0.2 is the release's; 0 never cuts.")
        case "tiles": String(localized: "VOSR 2.0 was distilled at 512 pixels. Past that, the picture is restored in tiles of 512, each on the grid its weights know, blended where they overlap — as its release does. One Pass restores the whole picture at once: faster, but on a grid the model never saw, which can weave a periodic pattern, a checkerboard on faces.")
        case "tile_overlap": String(localized: "How many pixels neighbouring tiles share, blended into each other: more hides the seams better, at more work. At least an eighth of a tile is used. One Pass has no tiles.")
        case "color_fix": String(localized: "Corrects each restored frame's colour toward its source frame, so the upscaled layer keeps the colours of the clip it replaces. FlashVSR 1.1 moves each channel's mean and spread to the source's (AdaIN); FlashVSR+ takes the source's low frequencies and keeps its own detail (a wavelet fix).")
        case "guidance_scale": String(localized: "Classifier-free guidance: each step runs the model twice, with the prompt and with an empty one, and moves the picture away from the empty one's by this much. MOSS-Image's own setting is 3. Higher follows the words more literally, with harder colours; 1 runs one forward a step, about twice as fast, the prompt followed less.")
        case "refine_start": String(localized: "How much of the draft the refine keeps. It puts the enlarged draft back to a level of noise and denoises it again in three steps. Keep the Draft starts at 0.7: in a test at 1920×1088, a woman's face and the words on a sign stayed the draft's, their detail sharper. NVIDIA's Recipe, Open-H3-Regenerate-2K as released, starts at 0.909, keeping about 9% of the draft: it redraws more, and in the same test it gave the woman another face and garbled the sign. The refine follows the prompt's words only weakly either way, so they do not bring back what it redraws.")
        case "fast_decoder": String(localized: "Decodes the regenerated clip with the fast decoder Open-H3-Regenerate-2K ships (NVIDIA's StreamRender-H3): 24 of MiniMax H3's decoder layers distilled, over the whole clip at once. About 3.6× faster on an M4 Pro, its frames about 40 dB from the released decoder's. Its working memory grows with the clip: about 12 GB for five seconds at 2K. Needs its download, in Settings › Capabilities › MiniMax H3.")
        default: ""
        }
    }

    /// A refine option's own key (the refiner's, offered beside the
    /// model's after "regenerate_": core models::kRegeneratePrefix): its
    /// words are the option's.
    static func own(_ key: String) -> String {
        key.hasPrefix("regenerate_")
            ? String(key.dropFirst("regenerate_".count)) : key
    }

    /// What a choice's value goes by, and the note beside it chosen.
    static func choiceLabel(_ key: String, _ value: String) -> String {
        switch (own(key), value) {
        case ("i8_gemm_tier", "accurate"): String(localized: "Accurate")
        case ("i8_gemm_tier", "fast"): String(localized: "Fast")
        case ("i8_gemm_tier", "token"): String(localized: "Per Token")
        case ("i8_gemm_tier", "token_plain"):
            String(localized: "Per Token, Unrotated")
        case ("vae_i8_gemm_tier", "accurate"): String(localized: "Accurate")
        case ("vae_i8_gemm_tier", "fast"): String(localized: "Fast")
        case ("ane_int8_tier", "token"): String(localized: "Per Token")
        case ("ane_int8_tier", "token_plain"):
            String(localized: "Per Token, Unrotated")
        case ("sol_threshold", "diag"): String(localized: "Diagonal")
        case ("sol_threshold", "exact"): String(localized: "Exact")
        case ("refine_start", "draft"): String(localized: "Keep the Draft")
        case ("refine_start", "release"): String(localized: "NVIDIA's Recipe")
        case ("color_fix", "wavelet"): String(localized: "Wavelet")
        case ("color_fix", "adain"): String(localized: "AdaIN")
        case ("color_fix", "none"): String(localized: "None")
        case ("tiles", "trained"): String(localized: "At Its Trained Size")
        case ("tiles", "one_pass"): String(localized: "One Pass")
        case ("flow_scale", "auto"): String(localized: "Auto")
        case ("flow_scale", "quarter"): String(localized: "Quarter")
        case ("flow_scale", "half"): String(localized: "Half")
        case ("flow_scale", "full"): String(localized: "Full")
        case ("flow_scale", "double"): String(localized: "Double")
        case ("flow_scale", "quadruple"): String(localized: "Quadruple")
        default: value
        }
    }

    static func choiceNote(_ key: String, _ value: String) -> String {
        switch (own(key), value) {
        case ("i8_gemm_tier", "accurate"):
            String(localized: "Groups of 64 on both sides: the closest to bf16")
        case ("i8_gemm_tier", "fast"):
            String(localized: "Rotated int8 weights in groups of 512: faster on an M5")
        case ("i8_gemm_tier", "token"):
            String(localized: "A scale a row and a token, rotated: the fastest")
        case ("i8_gemm_tier", "token_plain"):
            String(localized: "Not rotated: a little faster, less exact")
        case ("vae_i8_gemm_tier", "accurate"):
            String(localized: "The residual blocks' convolutions")
        case ("vae_i8_gemm_tier", "fast"):
            String(localized: "Every 3×3 convolution: faster, less exact")
        case ("ane_int8_tier", "token"):
            String(localized: "Rotated, a scale a row and a token: the GPU's Per Token")
        case ("ane_int8_tier", "token_plain"):
            String(localized: "Not rotated: the same speed, less exact")
        case ("sol_threshold", "diag"):
            String(localized: "The blocks' variance, channel by channel")
        case ("sol_threshold", "exact"):
            String(localized: "Their full covariance, as published: less error")
        case ("refine_start", "draft"):
            String(localized: "From noise 0.7: the draft's faces and lettering kept")
        case ("refine_start", "release"):
            String(localized: "From noise 0.909, as released: faces and lettering redrawn")
        case ("color_fix", "wavelet"):
            String(localized: "The original's low frequencies, the restored detail: as released")
        case ("color_fix", "adain"):
            String(localized: "Each channel's mean and spread moved to the original's")
        case ("color_fix", "none"):
            String(localized: "The restorer's own colours, uncorrected")
        case ("tiles", "trained"):
            String(localized: "Tiles of 512, blended where they meet: as released")
        case ("flow_scale", "auto"):
            String(localized: "The frame's own, half past 4K: the release's advice")
        case ("flow_scale", "quarter"), ("flow_scale", "half"):
            String(localized: "Coarser: faster, large motion followed better")
        case ("flow_scale", "full"):
            String(localized: "The frame's own resolution")
        case ("flow_scale", "double"), ("flow_scale", "quadruple"):
            String(localized: "Finer: slower, fine motion kept")
        case ("tiles", "one_pass"):
            String(localized: "Faster, but may weave a checkerboard through faces")
        default: ""
        }
    }

    static func why(_ code: String) -> String {
        switch code {
        case "not-installed": String(localized: "Not installed — download it in Models")
        case "continues-through": String(localized: "Not for a clip continued through its end: TaoMate streams from the prompt alone")
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
    // A clip past 768P regenerated (DESIGN §4q): the refine's own
    // options, the model's refiner's.
    case regenerate
    var id: Self { self }

    var title: String {
        switch self {
        case .steps: String(localized: "Steps")
        case .weights: String(localized: "LoRAs and checkpoints")
        case .attention: String(localized: "Attention")
        case .compute: String(localized: "Compute")
        case .schedule: String(localized: "Schedule")
        case .regenerate: String(localized: "High resolution pass")
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
        if key.hasPrefix("regenerate_") { return .regenerate }
        return switch key {
        case "steps", "hyperflow", "pika_flex", "taomate", "taomate_lora":
            .steps
        case "loras", "dits", "vaes": .weights
        case "vdn", "vdn_range", "sol_attn", "sol_threshold", "sol_tau",
             "sage_attn", "sage_pv8", "sage_pv8_all_rows":
            .attention
        case "i8_gemm", "i8_gemm_tier", "vae_i8_gemm", "vae_i8_gemm_tier",
             "ane_ffn", "ane_qkv", "ane_int8", "ane_int8_tier",
             "motion_cache": .compute
        default: .schedule
        }
    }
}

/// A number option: typed (taken on Return or when left, held to its
/// range by the core) or stepped. One that takes a SCHEDULE
/// (TuningOption.schedule) takes a list too, comma-separated -- a value
/// a step -- stepped all together.
struct TuningNumberField: View {
    let value: Double
    let option: TuningOption
    /// The schedule it holds ("1,1.5,1.75"), nil: the value.
    var schedule: String? = nil
    let commit: (Double) -> Void
    var commitSchedule: ((String) -> Void)? = nil
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
                .frame(width: option.schedule ? 128 : 56)
                .focused($focused)
                .onSubmit(take)
                // ↑ / ↓ step it, as the arrows beside it do.
                .onKeyPress(.upArrow) { nudge(1); return .handled }
                .onKeyPress(.downArrow) { nudge(-1); return .handled }
            Stepper(TuningPanel.label(option.key),
                    onIncrement: { nudge(1) }, onDecrement: { nudge(-1) })
                .labelsHidden()
        }
        .onAppear { show(current) }
        // What the core settled shows, focused or not, unless a value is
        // being typed in it.
        .onChange(of: value) { _, _ in
            if text == shown { show(current) }
        }
        .onChange(of: schedule) { _, _ in
            if text == shown { show(current) }
        }
        .onChange(of: focused) { _, f in
            if !f { take() }
        }
    }

    /// One step from what the field holds -- typed and not yet taken, or
    /// the value -- shown at once and committed, focused or not. A
    /// schedule's every value moves by it.
    private func nudge(_ dir: Double) {
        let base = typed ?? held
        let next = base.map {
            Swift.min(option.max,
                      Swift.max(option.min, $0 + dir * option.step))
        }
        guard !next.isEmpty else { return }
        show(written(next))
        if next != held { send(next) }
    }

    private func show(_ s: String) {
        text = s
        shown = s
    }

    /// What it holds now: the schedule's values, else the value.
    private var held: [Double] {
        schedule.flatMap(Self.values) ?? [value]
    }

    private var current: String { written(held) }

    /// What is typed, as values: a schedule's comma-separated list
    /// (decimals with a point), else one number (a decimal comma read
    /// as a point). nil: not numbers.
    private var typed: [Double]? {
        let t = text.trimmingCharacters(in: .whitespaces)
        if option.schedule { return Self.values(t) }
        return Double(t.replacingOccurrences(of: ",", with: ".")).map { [$0] }
    }

    static func values(_ text: String) -> [Double]? {
        let vs = text.split(separator: ",", omittingEmptySubsequences: false)
            .map { Double($0.trimmingCharacters(in: .whitespaces)) }
        guard !vs.isEmpty, vs.allSatisfy({ $0 != nil }) else { return nil }
        return vs.compactMap { $0 }
    }

    private func take() {
        if let vs = typed, !vs.isEmpty, vs != held { send(vs) }
        show(current)
    }

    /// One value as the number, several as the schedule.
    private func send(_ vs: [Double]) {
        if vs.count == 1 || commitSchedule == nil {
            commit(vs[0])
        } else {
            commitSchedule?(vs.map { String(format: "%g", $0) }
                .joined(separator: ","))
        }
    }

    private func written(_ vs: [Double]) -> String {
        vs.map(display).joined(separator: ", ")
    }

    /// Whole steps; a shift or threshold to its step's precision, finer
    /// where the value is (a threshold of 1.75).
    private func display(_ v: Double) -> String {
        if option.type == "int" { return String(Int(v.rounded())) }
        var digits = option.step < 0.1 ? 2 : 1
        while digits < 3 {
            let scaled = v * pow(10, Double(digits))
            if abs(scaled - scaled.rounded()) < 1e-6 { break }
            digits += 1
        }
        return String(format: "%.\(digits)f", v)
    }
}

/// Weights dropped anywhere on the panel: each filed by what it is -- a
/// LoRA, a DiT or a VAE checkpoint.
private struct WeightsDrop: ViewModifier {
    let model: any TuningHost
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

/// Its one view at the width it takes unconstrained -- `minWidth` at
/// least -- and as tall as it lays out AT that width. `.fixedSize()` asks
/// the height with no width proposed, every wrapping note a single line
/// long, then lays the view out at its width, where the notes wrap: the
/// rows overran the height measured, and the scroll view holding them
/// cut off the last (the regenerate pass's Fast decoder).
struct AtIdealWidth: Layout {
    var minWidth: CGFloat = 0

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews,
                      cache: inout ()) -> CGSize {
        guard let v = subviews.first else { return .zero }
        let w = max(minWidth, v.sizeThatFits(.unspecified).width)
        let h = v.sizeThatFits(ProposedViewSize(width: w, height: nil))
        return CGSize(width: w, height: h.height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize,
                       subviews: Subviews, cache: inout ()) {
        subviews.first?.place(
            at: bounds.origin, anchor: .topLeading,
            proposal: ProposedViewSize(width: bounds.width, height: nil))
    }
}
