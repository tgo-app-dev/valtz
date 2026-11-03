import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// GENERATION IN THE ATLAS (DESIGN §10a Atlas). A (+) beside an asset's
/// right edge -- shown as the pointer comes near it from outside -- makes
/// a GENERATE NODE from it (a picture, a clip or a sound: chosen from its
/// menu), or links the asset to one not run yet. A node takes ONE text,
/// its prompt, and a REFERENCE ROW of the rest, in its order; opened out
/// it shows the prompt as the model will read it -- each tag the row's
/// medium at its place -- the row, reordered by dragging, and its
/// settings: the model first, then what that model takes. Generate runs
/// it: its result follows it in the graph, its seed written in, its
/// settings frozen -- Duplicate sets it up again. A link into a node not
/// run has an ⓧ that cuts it. The core keeps the nodes
/// (Controller::generate_nodes); this draws them and asks for changes.

// MARK: - The model's side

extension AppModel {
    /// The generate node `id`, as the graph last read has it.
    func atlasGenerateNode(_ id: String) -> GenerateNodeDTO? {
        atlas?.graph.nodes.first { $0.generate?.id == id }?.generate
    }

    /// The nodes not run yet: what an asset's (+) may link to.
    var atlasDrafts: [GenerateNodeDTO] {
        (atlas?.graph.nodes ?? []).compactMap(\.generate)
            .filter { $0.started == nil }
    }

    /// What a generation made from an asset of `kind` may make: a text is
    /// any one's prompt; a picture a picture's or a clip's; a clip or a
    /// sound a clip's reference or a sound's voice.
    static func atlasTargets(for kind: String) -> [String] {
        switch kind {
        case "text": ["image", "video", "audio"]
        case "image": ["image", "video"]
        case "video", "audio": ["video", "audio"]
        default: []
        }
    }

    static func atlasTargetTitle(_ t: String) -> String {
        switch t {
        case "image": String(localized: "Generate Picture")
        case "video": String(localized: "Generate Clip")
        default: String(localized: "Generate Sound")
        }
    }

    /// What the (+)'s menu calls a new one.
    static func atlasTargetItem(_ t: String) -> String {
        switch t {
        case "image": String(localized: "New Picture Generation")
        case "video": String(localized: "New Clip Generation")
        default: String(localized: "New Sound Generation")
        }
    }

    static func atlasTargetSymbol(_ t: String) -> String {
        switch t {
        case "image": "photo.badge.plus"
        case "video": "film"
        default: "waveform"
        }
    }

    static func atlasModality(_ t: String) -> Modality {
        switch t {
        case "image": .image
        case "video": .video
        default: .audio
        }
    }

    /// A node as a menu names it: what it makes, and from what.
    func atlasNodeName(_ n: GenerateNodeDTO) -> String {
        let from = n.prompt.flatMap { asset(id: $0)?.name }
            ?? n.refs.first.flatMap { asset(id: $0)?.name }
        let title = Self.atlasTargetTitle(n.target)
        guard let from, !from.isEmpty else { return title }
        return "\(title) — \(from)"
    }

    /// Can `a` go into node `n`? A text as its prompt while it has none;
    /// a medium its target reads, not in its row yet; never one run.
    func atlasCanLink(_ a: AssetDTO, to n: GenerateNodeDTO) -> Bool {
        guard n.started == nil else { return false }
        if a.kind == "text" { return n.prompt == nil }
        guard Self.atlasTargets(for: a.kind).contains(n.target) else {
            return false
        }
        return !n.refs.contains(a.id)
    }

    /// The model a node runs: its own, else Auto's for what it holds --
    /// an edit's with pictures, a clip's from references with media, a
    /// voice's with a sound -- as the core picks it.
    func atlasNodeModel(_ n: GenerateNodeDTO) -> String {
        if !n.model.isEmpty { return n.model }
        let m = Self.atlasModality(n.target)
        if n.target == "audio" {
            let voiced = n.refs.contains {
                ["audio", "video"].contains(asset(id: $0)?.kind ?? "")
            }
            let speech = baseModel(autoPick("edit", m))
            if voiced, !speech.isEmpty { return speech }
            return baseModel(autoPick("generate", m))
        }
        return baseModel(autoPick(n.refs.isEmpty ? "generate" : "edit", m))
    }

    /// The model speaks words (its row's sound the voice) rather than
    /// singing: its Length is about how long, not at most.
    func atlasNodeSpeaks(_ n: GenerateNodeDTO) -> Bool {
        let id = atlasNodeModel(n)
        return catalog.first { $0.id == id }?.capabilities
            .contains("text-to-speech") == true
    }

    /// The prompt's tags that name nothing in its row: to fill in first.
    func atlasUnbound(_ n: GenerateNodeDTO) -> [String] {
        guard let words = n.prompt.flatMap({ asset(id: $0)?.text }) else {
            return []
        }
        var count: [String: Int] = [:]
        for id in n.refs {
            if let k = asset(id: id).flatMap({ Self.refKind($0.kind) }) {
                count[k, default: 0] += 1
            }
        }
        return Self.refTags(in: words)
            .filter { (count[$0.kind] ?? 0) <= $0.index }
            .map(\.text)
    }

    /// Where a node's generation is.
    enum AtlasRun {
        case draft
        /// Queued or running: its task.
        case working(TaskDTO)
        case made
        case failed
        /// Made, and the result removed since.
        case gone
    }

    func atlasRun(_ n: GenerateNodeDTO) -> AtlasRun {
        guard let s = n.started else { return .draft }
        if let t = tasks.first(where: { $0.job == s.job }) {
            return .working(t)
        }
        if jobs[s.job]?.state == "failed" { return .failed }
        guard let id = s.asset, let a = asset(id: id) else { return .gone }
        return a.head > 0 ? .made : .failed
    }

    /// One change to a node, the graph read again; the node as it is
    /// after (nil: refused, said).
    @discardableResult
    private func atlasNodeOp(_ action: String,
                             _ extra: [String: Any]) -> [String: Any]? {
        guard let core, let projectId, !refusedForAgent() else { return nil }
        let r = core.generateNode(project: projectId, action, extra)
        if !r.ok {
            flash(r.message, level: .error)
            return nil
        }
        reloadAtlas(force: true)
        return r.payload["node"] as? [String: Any]
    }

    /// A new node made from `asset`, opened out: its model and settings
    /// to choose.
    func atlasNewGeneration(_ target: String, from asset: String) {
        guard let n = atlasNodeOp("add", ["target": target,
                                          "assets": [asset]]),
              let id = n["id"] as? String else { return }
        atlasSelected = "g:" + id
        atlasNodeTuneOpen = false
        atlasNodeOpen = id
    }

    func atlasLink(_ asset: String, to node: String) {
        atlasNodeOp("link", ["node": node, "asset": asset])
    }

    func atlasUnlink(_ asset: String, from node: String) {
        atlasNodeOp("unlink", ["node": node, "asset": asset])
    }

    /// Its row in a new order.
    func atlasOrder(_ node: String, _ refs: [String]) {
        atlasNodeOp("order", ["node": node, "refs": refs])
    }

    /// Settings laid over its own. Another model: Custom's values are
    /// that model's, from its preset again.
    func atlasSetNode(_ node: String, _ settings: [String: Any]) {
        var s = settings
        if let m = settings["model"] as? String,
           m != atlasGenerateNode(node)?.model {
            s["tuning"] = [String: Any]()
        }
        atlasNodeOp("set", ["node": node, "settings": s])
        if s["model"] != nil || s["preference"] != nil
            || s["custom"] != nil {
            atlasNodeTuning = nil
        }
    }

    /// Run: the editor's request, made from the node; its result in the
    /// graph after it, its task in Assets.
    func atlasStartNode(_ node: String) {
        guard let core, let projectId, !refusedForAgent() else { return }
        let r = core.generateNode(project: projectId, "start",
                                  ["node": node])
        if !r.ok {
            flash(r.message, level: .error)
            return
        }
        atlasNodeTuning = nil
        atlasNodeTuneOpen = false
        reloadTasks()
        reloadAssets(force: true)
        reloadAtlas(force: true)
    }

    /// Set up again from one: its settings and links, a draft, opened.
    func atlasDuplicateNode(_ node: String) {
        guard let n = atlasNodeOp("duplicate", ["node": node]),
              let id = n["id"] as? String else { return }
        atlasSelected = "g:" + id
        atlasNodeTuneOpen = false
        atlasNodeOpen = id
    }

    func atlasRemoveNode(_ node: String) {
        guard atlasNodeOp("remove", ["node": node]) != nil else { return }
        if atlasNodeOpen == node { atlasNodeOpen = nil }
    }
}

// MARK: - Custom, for a node

/// A node's Custom values as the Custom panel edits them (TuningHost):
/// read from the node, settled by the core for the model it runs, written
/// back into the node.
@MainActor @Observable
final class AtlasNodeTuning: TuningHost {
    let node: String
    let modelId: String
    private(set) var base: Preference
    let edit: Bool
    var tuningInfo: TuningInfo?
    @ObservationIgnored private weak var app: AppModel?

    init(app: AppModel, node: GenerateNodeDTO) {
        self.app = app
        self.node = node.id
        modelId = app.atlasNodeModel(node)
        base = Preference(rawValue: node.preference) ?? .balanced
        edit = node.target == "image" && !node.refs.isEmpty
        load()
    }

    /// Its values as the node keeps them, settled; the first time, the
    /// preset's -- kept as its own.
    private func load() {
        guard let app, let core = app.core, let project = app.projectId,
              !modelId.isEmpty else { return }
        let r = core.generateNode(project: project, "get", ["node": node])
        let raw = (r.payload["node"] as? [String: Any])?["tuning"]
            as? [String: Any] ?? [:]
        tuningInfo = core.tuning(model: modelId, preference: base.rawValue,
                                 edit: edit, overrides: raw)
        if raw.isEmpty, let info = tuningInfo { keep(info) }
    }

    private func keep(_ info: TuningInfo, preference: Preference? = nil) {
        var s: [String: Any] = ["custom": true,
                                "tuning": info.tuning.json]
        if let preference { s["preference"] = preference.rawValue }
        app?.atlasSetNodeTuning(node, s)
    }

    var tuningTitle: String { app?.modelName(modelId) ?? modelId }
    var tuningRegenerates: Bool { false }
    var tuningCore: CoreService? { app?.core }
    func tuningWarn(_ text: String) { app?.flash(text, level: .warning) }

    func settleTuning(_ t: Tuning, stepsFollow: Bool) {
        guard let core = tuningCore else { return }
        var json = t.json
        if stepsFollow { json["steps"] = nil }
        guard let next = core.tuning(model: modelId,
                                     preference: base.rawValue,
                                     edit: edit, overrides: json)
        else { return }
        tuningInfo = next
        keep(next)
    }

    func resetTuning(to p: Preference) {
        guard let core = tuningCore else { return }
        let before = tuningInfo?.tuning
        guard let preset = core.tuning(model: modelId,
                                       preference: p.rawValue, edit: edit,
                                       overrides: [:]) else { return }
        var t = preset.tuning
        for var l in before?.loras ?? []
            where !t.loras.contains(where: { $0.path == l.path }) {
            l.on = false
            t.loras.append(l)
        }
        base = p
        let info = core.tuning(model: modelId, preference: p.rawValue,
                               edit: edit, overrides: t.json) ?? preset
        tuningInfo = info
        keep(info, preference: p)
    }
}

extension AppModel {
    /// Custom's values written into the node, the panel's host kept (it
    /// has them already).
    func atlasSetNodeTuning(_ node: String, _ settings: [String: Any]) {
        guard let core, let projectId, !refusedForAgent() else { return }
        let r = core.generateNode(project: projectId, "set",
                                  ["node": node, "settings": settings])
        if !r.ok { flash(r.message, level: .error) }
        reloadAtlas(force: true)
    }
}

// MARK: - The (+) beside a card

/// Beside an asset's card, past its right edge: a (+) as the pointer comes
/// near -- its menu a new generation from it, or a link into a node not
/// run that takes it.
struct AtlasPlus: View {
    @Bindable var model: AppModel
    let asset: AssetDTO
    let scale: CGFloat
    @State private var near = false
    /// How far past the edge it is near, at actual size.
    static let band: CGFloat = 44

    private func u(_ v: CGFloat) -> CGFloat { v * scale }

    var body: some View {
        let targets = AppModel.atlasTargets(for: asset.kind)
        ZStack {
            // The band the pointer comes into, from outside the card.
            Color.clear
                .frame(width: u(Self.band))
                .frame(maxHeight: .infinity)
                .contentShape(Rectangle())
                .onHover { near = $0 }
            if near && !targets.isEmpty && !model.agentHolds {
                Menu {
                    menu(targets)
                } label: {
                    Image(systemName: "plus.circle.fill")
                        .font(.system(size: u(18)))
                        .symbolRenderingMode(.palette)
                        .foregroundStyle(.white, Color.accentColor)
                }
                .menuStyle(.borderlessButton)
                .menuIndicator(.hidden)
                .fixedSize()
                .help("Generate from it, or link it to a generation")
                .onHover { if $0 { near = true } }
                .transition(.opacity)
            }
        }
        .animation(.easeOut(duration: 0.12), value: near)
    }

    @ViewBuilder
    private func menu(_ targets: [String]) -> some View {
        Section("New Generation") {
            ForEach(targets, id: \.self) { t in
                Button {
                    model.atlasNewGeneration(t, from: asset.id)
                } label: {
                    Label(AppModel.atlasTargetItem(t),
                          systemImage: AppModel.atlasTargetSymbol(t))
                }
            }
        }
        let into = model.atlasDrafts.filter {
            model.atlasCanLink(asset, to: $0)
        }
        if !into.isEmpty {
            Section("Link To") {
                ForEach(into, id: \.id) { n in
                    Button {
                        model.atlasLink(asset.id, to: n.id)
                    } label: {
                        Label(model.atlasNodeName(n),
                              systemImage: AppModel.atlasTargetSymbol(
                                  n.target))
                    }
                }
            }
        }
    }
}

// MARK: - A node's card

/// A generate node in the graph, at its zoom: what it makes, its model
/// and Favor, what it holds -- and Generate, or, run, how it went and
/// Duplicate. Opened out, AtlasGenerateExpanded.
struct AtlasGenerateNode: View {
    @Bindable var model: AppModel
    let node: AtlasNodeDTO
    let gen: GenerateNodeDTO
    let scale: CGFloat
    @State private var hovering = false
    @State private var lastClick = Date.distantPast

    private func u(_ v: CGFloat) -> CGFloat { v * scale }
    private var selected: Bool { model.atlasSelected == node.id }
    private var open: Bool { model.atlasNodeOpen == gen.id }

    var body: some View {
        VStack(alignment: .leading, spacing: u(3)) {
            HStack(spacing: u(5)) {
                Image(systemName: AppModel.atlasTargetSymbol(gen.target))
                    .font(.system(size: u(11)))
                    .foregroundStyle(Color.accentColor)
                Text(verbatim: AppModel.atlasTargetTitle(gen.target))
                    .font(.system(size: u(11.5), weight: .semibold))
                    .lineLimit(1)
                Spacer(minLength: 0)
                Button {
                    model.atlasNodeOpen = open ? nil : gen.id
                } label: {
                    Image(systemName: open ? "chevron.left.circle"
                                           : "chevron.right.circle")
                        .font(.system(size: u(11.5)))
                }
                .buttonStyle(.borderless)
                .help(open ? "Close" : "Open: the prompt, the row and the settings")
            }
            Text(verbatim: settingsLine)
                .font(.system(size: u(10.5)))
                .foregroundStyle(.secondary)
                .lineLimit(1)
            Text(verbatim: holdsLine)
                .font(.system(size: u(10.5)))
                .foregroundStyle(lacks ? Color.orange : .secondary)
                .lineLimit(1)
            Spacer(minLength: 0)
            HStack(spacing: u(6)) {
                AtlasRunBadge(model: model, gen: gen, scale: scale)
                Spacer(minLength: 0)
                action
            }
        }
        .padding(.horizontal, u(8))
        .padding(.vertical, u(7))
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Color.valtzPanel,
                    in: RoundedRectangle(cornerRadius: u(8),
                                         style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: u(8), style: .continuous)
                .strokeBorder(selected || open ? Color.accentColor
                              : Color.accentColor.opacity(0.5),
                              style: StrokeStyle(
                                  lineWidth: max(0.5, u(selected ? 2 : 1.25)),
                                  dash: gen.started == nil ? [u(4), u(3)]
                                      : []))
        }
        .padding(.vertical, u(4))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture { click() }
        .contextMenu { menu }
        .help(Text(verbatim: model.atlasNodeName(gen)))
    }

    // A click selects; two open it out.
    private func click() {
        let now = Date()
        if now.timeIntervalSince(lastClick) < NSEvent.doubleClickInterval {
            model.atlasNodeOpen = gen.id
            lastClick = .distantPast
        } else {
            model.atlasSelected = node.id
            lastClick = now
        }
    }

    /// "Krea 2 Turbo · Med", "Auto: Krea 2 Turbo · Tune".
    private var settingsLine: String {
        let id = gen.started?.model ?? model.atlasNodeModel(gen)
        var name = id.isEmpty ? String(localized: "No model here")
            : model.modelName(id)
        if gen.model.isEmpty && gen.started == nil && !id.isEmpty {
            name = String(localized: "Auto: \(name)")
        }
        let favor = gen.custom ? Preference.custom.label
            : (Preference(rawValue: gen.preference) ?? .balanced).label
        return "\(name) · \(favor)"
    }

    /// Its prompt or none, and its row.
    private var holdsLine: String {
        let refs = gen.refs.count
        let row = refs == 0 ? "" : refs == 1
            ? String(localized: "1 reference")
            : String(localized: "\(String(refs)) references")
        guard gen.prompt != nil else {
            return row.isEmpty ? String(localized: "No prompt yet")
                : String(localized: "No prompt · \(row)")
        }
        return row.isEmpty ? String(localized: "Prompt")
            : String(localized: "Prompt · \(row)")
    }

    private var lacks: Bool {
        gen.started == nil
            && (gen.prompt == nil || !model.atlasUnbound(gen).isEmpty)
    }

    @ViewBuilder
    private var action: some View {
        if gen.started == nil {
            Button {
                model.atlasStartNode(gen.id)
            } label: {
                pill("play.fill", on: !lacks && !model.agentHolds)
            }
            .buttonStyle(.plain)
            .disabled(lacks || model.agentHolds)
            .help(lacks
                  ? String(localized: "Link a text to be its prompt, and fill in what its tags name")
                  : String(localized: "Run it: its result follows it here"))
        } else {
            Button {
                model.atlasDuplicateNode(gen.id)
            } label: {
                pill("plus.square.on.square", on: !model.agentHolds)
            }
            .buttonStyle(.plain)
            .disabled(model.agentHolds)
            .help("Set it up again -- its settings and links -- to run once more")
        }
    }

    /// Its action as a small capsule: Generate's ▶, Duplicate's copies.
    private func pill(_ symbol: String, on: Bool) -> some View {
        Image(systemName: symbol)
            .font(.system(size: u(10), weight: .semibold))
            .foregroundStyle(on ? Color.white : Color.secondary)
            .padding(.horizontal, u(9))
            .padding(.vertical, u(3))
            .background(on ? Color.accentColor : Color.secondary.opacity(0.2),
                        in: Capsule())
    }

    @ViewBuilder
    private var menu: some View {
        Button(open ? "Close" : "Open") {
            model.atlasNodeOpen = open ? nil : gen.id
        }
        if gen.started == nil {
            Button("Generate") { model.atlasStartNode(gen.id) }
                .disabled(lacks || model.agentHolds)
        } else {
            Button("Duplicate") { model.atlasDuplicateNode(gen.id) }
                .disabled(model.agentHolds)
            if let made = gen.started?.asset, model.asset(id: made) != nil {
                Button("Open Large") { model.atlasExpanded = made }
            }
        }
        Divider()
        Button("Remove Generation Node") { model.atlasRemoveNode(gen.id) }
            .disabled(model.agentHolds)
    }
}

/// How a node's generation is: set up, queued or running (its task's
/// badge), made, failed.
struct AtlasRunBadge: View {
    @Bindable var model: AppModel
    let gen: GenerateNodeDTO
    var scale: CGFloat = 1

    var body: some View {
        let (text, tint): (String, Color) = switch model.atlasRun(gen) {
        case .draft: (String(localized: "Set Up"), .secondary)
        case .working(let t): (t.running
                                ? String(localized: "\(t.badge) Running")
                                : String(localized: "\(t.badge) Queued"),
                               .orange)
        case .made: (String(localized: "Made"), .green)
        case .failed: (String(localized: "Failed"), .red)
        case .gone: (String(localized: "Removed"), .secondary)
        }
        Text(verbatim: text)
            .font(.system(size: 9.5 * scale, weight: .semibold))
            .foregroundStyle(.white)
            .padding(.horizontal, 6 * scale)
            .padding(.vertical, 2 * scale)
            .background(tint.opacity(0.85), in: Capsule())
    }
}

// MARK: - A node opened out

/// A generate node opened out, at actual size beside its card: the
/// prompt as the model will read it, the reference row -- dragged to
/// reorder, ⓧ to take one out -- and the settings: the model first, then
/// its size, length, Favor (Custom's panel below), seed. Run, it is as it
/// ran: the seed written in, nothing to change -- Duplicate.
struct AtlasGenerateExpanded: View {
    @Bindable var model: AppModel
    let gen: GenerateNodeDTO
    @State private var thumbs: [String: CGImage] = [:]
    @State private var width = ""
    @State private var height = ""
    @State private var seconds = ""
    @State private var seed = ""
    @State private var dragging: String?

    static let narrow: CGFloat = 460
    static let wide: CGFloat = 720
    private var frozen: Bool { gen.started != nil || model.agentHolds }
    private var tune: Bool { model.atlasNodeTuneOpen && gen.custom }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            header
            section(String(localized: "Prompt")) { prompt }
            section(gen.refs.isEmpty ? String(localized: "References")
                    : String(localized: "References (\(String(gen.refs.count)))"))
            { row }
            section(String(localized: "Settings")) { settings }
            if tune, let host = model.atlasNodeTuning {
                Divider()
                ScrollView {
                    TuningPanel(model: host, embedded: true)
                        .disabled(frozen)
                }
                .frame(maxHeight: 360)
            }
        }
        .padding(14)
        .frame(width: tune ? Self.wide : Self.narrow, alignment: .leading)
        .background(Color.valtzPanel,
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Color.accentColor, lineWidth: 1.5)
        }
        .shadow(color: .black.opacity(0.28), radius: 18, y: 6)
        .controlSize(.small)
        .onAppear { fill() }
        .onChange(of: gen) { fill() }
        .task(id: gen.refs.joined(separator: ",")) { await loadThumbs() }
        .task(id: "\(gen.id)-\(gen.model)-\(gen.preference)-\(gen.custom)-\(tune)") {
            if tune, model.atlasNodeTuning?.node != gen.id
                || model.atlasNodeTuning?.modelId
                    != (gen.started?.model ?? model.atlasNodeModel(gen)) {
                model.atlasNodeTuning = AtlasNodeTuning(app: model, node: gen)
            }
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: AppModel.atlasTargetSymbol(gen.target))
                .foregroundStyle(Color.accentColor)
            Text(verbatim: AppModel.atlasTargetTitle(gen.target))
                .font(.headline)
            AtlasRunBadge(model: model, gen: gen)
            Spacer()
            if gen.started == nil {
                Button {
                    model.atlasStartNode(gen.id)
                } label: {
                    Label("Generate", systemImage: "play.fill")
                }
                .buttonStyle(.borderedProminent)
                .disabled(cannotStart != nil || model.agentHolds)
                .help(cannotStart ?? String(localized: "Run it: its result follows it here"))
            } else {
                Button {
                    model.atlasDuplicateNode(gen.id)
                } label: {
                    Label("Duplicate", systemImage: "plus.square.on.square")
                }
                .disabled(model.agentHolds)
                .help("Set it up again -- its settings and links -- to run once more")
            }
            Button {
                model.atlasNodeOpen = nil
            } label: {
                Image(systemName: "xmark")
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.cancelAction)
            .help("Close")
        }
    }

    /// Why Generate is off, or nil.
    private var cannotStart: String? {
        if gen.prompt == nil {
            return String(localized: "Link a text to be its prompt: its (+) › Link To")
        }
        if let tag = model.atlasUnbound(gen).first {
            return String(localized: "Fill in the row first: \(tag) names nothing yet.")
        }
        return nil
    }

    private func section<Content: View>(
        _ title: String, @ViewBuilder _ content: () -> Content
    ) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(verbatim: title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            content()
        }
    }

    // The prompt as the model reads it: each tag the row's medium at its
    // place (as the prompt box draws it) -- reordering the row moves them.
    @ViewBuilder
    private var prompt: some View {
        if let id = gen.prompt, let a = model.asset(id: id) {
            AtlasPromptPreview(words: a.text ?? "", chips: chips(a.text ?? ""))
                .frame(height: 120)
                .background(Color(nsColor: .textBackgroundColor),
                            in: RoundedRectangle(cornerRadius: 6))
            .overlay(alignment: .topTrailing) {
                if !frozen {
                    Button {
                        model.atlasUnlink(id, from: gen.id)
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.borderless)
                    .padding(4)
                    .help("Unlink the prompt")
                }
            }
            Text(verbatim: a.name)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
        } else {
            Text("Link a text to be its prompt: the (+) beside it › Link To. A generation takes one.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
    }

    /// Each tag of the words drawn as the prompt box draws it: "Image 2"
    /// joined to the row's second picture -- red while the row has none.
    private func chips(_ words: String) -> [AtlasPromptPreview.Chip] {
        let row = gen.refs.compactMap { model.asset(id: $0) }
        return AppModel.refTags(in: words).map { t in
            let of = row.filter { AppModel.refKind($0.kind) == t.kind }
            let item = of.indices.contains(t.index) ? of[t.index] : nil
            return .init(range: t.range,
                         image: PromptTextView.refTagChip(
                             kind: t.kind, index: t.index,
                             bound: item != nil,
                             thumb: item.flatMap { thumbs[$0.id] }),
                         key: "\(t.kind)\(t.index)-\(item?.id ?? "")-\(item.map { thumbs[$0.id] != nil } ?? false)")
        }
    }

    // The row: its media in order, numbered by kind as tags name them.
    @ViewBuilder
    private var row: some View {
        if gen.refs.isEmpty {
            Text(rowHint)
                .font(.callout)
                .foregroundStyle(.secondary)
        } else {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(Array(gen.refs.enumerated()), id: \.element) {
                        i, id in
                        rowItem(i, id)
                    }
                }
                .padding(.vertical, 2)
            }
            if !frozen && gen.refs.count > 1 {
                Text("Drag to reorder: the prompt's tags follow.")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
        }
    }

    private var rowHint: String {
        switch gen.target {
        case "image":
            String(localized: "None: made from the words. Link pictures with their (+): the first is the one edited.")
        case "video":
            String(localized: "None: made from the words. Link pictures, clips or sounds with their (+): the clip draws on them.")
        default:
            String(localized: "None. Link a sound or a clip with its (+): a voice to speak in.")
        }
    }

    private func rowItem(_ i: Int, _ id: String) -> some View {
        let a = model.asset(id: id)
        let kind = a.flatMap { AppModel.refKind($0.kind) } ?? ""
        let number = gen.refs.prefix(i).filter {
            model.asset(id: $0).flatMap { AppModel.refKind($0.kind) } == kind
        }.count + 1
        return VStack(spacing: 3) {
            ZStack {
                Color.black.opacity(0.85)
                if let img = thumbs[id] {
                    Image(decorative: img, scale: 2)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                } else if let a {
                    Image(systemName: AtlasAssetNode.symbol(a))
                        .foregroundStyle(.white.opacity(0.5))
                }
            }
            .frame(width: 72, height: 54)
            .clipShape(RoundedRectangle(cornerRadius: 5))
            .overlay(alignment: .topLeading) {
                Text(verbatim: "\(RefTagAttachment.kindName(kind)) \(number)")
                    .font(.system(size: 9, weight: .semibold))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 4)
                    .padding(.vertical, 1)
                    .background(Color.accentColor, in: Capsule())
                    .padding(3)
            }
            .overlay(alignment: .topTrailing) {
                if !frozen {
                    Button {
                        model.atlasUnlink(id, from: gen.id)
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .symbolRenderingMode(.palette)
                            .foregroundStyle(.white, .black.opacity(0.6))
                    }
                    .buttonStyle(.plain)
                    .padding(3)
                    .help("Take it out of the row")
                }
            }
            .opacity(dragging == id ? 0.4 : 1)
            Text(verbatim: i == 0 && gen.target == "image"
                 ? String(localized: "Edited")
                 : a?.name ?? "")
                .font(.caption2)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .frame(width: 72)
        }
        .onDrag {
            dragging = id
            return NSItemProvider(object: (Self.dragPrefix + id) as NSString)
        }
        .onDrop(of: [UTType.text], delegate: RowDrop(
            target: id, gen: gen, model: model, dragging: $dragging,
            frozen: frozen))
    }

    static let dragPrefix = "valtz-node-ref:"

    /// A row item dropped on another: it takes that one's place.
    private struct RowDrop: DropDelegate {
        let target: String
        let gen: GenerateNodeDTO
        let model: AppModel
        @Binding var dragging: String?
        let frozen: Bool

        func validateDrop(info: DropInfo) -> Bool {
            !frozen && dragging != nil
        }

        func dropUpdated(info: DropInfo) -> DropProposal? {
            DropProposal(operation: .move)
        }

        func performDrop(info: DropInfo) -> Bool {
            defer { dragging = nil }
            guard let from = dragging, from != target,
                  let i = gen.refs.firstIndex(of: from),
                  let j = gen.refs.firstIndex(of: target) else {
                return false
            }
            var refs = gen.refs
            refs.remove(at: i)
            refs.insert(from, at: j)
            Task { @MainActor in model.atlasOrder(gen.id, refs) }
            return true
        }
    }

    // The settings: the model first, then what it takes.
    private var settings: some View {
        Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 8) {
            GridRow {
                label("Model")
                modelPicker
            }
            if gen.target != "audio" {
                GridRow {
                    label("Size")
                    sizeRow
                }
            }
            if gen.target != "image" {
                GridRow {
                    label(gen.target == "audio" && !model.atlasNodeSpeaks(gen)
                          ? "Length at most" : "Length")
                    HStack(spacing: 4) {
                        TextField("Default", text: $seconds)
                            .frame(width: 64)
                            .onSubmit { commitSeconds() }
                        Text("s").foregroundStyle(.secondary)
                    }
                    .disabled(frozen)
                }
            }
            GridRow {
                label("Favor")
                favorRow
            }
            GridRow {
                label("Seed")
                if let s = gen.started {
                    Text(verbatim: String(s.seed))
                        .monospacedDigit()
                        .textSelection(.enabled)
                } else {
                    TextField("Random", text: $seed)
                        .frame(width: 120)
                        .onSubmit { commitSeed() }
                        .disabled(frozen)
                }
            }
        }
    }

    private func label(_ key: LocalizedStringKey) -> some View {
        Text(key)
            .foregroundStyle(.secondary)
            .gridColumnAlignment(.trailing)
    }

    private var modelPicker: some View {
        let options = model.modelOptions(for: AppModel.atlasModality(gen.target))
        let auto = model.atlasNodeModel(
            GenerateNodeDTO(id: gen.id, target: gen.target,
                            prompt: gen.prompt, refs: gen.refs, model: "",
                            preference: gen.preference, custom: gen.custom,
                            width: gen.width, height: gen.height,
                            seconds: gen.seconds, plan: gen.plan,
                            seed: gen.seed, started: nil))
        let autoName = auto.isEmpty ? String(localized: "none here")
            : model.modelName(auto)
        return Picker("Model", selection: Binding(
            get: { gen.started?.model ?? gen.model },
            set: { model.atlasSetNode(gen.id, ["model": $0]) })) {
            if gen.started == nil {
                Text("Auto (\(autoName))").tag("")
                Divider()
            }
            ForEach(options, id: \.model) { o in
                Text(verbatim: o.downloading == true
                     ? String(localized: "\(o.name) · Downloading")
                     : o.name)
                    .tag(o.model)
            }
            // A model it ran on that is no longer listed.
            if let ran = gen.started?.model,
               !options.contains(where: { $0.model == ran }) {
                Text(verbatim: model.modelName(ran)).tag(ran)
            }
        }
        .labelsHidden()
        .fixedSize()
        .disabled(frozen)
    }

    private var sizeRow: some View {
        HStack(spacing: 4) {
            TextField("Auto", text: $width)
                .frame(width: 56)
                .onSubmit { commitSize() }
            Text(verbatim: "×").foregroundStyle(.secondary)
            TextField("Auto", text: $height)
                .frame(width: 56)
                .onSubmit { commitSize() }
            Menu {
                ForEach(presets, id: \.0) { p in
                    Button(p.0) {
                        model.atlasSetNode(gen.id, ["width": p.1,
                                                    "height": p.2])
                    }
                }
            } label: {
                Image(systemName: "rectangle.3.group")
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            .help("A size it is made at")
        }
        .disabled(frozen)
    }

    /// Sizes to pick: the model's own (an edit: its picture's shape),
    /// then each shape at about the area a model makes them.
    private var presets: [(String, Int, Int)] {
        let own = gen.target == "image" && !gen.refs.isEmpty
            ? String(localized: "Auto: as the picture edited")
            : String(localized: "Auto: the model's")
        if gen.target == "video" {
            return [(own, 0, 0), ("16:9 · 832 × 480", 832, 480),
                    ("9:16 · 480 × 832", 480, 832),
                    ("16:9 · 1344 × 768", 1344, 768),
                    ("9:16 · 768 × 1344", 768, 1344),
                    ("1:1 · 640 × 640", 640, 640)]
        }
        return [(own, 0, 0), ("1:1 · 1024 × 1024", 1024, 1024),
                ("3:2 · 1216 × 832", 1216, 832),
                ("2:3 · 832 × 1216", 832, 1216),
                ("16:9 · 1344 × 768", 1344, 768),
                ("9:16 · 768 × 1344", 768, 1344)]
    }

    private var favorRow: some View {
        VStack(alignment: .leading, spacing: 4) {
            Picker("Favor", selection: Binding(
                get: { gen.custom ? Preference.custom
                    : Preference(rawValue: gen.preference) ?? .balanced },
                set: { p in
                    if p == .custom {
                        model.atlasSetNode(gen.id, ["custom": true])
                        model.atlasNodeTuneOpen = true
                    } else {
                        model.atlasSetNode(gen.id, ["custom": false,
                                                    "preference": p.rawValue])
                    }
                })) {
                ForEach(Preference.allCases) { p in
                    Text(p.label).tag(p)
                }
            }
            .labelsHidden()
            .pickerStyle(.segmented)
            .fixedSize()
            .disabled(frozen)
            if gen.custom {
                Button {
                    model.atlasNodeTuneOpen.toggle()
                } label: {
                    Label(tune ? "Hide Custom's Options" : "Custom's Options…",
                          systemImage: tune ? "chevron.up" : "slider.horizontal.3")
                }
                .buttonStyle(.borderless)
            }
        }
    }

    // The fields as the node has them, again after each change.
    private func fill() {
        width = gen.width > 0 ? String(gen.width) : ""
        height = gen.height > 0 ? String(gen.height) : ""
        seconds = gen.seconds > 0 ? String(format: "%g", gen.seconds) : ""
        seed = gen.seed >= 0 ? String(gen.seed) : ""
    }

    private func commitSize() {
        let w = Int(width.trimmingCharacters(in: .whitespaces)) ?? 0
        let h = Int(height.trimmingCharacters(in: .whitespaces)) ?? 0
        model.atlasSetNode(gen.id, ["width": max(0, w), "height": max(0, h)])
    }

    private func commitSeconds() {
        let s = Double(seconds.trimmingCharacters(in: .whitespaces)) ?? 0
        model.atlasSetNode(gen.id, ["seconds": max(0, s)])
    }

    private func commitSeed() {
        let t = seed.trimmingCharacters(in: .whitespaces)
        model.atlasSetNode(gen.id, ["seed": Int64(t).map { max(0, $0) } ?? -1])
    }

    private func loadThumbs() async {
        guard let core = model.core, let project = model.projectId else {
            return
        }
        for id in gen.refs where thumbs[id] == nil {
            guard let a = model.asset(id: id) else { continue }
            if a.kind == "audio", let url = a.url {
                if let img = await AudioWaveform.image(url, width: 144,
                                                       height: 108) {
                    thumbs[id] = img
                }
                continue
            }
            let img = await Task.detached(priority: .utility) {
                core.thumbnail(project: project, asset: id, maxPixels: 160)
                    .flatMap { AppModel.loadImage($0) }
            }.value
            if let img { thumbs[id] = img }
        }
    }
}

// MARK: - A link's ⓧ

/// On a link into a node not run: a click cuts it -- the asset out of
/// its row, or its prompt.
struct AtlasLinkCut: View {
    @Bindable var model: AppModel
    let asset: String
    let node: String
    let scale: CGFloat
    @State private var hovering = false

    var body: some View {
        Button {
            model.atlasUnlink(asset, from: node)
        } label: {
            Image(systemName: "xmark.circle.fill")
                .font(.system(size: max(9, 13 * scale)))
                .symbolRenderingMode(.palette)
                .foregroundStyle(.white,
                                 hovering ? Color.red
                                          : Color.secondary.opacity(0.75))
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .disabled(model.agentHolds)
        .help("Disconnect it from the generation")
    }
}

// MARK: - The prompt, as read

/// A node's prompt as its model reads it, read only: its words in the
/// Prompt Editor's type, each tag the chip of what the row holds at its
/// place (PromptTextView.refTagChip).
struct AtlasPromptPreview: NSViewRepresentable {
    struct Chip {
        let range: NSRange
        let image: NSImage
        /// What it shows: drawn again only when that changes.
        let key: String
    }
    let words: String
    let chips: [Chip]

    final class Coordinator {
        var shown: String?
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSScrollView {
        let sv = NSTextView.scrollableTextView()
        sv.drawsBackground = false
        sv.hasVerticalScroller = true
        if let tv = sv.documentView as? NSTextView {
            tv.isEditable = false
            tv.isSelectable = true
            tv.drawsBackground = false
            tv.isRichText = true
            tv.textContainerInset = NSSize(width: 8, height: 8)
        }
        return sv
    }

    func updateNSView(_ sv: NSScrollView, context: Context) {
        let key = words + "|" + chips.map(\.key).joined(separator: ",")
        guard let tv = sv.documentView as? NSTextView,
              context.coordinator.shown != key else { return }
        context.coordinator.shown = key
        let font = PromptEditor.font
        let para = NSMutableParagraphStyle()
        para.lineHeightMultiple = 1.1
        para.paragraphSpacing = 6
        let attrs: [NSAttributedString.Key: Any] = [
            .font: font, .foregroundColor: NSColor.labelColor,
            .paragraphStyle: para]
        let out = NSMutableAttributedString(string: words, attributes: attrs)
        // From the end: each tag's text made its chip.
        for c in chips.sorted(by: { $0.range.location > $1.range.location })
        where NSMaxRange(c.range) <= out.length {
            let att = NSTextAttachment()
            att.image = c.image
            att.bounds = CGRect(x: 0, y: font.descender,
                                width: c.image.size.width,
                                height: c.image.size.height)
            let chip = NSMutableAttributedString(attachment: att)
            chip.addAttributes(attrs, range: NSRange(location: 0,
                                                     length: chip.length))
            out.replaceCharacters(in: c.range, with: chip)
        }
        tv.textStorage?.setAttributedString(out)
    }
}
