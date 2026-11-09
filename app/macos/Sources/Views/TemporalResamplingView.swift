import SwiftUI

/// Settings › Temporal Resampling (DESIGN §4u): Render Interpolated makes
/// a layer's clip at the rate its timeline shows it -- the frames between
/// its own made by RIFE (Practical-RIFE, run by vpipe on the GPU) -- and
/// has no generation card, so what it runs with is set here: which RIFE
/// release (Auto: the best downloaded, 4.25 first), the scale its motion
/// is read at, and how unlike two frames must be to be a cut. Kept by the
/// core for every later interpolation (valtzctl's `interpolator` sets
/// the same). The models come from Google Drive, where their makers
/// publish them: Settings › Capabilities › Frame Interpolation fetches
/// and checks them.
struct TemporalResamplingView: View {
    @Bindable var model: AppModel

    var body: some View {
        Form {
            Section { SettingsHeader(page: .resampling) }
            pick
            ForEach(model.interpolators) { u in
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
                    }
                } header: {
                    HStack(alignment: .firstTextBaseline) {
                        Text(verbatim: u.name)
                        if u.runs {
                            Text("In Use")
                                .font(.caption.weight(.medium))
                                .foregroundStyle(.secondary)
                                .padding(.horizontal, 6)
                                .padding(.vertical, 1)
                                .background(.quaternary, in: Capsule())
                                .help("Render Interpolated runs this one")
                        }
                        Spacer()
                        Button("Restore Defaults") {
                            model.resetInterpolator(u.id)
                        }
                        .controlSize(.small)
                        .disabled(!u.custom)
                    }
                    .settingsMark(u.name)
                }
            }
            Section {
            } footer: {
                Text("Where a clip is shown at more frames a second than it has -- slowed down, or on a timeline of a higher rate -- the timeline repeats its frames. Render Interpolated, in the Trim panel, makes the frames in between: the clip at the rate it is shown, its length and sound kept, the layer then showing it. Where a new frame falls on one of the clip's own, that frame is kept as it is.")
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onAppear { model.refreshMachine() }
    }

    /// Which RIFE runs: Auto (naming its pick) or one by name.
    @ViewBuilder
    private var pick: some View {
        let p = model.interpolatorPick
        let name = { (id: String) in
            model.interpolators.first { $0.id == id }?.name ?? id
        }
        Section {
            Picker(selection: Binding(
                get: { p.chosen },
                set: { model.useInterpolator($0) })) {
                if p.auto.isEmpty {
                    Text("Auto").tag("")
                } else {
                    Text("Auto (\(name(p.auto)))").tag("")
                }
                Divider()
                ForEach(model.interpolators) { u in
                    if u.ready {
                        Text(verbatim: u.name).tag(u.id)
                    } else {
                        Text("\(u.name) (not downloaded)").tag(u.id)
                    }
                }
            } label: {
                Text("Interpolate With")
            }
            .settingsMark(String(localized: "Interpolate With"),
                          String(localized: "Render Interpolated"),
                          String(localized: "Frame Interpolation"))
            if !p.engine {
                Text("This build's engine has no RIFE: its frame interpolation needs a newer vpipe.")
                    .foregroundStyle(.secondary)
            } else if p.runs.isEmpty {
                LabeledContent {
                    Button("Open Capabilities") {
                        model.openCapabilities(family: "rife")
                    }
                } label: {
                    Text("No RIFE model is downloaded yet.")
                        .foregroundStyle(.secondary)
                }
            } else if !p.chosen.isEmpty, p.chosen != p.runs {
                LabeledContent {
                    Button("Open Capabilities") {
                        model.openCapabilities(family: "rife")
                    }
                } label: {
                    Text("\(name(p.chosen)) is not downloaded yet: \(name(p.runs)) runs until it is.")
                        .foregroundStyle(.secondary)
                }
            }
        } footer: {
            Text("Auto runs the best one downloaded: 4.25, which Practical-RIFE recommends for most footage, then 4.26, 4.22 and the lighter ones. Each is a download of 20 to 40 MB.")
                .foregroundStyle(.secondary)
        }
    }

    /// One option: a choice (the flow scale) or a number (the scene cut).
    @ViewBuilder
    private func row(_ u: UpscalerSettings, _ o: TuningOption) -> some View {
        let t = u.info.tuning
        let label = VStack(alignment: .leading, spacing: 2) {
            Text(verbatim: TuningPanel.title(o))
            Text(o.type == "choice" ? TuningPanel.choiceNote(o, u.info)
                                    : TuningPanel.note(o))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        Group {
            if o.type == "choice" {
                Picker(selection: Binding(
                    get: { t.choices[o.key] ?? o.choices.first ?? "" },
                    set: { model.setInterpolateOption(u.id, o.key, $0) })) {
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
                        commit: { model.setInterpolateOption(u.id, o.key, $0) },
                        commitSchedule: {
                            model.setInterpolateOption(u.id, o.key, $0)
                        })
                } label: {
                    label
                }
            }
        }
        .help(TuningPanel.help(o))
        .settingsMark(TuningPanel.title(o), scope: u.id)
    }
}
