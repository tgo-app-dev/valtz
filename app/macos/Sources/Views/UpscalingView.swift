import SwiftUI

/// Settings › Upscaling (DESIGN §4f): Render Upscaled has no generation
/// card, so the upscalers' arithmetic is set here -- each one's int8
/// GEMMs and SageAttention, and VOSR's Sol-Attn with its threshold --
/// kept by the core for every later upscale (valtzctl's `upscaler` sets
/// the same). The defaults are Med's: int8 GEMMs wherever the GPU has
/// matrix cores, the rest off. An option this Mac cannot have is shown,
/// off, with why; one an upscaler does not take is not listed.
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
            ForEach(model.upscalers) { u in
                Section {
                    ForEach(u.options) { o in row(u, o) }
                    if u.state != "installed" {
                        LabeledContent {
                            Button("Open Capabilities") {
                                model.openCapabilities(family: "upscalers")
                            }
                        } label: {
                            Text("\(u.name) is not downloaded yet: these apply once it is.")
                                .foregroundStyle(.secondary)
                        }
                    }
                } header: {
                    HStack(alignment: .firstTextBaseline) {
                        Text(verbatim: u.name)
                        Text(u.kind == "video" ? "Clips" : "Pictures")
                            .foregroundStyle(.secondary)
                        Spacer()
                        Button("Restore Defaults") { model.resetUpscaler(u.id) }
                            .controlSize(.small)
                            .disabled(!u.custom)
                    }
                } footer: {
                    Text(u.kind == "video"
                         ? "Restores a clip a group of frames at a time, in one step each. Its attention is already sparse by its own weights, so it takes no Sol-Attn."
                         : "Restores a picture in one step, in tiles past the size it was trained at; DINOv2 reads it first.")
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

    /// One option: a switch, or a number (Sol's threshold) -- its name
    /// and what it does, or why it cannot be changed.
    @ViewBuilder
    private func row(_ u: UpscalerSettings, _ o: TuningOption) -> some View {
        let t = u.info.tuning
        let block = blocker(u, o)
        let label = VStack(alignment: .leading, spacing: 2) {
            Text(verbatim: TuningPanel.title(o))
            Text(block ?? TuningPanel.note(o.key))
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
            } else {
                LabeledContent {
                    TuningNumberField(value: t.numbers[o.key] ?? 0,
                                      option: o) {
                        model.setUpscaleOption(u.id, o.key, $0)
                    }
                } label: {
                    label
                }
            }
        }
        .disabled(block != nil)
        .help(TuningPanel.help(o.key))
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
