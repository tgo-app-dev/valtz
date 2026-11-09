import SwiftUI

/// PROCEDURAL (DESIGN §4t): the prompt says what to draw, and a model
/// writes a PROGRAM -- JavaScript a sandboxed page runs: a canvas, WebGL,
/// SVG; three.js, Babylon.js -- that Valtz draws, a frame at a time, as a
/// picture or a clip. It is an agent session whose tools write programs
/// and draw them (skills "procedural"): the Agent's card and chat show
/// it, its tabs its own. Each drawing lands in Assets beside its program
/// and is put on the stage as it lands.
extension AppModel {
    /// What its programs are drawn at (AgentRequest::render): the card's
    /// -- "card" where the person sees it, Procedural's own tab: the
    /// model's own size or length then taken only where their words name
    /// it. A Programs tab of the Agent's shows no card: its defaults.
    var proceduralRender: [String: Any] {
        ["width": proceduralWidth, "height": proceduralHeight,
         "seconds": proceduralSeconds,
         "card": activeModality == .procedural]
    }

    /// The card's summary: " · 1280 × 720 · Still", " · … · 5 s".
    var proceduralSummary: String {
        let size = "\(proceduralWidth) × \(proceduralHeight)"
        let length = proceduralSeconds > 0
            ? String(localized: "\(Self.secondsText(proceduralSeconds)) s")
            : String(localized: "Still")
        return " · " + size + " · " + length
    }

    static func secondsText(_ s: Double) -> String {
        s == s.rounded() ? String(Int(s)) : String(format: "%.1f", s)
    }

    /// A size from the card's presets.
    func setProceduralSize(_ w: Int, _ h: Int) {
        proceduralWidth = min(max(w, 16), 8192)
        proceduralHeight = min(max(h, 16), 8192)
    }

    /// The session's newest drawing, put on the stage as it lands --
    /// viewed, as a click in Assets views it; a clip plays -- while the
    /// Procedural target is the one shown. Each step's once; a drawing
    /// drawn again (its next version) put there again.
    func showProgramDrawn(was: AgentSession?, next: AgentSession) {
        // A procedural session's, in either target; a chat's beside the
        // editor's agent -- not the editor's own, whose stage follows the
        // composition it works on.
        let draws = next.skills == "procedural"
            || (next.skills == "chat" && activeModality == .agent)
        guard draws, next.project == projectId,
              activeModality == .procedural || activeModality == .agent
        else { return }
        // Its frames watched as they were drawn: the drawing viewed as it
        // landed already, there.
        followProgramSteps(next)
        guard let step = next.steps.last(where: {
                  $0.tool == "render_program" && $0.state == "done"
              }),
              let text = step.resultText,
              let obj = try? JSONSerialization.jsonObject(
                  with: Data(text.utf8)) as? [String: Any],
              let id = obj["asset"] as? String,
              !programsShown.contains("\(next.agent)#\(step.n)"),
              step.job.map({ !programJobsWatched.contains($0) }) ?? true
        else { return }
        programsShown.insert("\(next.agent)#\(step.n)")
        reloadAssets()
        guard let a = asset(id: id) else { return }
        // Its next version, of the drawing on the stage already: put
        // there again.
        if viewedAsset == a.id {
            putOnStage(a)
            askAutoplay(a.id)
        } else {
            steerForAgent { viewAsset(a) }
        }
    }
}

extension AppModel {
    /// The session's program being drawn now: its frames on the stage as
    /// they come (DESIGN §4t), a clip's as a still's.
    func followProgramSteps(_ s: AgentSession) {
        guard let step = s.steps.last(where: {
                  $0.tool == "render_program" && $0.state == "running"
              }),
              let job = step.job, !job.isEmpty,
              !programJobsWatched.contains(job)
        else { return }
        // A clip, by its call's seconds -- else the card's.
        let args = (try? JSONSerialization.jsonObject(
            with: Data((step.argsText ?? "{}").utf8))) as? [String: Any]
        let seconds = (args?["seconds"] as? NSNumber)?.doubleValue
            ?? proceduralSeconds
        watchProgram(job, clip: seconds > 0)
    }

    /// Render Again (Assets' menu): a program's drawing drawn again --
    /// at another size, length or seed -- as its next version; a
    /// program, drawn anew.
    func openRenderAgain(_ a: AssetDTO) {
        var f = RenderAgainForm(asset: a.id, name: a.name)
        if a.isProgramDrawing, let r = a.recipe {
            f.program = r.inputs?.first { $0.role == "program" }?.asset ?? ""
            f.drawing = a.id
            f.assets = (r.inputs ?? []).filter { $0.role == "asset" }
                .compactMap(\.asset)
            f.width = r.params?.width ?? proceduralWidth
            f.height = r.params?.height ?? proceduralHeight
            let frames = r.params?.frames ?? 0
            let fps = Double(r.params?.fpsNum ?? 24)
                / Double(max(1, r.params?.fpsDen ?? 1))
            f.seconds = frames > 0 ? Double(frames) / fps : 0
            f.seed = r.params?.seed.flatMap { UInt64($0) }
        } else {
            f.program = a.id
            f.width = proceduralWidth
            f.height = proceduralHeight
            f.seconds = proceduralSeconds
        }
        renderAgain = f
    }

    func renderAgain(_ f: RenderAgainForm) {
        renderAgain = nil
        guard let core, let projectId, !f.program.isEmpty else { return }
        var req: [String: Any] = [
            "project": projectId, "program": f.program,
            "width": min(max(f.width, 16), 8192),
            "height": min(max(f.height, 16), 8192),
            "seconds": max(0, f.seconds), "assets": f.assets,
        ]
        if let d = f.drawing { req["drawing"] = d }
        if f.sameSeed, let s = f.seed { req["seed"] = s }
        let r = core.renderProcedural(req)
        guard r.ok, let job = r["job"] as? String else {
            flash(r.message, level: .error)
            return
        }
        reloadAssets()
        watchProgram(job, clip: f.seconds > 0, asked: true)
    }
}

/// Render Again's values: the program, its drawing (drawn again as its
/// next version), the assets it reads, its size, length and seed.
struct RenderAgainForm: Identifiable, Equatable {
    var asset: String
    var name: String
    var program = ""
    var drawing: String?
    var assets: [String] = []
    var width = 1280
    var height = 720
    var seconds = 0.0
    var seed: UInt64?
    var sameSeed = true
    var id: String { asset }
}

extension AssetDTO {
    /// A PROGRAM (DESIGN §4t): a text kept as one.
    var isProgram: Bool {
        kind == "text" && (tags ?? []).contains("program")
    }
    /// A program's drawing: a picture or a clip made from one.
    var isProgramDrawing: Bool { recipe?.op == "render-program" }
}

/// Render Again's sheet: the size, still or clip, the seed.
struct RenderAgainSheet: View {
    @Bindable var model: AppModel
    @State var form: RenderAgainForm

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(form.drawing == nil ? "Render “\(form.name)”"
                                     : "Render “\(form.name)” Again")
                .font(.headline)
            Form {
                LabeledContent("Size") {
                    HStack(spacing: 6) {
                        TextField("Width", value: $form.width,
                                  format: .number.grouping(.never))
                            .frame(width: 64)
                        Text("×").foregroundStyle(.secondary)
                        TextField("Height", value: $form.height,
                                  format: .number.grouping(.never))
                            .frame(width: 64)
                    }
                }
                LabeledContent("Length") {
                    HStack(spacing: 8) {
                        Picker("Length", selection: Binding(
                            get: { form.seconds > 0 },
                            set: { form.seconds = $0 ? 5 : 0 })) {
                            Text("Still").tag(false)
                            Text("Clip").tag(true)
                        }
                        .labelsHidden()
                        .pickerStyle(.segmented)
                        .fixedSize()
                        if form.seconds > 0 {
                            TextField("Seconds", value: $form.seconds,
                                      format: .number.precision(
                                          .fractionLength(0...1)))
                                .frame(width: 52)
                            Text("seconds").foregroundStyle(.secondary)
                        }
                    }
                }
                if form.seed != nil {
                    Toggle("Same seed", isOn: $form.sameSeed)
                        .help("The same seed draws the same picture; another, another take")
                }
            }
            .textFieldStyle(.roundedBorder)
            Text(form.drawing == nil
                 ? "Drawn as a new picture or clip, beside its program in Assets."
                 : "Drawn as its next version; a still drawn as a clip, or a clip as a still, is a new one.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Spacer()
                Button("Cancel") { model.renderAgain = nil }
                    .keyboardShortcut(.cancelAction)
                Button("Render") { model.renderAgain(form) }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 380)
    }
}

/// The Procedural card's rows (the generation card's place): the size
/// its programs are drawn at, still or a clip of so many seconds, and
/// where the writer thinks.
struct ProceduralSettingsRows: View {
    @Bindable var model: AppModel

    /// Sizes to start from: the stills and screens people draw for.
    private static let presets: [(String, Int, Int)] = [
        ("1:1", 1024, 1024), ("16:9 HD", 1280, 720),
        ("16:9 Full HD", 1920, 1080), ("9:16", 1080, 1920),
        ("4:3", 1440, 1080), ("4K", 3840, 2160),
    ]

    var body: some View {
        LabeledContent("Size") {
            HStack(spacing: 6) {
                TextField("Width", value: $model.proceduralWidth,
                          format: .number.grouping(.never))
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 64)
                    .onSubmit { model.setProceduralSize(
                        model.proceduralWidth, model.proceduralHeight) }
                Text("×").foregroundStyle(.secondary)
                TextField("Height", value: $model.proceduralHeight,
                          format: .number.grouping(.never))
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 64)
                    .onSubmit { model.setProceduralSize(
                        model.proceduralWidth, model.proceduralHeight) }
                Menu {
                    ForEach(Self.presets, id: \.0) { p in
                        Button("\(p.0) — \(String(p.1)) × \(String(p.2))") {
                            model.setProceduralSize(p.1, p.2)
                        }
                    }
                } label: {
                    Image(systemName: "rectangle.3.group")
                }
                .menuStyle(.borderlessButton)
                .fixedSize()
                .help("Sizes to start from")
            }
        }
        LabeledContent("Length") {
            HStack(spacing: 8) {
                Picker("Length", selection: Binding(
                    get: { model.proceduralSeconds > 0 },
                    set: { model.proceduralSeconds = $0 ? 5 : 0 })) {
                    Text("Still").tag(false)
                    Text("Clip").tag(true)
                }
                .labelsHidden()
                .pickerStyle(.segmented)
                .fixedSize()
                if model.proceduralSeconds > 0 {
                    TextField("Seconds", value: Binding(
                        get: { model.proceduralSeconds },
                        set: { model.proceduralSeconds = min(max($0, 0.1),
                                                             600) }),
                              format: .number.precision(.fractionLength(0...1)))
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 52)
                    Text("seconds").foregroundStyle(.secondary)
                }
            }
        }
        LabeledContent("Thinks On") {
            Picker("Thinks On", selection: Binding(
                get: { model.agentSettings?.backend ?? "helper" },
                set: { model.setAgentSettings(["backend": $0]) })) {
                Text("Agentic Helper").tag("helper")
                Text("Cloud").tag("cloud")
            }
            .labelsHidden()
            .fixedSize()
        }
        Text("Programs run in a sandboxed page with no network and no files. Each drawing lands in Assets beside its program.")
            .font(.caption)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }
}
