import SwiftUI

/// A PLACEMENT NODE opened out (DESIGN §10a Atlas): the op putting an
/// asset on a timeline -- or a composition of sound -- worked on where it
/// stands in the graph, as the Trim panel works on the selected layer:
/// its marks (the span of its source kept), its speed, where it starts on
/// the timeline, a still's length. The first of the adjustments the
/// Atlas edits node by node; the layer need not be on the stage.

extension AppModel {
    /// The composition a layer node puts its asset on, and that layer.
    func atlasPlacement(_ node: AtlasNodeDTO)
        -> (comp: AssetDTO, layer: LayerDTO, source: AssetDTO?)? {
        guard node.type == "layer", let into = node.into,
              let c = asset(id: into), let id = node.layer,
              let l = c.layers?.first(where: { $0.id == id }) else {
            return nil
        }
        return (c, l, source(of: l))
    }

    /// Whether a layer node opens out: one on a timeline, or on a
    /// composition of sound (a still's pages are the Layers section's).
    func atlasPlacementEditable(_ node: AtlasNodeDTO) -> Bool {
        guard node.step == "time", let p = atlasPlacement(node) else {
            return false
        }
        return p.comp.isTimeline || p.comp.kind == "audio"
    }

    /// The editor's card a step of a layer's chain is set in.
    static func atlasStepCard(_ step: String?)
        -> (panel: ComposerPanel, title: String)? {
        switch step {
        case "time": (.trim, String(localized: "Trim"))
        case "adjust", "look": (.adjust, String(localized: "Adjust"))
        case "color": (.color, String(localized: "Color"))
        case "grade": (.grade, String(localized: "Grade"))
        case "place": (.crop, String(localized: "Crop"))
        case "sound": (.audio, String(localized: "Audio"))
        default: nil
        }
    }

    /// The steps a layer's chain could have that it has not yet: an
    /// adjustment added to it, in the editor's card for it.
    func atlasAddable(_ node: AtlasNodeDTO) -> [String] {
        guard let p = atlasPlacement(node), let g = atlas else { return [] }
        let have = Set(g.graph.nodes.compactMap { n -> String? in
            n.type == "layer" && n.into == node.into && n.layer == node.layer
                ? n.step : nil
        })
        let sound = p.comp.kind == "audio" || p.source?.kind == "audio"
            || (p.source?.kind == "video"
                && p.source?.info?.audio?.has == true)
        let picture = p.comp.kind != "audio"
        var all: [String] = []
        if picture { all += ["adjust", "color", "grade", "place"] }
        if sound && p.comp.isComposition { all.append("sound") }
        return all.filter { !have.contains($0) }
    }

    /// An adjustment's node cut from its layer's chain: that adjustment
    /// taken off the layer (the layer itself stays, on its composition).
    func atlasRemoveStep(_ node: AtlasNodeDTO) {
        guard let core, let projectId, let into = node.into,
              let layer = node.layer, let step = node.step,
              step != "time", !(step == "place"
                  && asset(id: into)?.isTimeline != true)
        else { return }
        guard !refusedForAgent() else { return }
        let r = core.layerOp(project: projectId, asset: into,
                             "remove-adjustment", layer: layer,
                             extra: ["step": step])
        if !r.ok {
            flash(r.message, level: .error)
            return
        }
        if atlasSelected == node.id { atlasSelected = nil }
        placementWritten(into)
    }

    /// Whether a node's scissors take its adjustment off (an adjustment's
    /// node), not the layer (the node that places it).
    func atlasStepRemovable(_ node: AtlasNodeDTO) -> Bool {
        guard let step = node.step, step != "time" else { return false }
        if step == "place" {
            return asset(id: node.into ?? "")?.isTimeline == true
        }
        return true
    }

    /// A step of a layer's chain set in the editor: its composition made
    /// active, the layer selected, the step's card open (`as`: a step
    /// it has not yet -- the adjustment added there).
    func atlasOpenStep(_ node: AtlasNodeDTO, as step: String? = nil) {
        guard let into = node.into, let layer = node.layer,
              let card = Self.atlasStepCard(step ?? node.step) else {
            return
        }
        atlasOpenInEditor(into)
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(250))
            guard let self else { return }
            self.selectedLayer = layer
            self.layersChanged()
            withAnimation(AppModel.motion) { self.openPanel = card.panel }
            // The player on the layer (a card greys its values where the
            // layer does not show): at its start, where it is off it.
            if let c = self.asset(id: into), c.isTimeline {
                let t = c.time(of: layer)
                let end = t.duration > 0 ? t.offset + t.duration : Int.max
                if self.videoFrame < t.offset || self.videoFrame >= end {
                    self.sendVideo(.place(t.offset))
                }
            }
        }
    }

    /// What a composition's start counts in: its frames (a composition of
    /// sound's milliseconds).
    func atlasTimelineRate(_ c: AssetDTO) -> FrameRate {
        if c.kind == "audio" { return .milliseconds }
        return c.compositionRate ?? c.info?.frameRate ?? .fallback
    }

    /// What a layer's marks count in: its source's own frames (a sound's
    /// milliseconds), as the Trim panel counts them.
    func atlasMarkRate(_ c: AssetDTO, _ l: LayerDTO) -> FrameRate {
        if let r = c.time(of: l.id).rate { return r }
        if c.kind == "audio" { return .milliseconds }
        guard let src = source(of: l) else { return atlasTimelineRate(c) }
        if src.kind == "audio" { return .milliseconds }
        if let r = src.compositionRate { return r }
        return src.info?.frameRate ?? atlasTimelineRate(c)
    }

    /// How long its source runs, in the marks' units; nil for a still.
    func atlasSourceLength(_ c: AssetDTO, _ l: LayerDTO) -> Int? {
        guard let src = source(of: l) else { return nil }
        if src.isComposition, src.isTimeline, let n = src.length {
            return n
        }
        return runSeconds(of: src, asSound: c.kind == "audio")
            .map { Int(($0 * atlasMarkRate(c, l).fps).rounded()) }
    }

    /// A layer's time written as the node's panel sets it -- its marks
    /// (-1: none), its start, a still's length -- one command.
    func atlasSetPlacement(_ node: AtlasNodeDTO, markIn: Int? = nil,
                           markOut: Int? = nil, offset: Int? = nil,
                           duration: Int? = nil) {
        guard let core, let projectId, let p = atlasPlacement(node) else {
            return
        }
        guard !refusedForAgent() else { return }
        var t = p.comp.time(of: p.layer.id)
        if markIn != nil || markOut != nil {
            if let i = markIn { t.in = i }
            if let o = markOut { t.out = o }
            // A mark past the other one: the other cleared.
            if t.in >= 0, t.out >= 0, t.out < t.in {
                if markIn != nil { t.out = -1 } else { t.in = -1 }
            }
            let r = atlasMarkRate(p.comp, p.layer)
            let none = t.in < 0 && t.out < 0
            t.rateNum = none ? 0 : r.num
            t.rateDen = none ? 1 : r.den
        }
        if let o = offset { t.offset = max(0, o) }
        if let d = duration { t.duration = max(1, d) }
        let r = core.setLayerTime(project: projectId, asset: p.comp.id,
                                  layer: p.layer.id, t.json)
        if !r.ok {
            flash(r.message, level: .error)
            return
        }
        placementWritten(p.comp.id)
    }

    /// Its speed, the whole layer's (a keyed speed is the Trim panel's).
    func atlasSetPlacementSpeed(_ node: AtlasNodeDTO, _ rate: Double) {
        guard let core, let projectId, let p = atlasPlacement(node) else {
            return
        }
        guard !refusedForAgent() else { return }
        let v = min(LayerSpeed.range.upperBound,
                    max(LayerSpeed.range.lowerBound, rate))
        let keys = Keyframes(start: LayerSpeed(rate: v))
            .json(rate: atlasTimelineRate(p.comp)) { ["rate": $0.rate] }
        let r = core.layerOp(project: projectId, asset: p.comp.id, "speed",
                             layer: p.layer.id, extra: ["keys": keys])
        if !r.ok {
            flash(r.message, level: .error)
            return
        }
        placementWritten(p.comp.id)
    }

    /// Its speed's keys differ: one number does not say it.
    func atlasSpeedKeyed(_ c: AssetDTO, _ l: LayerDTO) -> Bool {
        let k = c.speedKeys(layer: l.id).keys
        guard let first = k.first?.value.rate else { return false }
        return k.contains { $0.value.rate != first }
    }

    /// The composition written: the stage's panels and player read it
    /// again where it shows there, the graph in any case.
    private func placementWritten(_ comp: String) {
        if stageStack.map({ composedTarget($0.id) }) == comp {
            layersChanged()
        } else {
            reloadAssets()
        }
        reloadAtlas()
    }
}

struct AtlasPlacementPanel: View {
    @Bindable var model: AppModel
    let node: AtlasNodeDTO

    static let wide: CGFloat = 380

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let p = model.atlasPlacement(node) {
                header(p.comp, p.layer)
                Divider()
                rows(p.comp, p.layer, p.source)
            } else {
                Text("This layer is gone.").foregroundStyle(.secondary)
            }
        }
        .padding(14)
        .frame(width: Self.wide, alignment: .leading)
        .background(Color.valtzPanel,
                    in: RoundedRectangle(cornerRadius: 12, style: .continuous))
        .clipShape(RoundedRectangle(cornerRadius: 12, style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: 12, style: .continuous)
                .strokeBorder(Color.accentColor, lineWidth: 1.5)
        }
        .shadow(color: .black.opacity(0.28), radius: 18, y: 6)
        .controlSize(.small)
    }

    private func header(_ c: AssetDTO, _ l: LayerDTO) -> some View {
        HStack(spacing: 8) {
            Image(systemName: "timeline.selection")
                .foregroundStyle(Color.accentColor)
            VStack(alignment: .leading, spacing: 1) {
                Text(c.kind == "audio" ? "Trim + Time" : "Trim + Place")
                    .font(.headline)
                Text(verbatim: "\(l.title) · \(c.name)")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            Spacer()
            Button {
                model.atlasOpenInEditor(c.id)
            } label: {
                Label("Open in Editor", systemImage: "rectangle.and.pencil.and.ellipsis")
            }
            .help("Open the composition in the editor, the Trim panel there")
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

    @ViewBuilder
    private func rows(_ c: AssetDTO, _ l: LayerDTO,
                      _ src: AssetDTO?) -> some View {
        let t = c.time(of: l.id)
        let marks = model.atlasMarkRate(c, l)
        let tl = model.atlasTimelineRate(c)
        let length = model.atlasSourceLength(c, l)
        let timed = length != nil
        let locked = l.locked == true
        let frozen = locked || model.agentHolds
        Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 10,
             verticalSpacing: 9) {
            if let src {
                GridRow {
                    label("Source")
                    Text(verbatim: src.name).lineLimit(1)
                }
            }
            if timed, let length {
                GridRow {
                    label("Mark In")
                    TrimValueField(
                        name: "Mark In",
                        shown: marks.timecode(max(0, t.in)), width: 96,
                        isReset: t.in < 0,
                        commit: { s in
                            guard let n = marks.parseFrame(s) else {
                                return false
                            }
                            model.atlasSetPlacement(
                                node, markIn: min(n, max(0, length - 1)))
                            return true
                        },
                        reset: { model.atlasSetPlacement(node, markIn: -1) })
                        .help("Where its source starts being shown -- a timecode, #frame or seconds")
                }
                GridRow {
                    label("Mark Out")
                    TrimValueField(
                        name: "Mark Out",
                        shown: marks.timecode(t.out >= 0 ? t.out
                                              : max(0, length - 1)),
                        width: 96, isReset: t.out < 0,
                        commit: { s in
                            guard let n = marks.parseFrame(s) else {
                                return false
                            }
                            model.atlasSetPlacement(
                                node, markOut: min(n, max(0, length - 1)))
                            return true
                        },
                        reset: { model.atlasSetPlacement(node, markOut: -1) })
                        .help("Its source's last frame shown")
                }
                GridRow {
                    label("Kept")
                    Text(verbatim: kept(t, length, marks))
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
                GridRow {
                    label("Speed")
                    if model.atlasSpeedKeyed(c, l) {
                        Text("Keyed: changed in the Trim panel")
                            .foregroundStyle(.secondary)
                    } else {
                        let here = c.speedKeys(layer: l.id).keys.first?
                            .value.rate ?? 1
                        HStack(spacing: 6) {
                            TrimValueField(
                                name: "Speed", shown: SpeedRow.display(here),
                                isReset: here == 1,
                                commit: { s in
                                    guard let v = SpeedRow.parse(s) else {
                                        return false
                                    }
                                    model.atlasSetPlacementSpeed(node, v)
                                    return true
                                },
                                reset: { model.atlasSetPlacementSpeed(node, 1) })
                            Menu {
                                ForEach([0.25, 0.5, 1, 1.5, 2, 4], id: \.self) { v in
                                    Button(SpeedRow.display(v)) {
                                        model.atlasSetPlacementSpeed(node, v)
                                    }
                                }
                            } label: {
                                Image(systemName: "chevron.down")
                            }
                            .menuStyle(.borderlessButton)
                            .menuIndicator(.hidden)
                            .fixedSize()
                        }
                        .help("How fast it plays: 2× (or 200%) twice as fast, 0.5× half -- it then takes less, or more, of the timeline")
                    }
                }
            }
            GridRow {
                label("Starts At")
                TrimValueField(
                    name: "Starts At", shown: tl.timecode(t.offset),
                    width: 96, isReset: t.offset == 0,
                    commit: { s in
                        guard let n = tl.parseFrame(s) else { return false }
                        model.atlasSetPlacement(node, offset: n)
                        return true
                    },
                    reset: { model.atlasSetPlacement(node, offset: 0) })
                    .help("Where it starts on \(c.name) -- a timecode, #frame or seconds")
            }
            if !timed {
                GridRow {
                    label("Length")
                    TrimValueField(
                        name: "Length",
                        shown: tl.timecode(max(1, t.duration)), width: 96,
                        isReset: true,
                        commit: { s in
                            guard let n = tl.parseFrame(s), n > 0 else {
                                return false
                            }
                            model.atlasSetPlacement(node, duration: n)
                            return true
                        },
                        reset: {})
                        .help("How long the still is shown on the timeline")
                }
            }
        }
        .disabled(frozen)
        if locked {
            Label("The layer is locked: unlock it to change its time", systemImage: "lock.fill")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private func label(_ s: LocalizedStringKey) -> some View {
        Text(s)
            .foregroundStyle(.secondary)
            .gridColumnAlignment(.trailing)
    }

    /// The span its marks keep: "48 frames · 2.0 s" (a sound's seconds).
    private func kept(_ t: LayerTimeDTO, _ length: Int,
                      _ r: FrameRate) -> String {
        let a = max(0, t.in)
        let b = t.out >= 0 ? t.out : max(0, length - 1)
        let n = max(0, b - a + (r.isMilliseconds ? 0 : 1))
        let secs = String(format: r.isMilliseconds ? "%.3f" : "%.1f",
                          Double(n) / r.fps) + " s"
        if r.isMilliseconds { return secs }
        let frames = String(n)
        return String(localized: "\(frames) frames · \(secs)")
    }
}
