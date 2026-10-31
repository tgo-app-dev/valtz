import SwiftUI

/// Settings › Upscaling (DESIGN §4f): Render Upscaled has no generation
/// card, so the upscalers' arithmetic is set here -- each one's int8
/// GEMMs and SageAttention, and VOSR's Sol-Attn with its threshold --
/// kept by the core for every later upscale (valtzctl's `upscaler` sets
/// the same). The defaults are Med's: int8 GEMMs wherever the GPU has
/// matrix cores, the rest off. An option this Mac cannot have is shown,
/// off, with why; one an upscaler does not take is not listed. Where a
/// kind has more than one upscaler -- clips: FlashVSR+ Turbo and Base,
/// FlashVSR 1.1 -- which one it runs is picked here too (Auto: the best
/// downloaded).
struct UpscalingView: View {
    @Bindable var model: AppModel

    var body: some View {
        Form {
            Section { SettingsHeader(page: .upscaling) }
            if model.upscalers.isEmpty {
                Section {
                    Text("No upscaler is known to this build.")
                        .foregroundStyle(.secondary)
                }
            }
            ForEach(model.upscalerKinds.filter { k in
                model.upscalers.filter { $0.kind == k.kind }.count > 1
            }, id: \.kind) { k in
                pick(k)
            }
            ForEach(model.upscalers) { u in
                Section {
                    ForEach(u.options) { o in row(u, o) }
                    if u.state != "installed" {
                        LabeledContent {
                            Button("Open Capabilities") {
                                model.openCapabilities(family: u.family)
                            }
                        } label: {
                            Text("\(u.name) is not downloaded yet: these apply once it is.")
                                .foregroundStyle(.secondary)
                        }
                    } else if !u.ready {
                        // A refiner without its latent upscaler or its
                        // model beside it.
                        LabeledContent {
                            Button("Open Capabilities") {
                                model.openCapabilities(family: u.family)
                            }
                        } label: {
                            Text("\(u.name) runs with its latent upscaler and MiniMax H3 FL2VA beside it: get them in Settings › Capabilities.")
                                .foregroundStyle(.secondary)
                        }
                    }
                } header: {
                    HStack(alignment: .firstTextBaseline) {
                        Text(verbatim: u.name)
                        Text(u.kind == "video" ? "Clips" : "Pictures")
                            .foregroundStyle(.secondary)
                        if u.runs {
                            Text("In Use")
                                .font(.caption.weight(.medium))
                                .foregroundStyle(.secondary)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 1)
                                .background(.quaternary, in: Capsule())
                                .help("Render Upscaled runs this one")
                        }
                        Spacer()
                        Button("Restore Defaults") { model.resetUpscaler(u.id) }
                            .controlSize(.small)
                            .disabled(!u.custom)
                    }
                    .settingsMark(u.name, u.kind == "video"
                                  ? String(localized: "Clips")
                                  : String(localized: "Pictures"))
                } footer: {
                    Group {
                        if u.refines {
                            Text("Regenerates a clip at the size it is shown, as MiniMax H3 makes clips past 768 on their short side: the clip read at 768 at most, its latent enlarged, then three steps of MiniMax H3 FL2VA under NVIDIA's merged LoRA, against the words that made it (else its summary). Detail is made anew, not restored, and a 2K clip of five seconds takes hours on most Macs, so it runs only when chosen above. These also set the refine of a clip made past 768P, unless its Favor is Custom: Custom has them for the clip.")
                        } else if u.kind == "video" {
                            Text("Restores a clip a group of frames at a time, in one step each. Its attention is already sparse by its own weights, so it takes no Sol-Attn.")
                        } else {
                            Text("Restores a picture in one step, in tiles past the size it was trained at; DINOv2 reads it first.")
                        }
                    }
                    .foregroundStyle(.secondary)
                }
            }
            Section {
            } footer: {
                Text("The defaults are Med's: int8 GEMMs on an M5 or later, the rest off. An upscale already queued keeps what it was asked with; its recipe records them.")
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onAppear { model.refreshMachine() }
    }

    /// Which upscaler a kind runs: Auto (naming its pick) or one by name,
    /// one not downloaded said so -- and, picked but not here, what runs
    /// meanwhile.
    @ViewBuilder
    private func pick(_ k: UpscalerKind) -> some View {
        let mine = model.upscalers.filter { $0.kind == k.kind }
        let name = { (id: String) in
            mine.first { $0.id == id }?.name ?? id
        }
        Section {
            Picker(selection: Binding(
                get: { k.chosen },
                set: { model.useUpscaler(kind: k.kind, $0) })) {
                if k.auto.isEmpty {
                    Text("Auto").tag("")
                } else {
                    Text("Auto (\(name(k.auto)))").tag("")
                }
                Divider()
                ForEach(mine) { u in
                    if u.ready {
                        Text(verbatim: u.name).tag(u.id)
                    } else {
                        Text("\(u.name) (not downloaded)").tag(u.id)
                    }
                }
            } label: {
                Text(k.kind == "video" ? "Restore Clips With"
                                       : "Restore Pictures With")
            }
            .settingsMark(k.kind == "video"
                          ? String(localized: "Restore Clips With")
                          : String(localized: "Restore Pictures With"),
                          String(localized: "Render Upscaled"),
                          String(localized: "Upscale"))
            if k.runs.isEmpty {
                LabeledContent {
                    Button("Open Capabilities") {
                        model.openCapabilities(family: "upscalers")
                    }
                } label: {
                    Text(k.kind == "video"
                         ? "No upscaler for clips is downloaded yet."
                         : "No upscaler for pictures is downloaded yet.")
                        .foregroundStyle(.secondary)
                }
            } else if !k.chosen.isEmpty, k.chosen != k.runs {
                LabeledContent {
                    Button("Open Capabilities") {
                        model.openCapabilities(family: "upscalers")
                    }
                } label: {
                    Text("\(name(k.chosen)) is not downloaded yet: \(name(k.runs)) runs until it is.")
                        .foregroundStyle(.secondary)
                }
            }
        } footer: {
            Text(k.kind == "video"
                 ? "Auto runs the best one downloaded: FlashVSR+ Turbo, then FlashVSR+ Base, then FlashVSR 1.1. Turbo is the faster, and its makers' pick for high resolutions; Base restores at a higher quality, more slowly and in more memory. Open-H3-Regenerate-2K is never Auto's: it runs when chosen here."
                 : "Auto runs the best one downloaded.")
                .foregroundStyle(.secondary)
        }
    }

    /// One option: a switch, a choice, or a number (Sol's threshold, a
    /// schedule of them for the refiner) -- its name
    /// and what it does, or why it cannot be changed.
    @ViewBuilder
    private func row(_ u: UpscalerSettings, _ o: TuningOption) -> some View {
        let t = u.info.tuning
        let block = blocker(u, o)
        let label = VStack(alignment: .leading, spacing: 2) {
            Text(verbatim: TuningPanel.title(o))
            Text(block ?? (o.type == "choice"
                           ? TuningPanel.choiceNote(
                               o.key, t.choices[o.key] ?? "")
                           : TuningPanel.note(o)))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        Group {
            if o.type == "bool" {
                Toggle(isOn: Binding(
                    get: { t.flags[o.key] ?? false },
                    set: { model.setUpscaleOption(u.id, o.key, $0) })) {
                    label
                }
            } else if o.type == "choice" {
                // The refiner's int8 tier: a pop-up, as Custom's.
                Picker(selection: Binding(
                    get: { t.choices[o.key] ?? o.choices.first ?? "" },
                    set: { model.setUpscaleOption(u.id, o.key, $0) })) {
                    ForEach(o.choices, id: \.self) { c in
                        Text(TuningPanel.choiceLabel(o.key, c)).tag(c)
                    }
                } label: {
                    label
                }
            } else {
                LabeledContent {
                    TuningNumberField(
                        value: t.numbers[o.key] ?? 0, option: o,
                        schedule: t.schedules[o.key],
                        commit: { model.setUpscaleOption(u.id, o.key, $0) },
                        commitSchedule: {
                            model.setUpscaleOption(u.id, o.key, $0)
                        })
                } label: {
                    label
                }
            }
        }
        .disabled(block != nil)
        .help(TuningPanel.help(o))
        .settingsMark(TuningPanel.title(o), scope: u.id)
    }

    /// Why `o` cannot be changed now: this Mac cannot have it, or it
    /// refines a switch that is off.
    private func blocker(_ u: UpscalerSettings, _ o: TuningOption) -> String? {
        if !o.available { return TuningPanel.why(o.why) }
        if let needs = o.needs, u.info.tuning.flags[needs] != true {
            let name = u.info.options.first { $0.key == needs }
                .map(TuningPanel.title) ?? TuningPanel.label(needs)
            return String(localized: "With \(name) on")
        }
        return nil
    }
}
