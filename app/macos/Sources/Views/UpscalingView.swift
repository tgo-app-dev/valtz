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
/// downloaded); and which REFINER regenerates a clip made past 768P
/// (High-Res Pass With: Open-H3-Regenerate-2K, or its IC refine).
private extension UpscalerKind {
    /// The upscalers it picks among: a refiner's, those that refine.
    func members(_ all: [UpscalerSettings]) -> [UpscalerSettings] {
        kind == "regenerate" ? all.filter(\.refines)
                             : all.filter { $0.kind == kind }
    }
}

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
                k.members(model.upscalers).count > 1
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
                        // Downloaded, without what it needs beside it --
                        // VOSR its DINOv2, a refiner its latent upscaler
                        // and model -- named as the core says (it said
                        // the refiner's of VOSR too).
                        LabeledContent {
                            Button("Open Capabilities") {
                                model.openCapabilities(family: u.family)
                            }
                        } label: {
                            Group {
                                if u.needs.isEmpty {
                                    Text("\(u.name) needs a newer engine than this build has.")
                                } else {
                                    Text("\(u.name) runs with \(u.needs.formatted(.list(type: .and))) beside it: get them in Settings › Capabilities.")
                                }
                            }
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
                        if u.inContext {
                            Text("Regenerates a clip at the size it is shown IN CONTEXT: the clip read at 768 at most and held clean beside it as a reference, its latent enlarged and regenerated in three steps of MiniMax H3 Ref2VA under HyperFlow, LMS Sharp and NVIDIA's IC LoRA, against the words that made it (else its summary). Faces, mouths, lettering and layout stay the clip's. Slower than Open-H3-Regenerate-2K: the clip's own rows ride every step. These also set its refine of a clip made past 768P, unless its Favor is Custom.")
                        } else if u.refines {
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
        let mine = k.members(model.upscalers)
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
                Text(k.kind == "regenerate" ? "High-Res Pass With"
                     : k.kind == "video" ? "Restore Clips With"
                     : "Restore Pictures With")
            }
            .settingsMark(k.kind == "regenerate"
                          ? String(localized: "High-Res Pass With")
                          : k.kind == "video"
                          ? String(localized: "Restore Clips With")
                          : String(localized: "Restore Pictures With"),
                          String(localized: "Render Upscaled"),
                          String(localized: "Upscale"))
            if k.runs.isEmpty {
                LabeledContent {
                    Button("Open Capabilities") {
                        model.openCapabilities(
                            family: k.kind == "regenerate" ? "minimax-h3"
                                                           : "upscalers")
                    }
                } label: {
                    Text(k.kind == "regenerate"
                         ? "No refiner is downloaded yet."
                         : k.kind == "video"
                         ? "No upscaler for clips is downloaded yet."
                         : "No upscaler for pictures is downloaded yet.")
                        .foregroundStyle(.secondary)
                }
            } else if !k.chosen.isEmpty, k.chosen != k.runs {
                LabeledContent {
                    Button("Open Capabilities") {
                        model.openCapabilities(
                            family: k.kind == "regenerate" ? "minimax-h3"
                                                           : "upscalers")
                    }
                } label: {
                    Text("\(name(k.chosen)) is not downloaded yet: \(name(k.runs)) runs until it is.")
                        .foregroundStyle(.secondary)
                }
            }
        } footer: {
            Text(k.kind == "regenerate"
                 ? "A clip made past 768 on its short side is drafted at 768 and regenerated at its size by one of these. Auto runs Open-H3-Regenerate-2K where it is here, else its IC refine. The IC refine holds the draft beside the clip as a reference -- faces, mouths and lettering stay the draft's -- and runs on Ref2VA, more slowly."
                 : k.kind == "video"
                 ? "Auto runs the best one downloaded: FlashVSR+ Turbo, then FlashVSR+ Base, then FlashVSR 1.1. Turbo is the faster, and its makers' pick for high resolutions; Base restores at a higher quality, more slowly and in more memory. Open-H3-Regenerate-2K and its IC refine are never Auto's: they run when chosen here."
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
        let block = TuningPanel.blocker(o, u.info)
        let label = VStack(alignment: .leading, spacing: 2) {
            Text(verbatim: TuningPanel.title(o))
            Text(block ?? (o.type == "choice"
                           ? TuningPanel.choiceNote(o, u.info)
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
                            .selectionDisabled(
                                TuningPanel.isWithheld(o, c, t))
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
}
