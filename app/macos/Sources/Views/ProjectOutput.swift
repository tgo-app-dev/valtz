import SwiftUI

/// The project's OUTPUT (DESIGN §6b; core project::OutputSettings): what
/// exporting the project writes -- its colour space, a timeline's frame
/// rate, its sound's channels and sample rate. An asset keeps its own; the
/// project's timeline runs at the frame rate, a clip at another
/// resampled to it.
struct OutputSettingsDTO: Decodable, Equatable, Sendable {
    var color = "rec709"
    var fps: [Int64] = [24, 1]
    var channels = 2
    var sampleRate = 48000

    var json: [String: Any] {
        ["color": color, "fps": fps, "channels": channels,
         "sample_rate": sampleRate]
    }
}

/// What a project is set up as, for the fields it has.
enum ProjectKind: Int, Sendable {
    case still, timeline, sound
}

/// The output's fields, as a project of `kind` has them: a still its
/// colour; a timeline its colour, frame rate and sound; sound alone its
/// sound. The setup form's, and Information › Project's.
struct OutputFields: View {
    let kind: ProjectKind
    @Binding var output: OutputSettingsDTO
    /// The frame rate is the timeline's own: set while it is empty.
    var rateLocked = false

    /// The colour spaces offered: a still's display spaces, a timeline's
    /// video ones (HDR among them).
    private var colors: [(id: String, label: LocalizedStringKey)] {
        switch kind {
        case .still:
            [("rec709", "Rec. 709"), ("srgb", "sRGB"),
             ("display-p3", "Display P3"), ("rec2020", "Rec. 2020")]
        default:
            [("rec709", "Rec. 709"), ("rec2020", "Rec. 2020"),
             ("rec2100-pq", "Rec. 2100 PQ (HDR)"),
             ("rec2100-hlg", "Rec. 2100 HLG (HDR)")]
        }
    }

    static let rates: [(label: String, fps: [Int64])] = [
        ("23.976", [24000, 1001]), ("24", [24, 1]), ("25", [25, 1]),
        ("29.97", [30000, 1001]), ("30", [30, 1]), ("50", [50, 1]),
        ("59.94", [60000, 1001]), ("60", [60, 1]),
    ]
    static let sampleRates = [44100, 48000, 96000]

    var body: some View {
        Grid(alignment: .leading, horizontalSpacing: 8, verticalSpacing: 6) {
            if kind != .sound {
                GridRow {
                    label("Color space")
                    Picker("Color space", selection: $output.color) {
                        ForEach(colors, id: \.id) { c in
                            Text(c.label).tag(c.id)
                        }
                        // One set before, not offered for this kind.
                        if !colors.contains(where: { $0.id == output.color }) {
                            Text(verbatim: output.color).tag(output.color)
                        }
                    }
                    .labelsHidden()
                    .fixedSize()
                }
            }
            if kind == .timeline {
                GridRow {
                    label("Frame rate")
                    Picker("Frame rate", selection: $output.fps) {
                        ForEach(Self.rates, id: \.label) { r in
                            Text(verbatim: r.label + " fps").tag(r.fps)
                        }
                        if !Self.rates.contains(where: { $0.fps == output.fps }),
                           output.fps.count == 2, output.fps[1] > 0 {
                            Text(verbatim: String(
                                format: "%g fps",
                                Double(output.fps[0]) / Double(output.fps[1])))
                                .tag(output.fps)
                        }
                    }
                    .labelsHidden()
                    .fixedSize()
                    .disabled(rateLocked)
                    .help(rateLocked
                          ? "The timeline's own rate: it changes only while the timeline is empty"
                          : "The project timeline's rate: a clip at another is resampled to it")
                }
            }
            if kind != .still {
                GridRow {
                    label("Channels")
                    Picker("Channels", selection: $output.channels) {
                        Text("Mono").tag(1)
                        Text("Stereo").tag(2)
                    }
                    .labelsHidden()
                    .fixedSize()
                }
                GridRow {
                    label("Sample rate")
                    Picker("Sample rate", selection: $output.sampleRate) {
                        ForEach(Self.sampleRates, id: \.self) { r in
                            Text(verbatim: String(format: "%g kHz",
                                                  Double(r) / 1000)).tag(r)
                        }
                    }
                    .labelsHidden()
                    .fixedSize()
                }
            }
        }
    }

    private func label(_ s: LocalizedStringKey) -> some View {
        Text(s)
            .foregroundStyle(.secondary)
            .gridColumnAlignment(.trailing)
    }
}

extension AppModel {
    /// The project's output, from the core.
    func reloadOutput() {
        guard let core, let projectId else { return }
        let r = core.assetOp(project: projectId, "output", [:])
        if let o = DTO.decode(OutputSettingsDTO.self, r["output"]),
           o != projectOutput {
            projectOutput = o
        }
    }

    /// The output changed (Information › Project): a command.
    func setProjectOutput(_ o: OutputSettingsDTO) {
        guard let core, let projectId, o != projectOutput else { return }
        let r = core.assetOp(project: projectId, "set-output",
                             ["output": o.json])
        if !r.ok { flash(r.message) }
        reloadAssets()
    }

    /// The project set up: its composition -- a still, a timeline at the
    /// output's frame rate, sound alone -- and its output; on the stage.
    func setUpProject(_ kind: ProjectKind, width: Int, height: Int,
                      output: OutputSettingsDTO) {
        guard let core, let projectId else { return }
        let r = core.assetOp(project: projectId, "set-up-project", [
            "still": kind == .still,
            "width": kind == .sound ? 0 : width,
            "height": kind == .sound ? 0 : height,
            "output": output.json,
        ])
        guard r.ok, let id = r["asset"] as? String else {
            flash(r.message)
            return
        }
        reloadAssets()
        if let a = assets.first(where: { $0.id == id }) { putOnStage(a) }
    }
}
