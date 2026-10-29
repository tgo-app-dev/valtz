import AVFoundation
import AVKit
import SwiftUI
import UniformTypeIdentifiers

/// The ATLAS (DESIGN §10a Atlas): the project as a graph, left to right --
/// each asset a card (its thumbnail, played in place, opened large), what
/// made it an OP between it and what it was made from, each layer of a
/// composition an op -- its look and its place -- between what it shows
/// and the composition, a DESTINATION (a timeline, a still, a sound).
/// The core lays it out (Controller::atlas); this draws what is in sight.
/// A layer's link cut takes it off its composition; an asset dropped on a
/// destination -- or placed on one from its menu -- goes on it as a new
/// layer; a LEAF, as the Assets list allows, may be removed.

/// The graph as the core laid it out, in points: each node's frame.
struct AtlasLayout {
    static let assetWidth: CGFloat = 184
    static let opWidth: CGFloat = 156
    static let columnGap: CGFloat = 52
    static let slot: CGFloat = 78
    static let margin: CGFloat = 40

    let graph: AtlasDTO
    let frames: [CGRect]
    let size: CGSize
    /// Each node's place in `graph.nodes`, by its id; an asset's node by
    /// the asset.
    let index: [String: Int]
    let assetNode: [String: Int]
    /// Each edge's ends, as node places.
    let ends: [(Int, Int)]

    init(_ g: AtlasDTO) {
        graph = g
        var f: [CGRect] = []
        f.reserveCapacity(g.nodes.count)
        var idx: [String: Int] = [:]
        var byAsset: [String: Int] = [:]
        var maxX: CGFloat = 0
        for (i, n) in g.nodes.enumerated() {
            let w = n.isAsset ? Self.assetWidth : Self.opWidth
            let x = Self.margin + Self.x(column: n.column)
            let r = CGRect(x: x, y: Self.margin + n.row * Self.slot,
                           width: w, height: n.height * Self.slot)
            f.append(r)
            maxX = max(maxX, r.maxX)
            idx[n.id] = i
            if n.isAsset, let a = n.asset { byAsset[a] = i }
        }
        frames = f
        index = idx
        assetNode = byAsset
        ends = g.edges.compactMap { e in
            guard let a = idx[e.from], let b = idx[e.to] else { return nil }
            return (a, b)
        }
        size = CGSize(width: maxX + Self.margin,
                      height: g.height * Self.slot + 2 * Self.margin)
    }

    /// Where a column starts: assets in the even ones, ops between.
    static func x(column c: Int) -> CGFloat {
        let pairs = CGFloat(c / 2)
        return pairs * (assetWidth + opWidth + 2 * columnGap)
            + (c % 2 == 1 ? assetWidth + columnGap : 0)
    }

    /// The scroll view's content at zoom `z` in a viewport of `v`: the
    /// graph zoomed, at least the viewport.
    func content(zoom z: CGFloat, in v: CGSize) -> CGSize {
        CGSize(width: max(size.width * z, v.width),
               height: max(size.height * z, v.height))
    }

    /// Where the zoomed graph lies in it: in the middle where it is
    /// smaller than the viewport, else from its top left.
    func inset(zoom z: CGFloat, in v: CGSize) -> CGSize {
        CGSize(width: max(0, (v.width - size.width * z) / 2),
               height: max(0, (v.height - size.height * z) / 2))
    }

    /// The nodes whose frames meet `rect`.
    func nodes(in rect: CGRect) -> [Int] {
        frames.indices.filter { frames[$0].intersects(rect) }
    }

    /// The edges in and out of node `i`, as places in `graph.edges`.
    func edges(of i: Int) -> [Int] {
        ends.indices.filter { ends[$0].0 == i || ends[$0].1 == i }
    }
}

// MARK: - The model's side

extension AppModel {
    /// The graph read again when the project wrote something since it
    /// was (its revision), off the main thread.
    func reloadAtlas(force: Bool = false) {
        guard let core, let projectId else {
            atlas = nil
            return
        }
        if !force, let read = atlasRead,
           core.assetsRevision(project: projectId) == read {
            return
        }
        Task { [weak self] in
            let got = await Task.detached(priority: .userInitiated) {
                core.atlas(project: projectId)
            }.value
            guard let self, self.projectId == projectId else { return }
            guard let g = got.graph else {
                if let m = got.message { self.flash(m, level: .error) }
                return
            }
            self.atlasRead = g.revision
            self.atlas = AtlasLayout(g)
            // What it showed may be gone.
            if let e = self.atlasExpanded, self.asset(id: e) == nil {
                self.atlasExpanded = nil
            }
            if let p = self.atlasPlaying, self.asset(id: p) == nil {
                self.atlasStop()
            }
            if let s = self.atlasSelected, self.atlas?.index[s] == nil {
                self.atlasSelected = nil
            }
        }
    }

    /// The screen left: nothing plays on in it.
    func leaveAtlas() {
        atlasStop()
        atlasExpanded = nil
        atlasPlayback?.close()
        atlasPlayback = nil
    }

    /// An asset that may go -- a LEAF (nothing made from it, on no layer,
    /// not the project's: the core's rule) that is not on the stage or in
    /// the prompt either, nor being made: the Assets list's rule.
    func atlasRemovable(_ node: AtlasNodeDTO) -> Bool {
        guard node.leaf == true, let id = node.asset, !agentHolds,
              task(of: id) == nil else { return false }
        return !assetsInUse.contains(id)
    }

    func atlasRemove(_ id: String) {
        guard !refusedForAgent() else { return }
        if atlasPlaying == id { atlasStop() }
        if atlasExpanded == id { atlasExpanded = nil }
        removeAsset(id)
    }

    /// A layer's link to its composition may be cut -- the layer taken
    /// off it -- unless it is locked, or the composition's last.
    func atlasCanDisconnect(_ node: AtlasNodeDTO) -> Bool {
        guard node.type == "layer", !agentHolds, let into = node.into,
              let comp = asset(id: into) else { return false }
        // Its only layer too: a blank takes its place (core remove_layer).
        guard let l = (comp.layers ?? []).first(where: {
            $0.id == node.layer
        }) else {
            return false
        }
        return !l.locked && !l.isEmpty
    }

    /// Cut: the layer off its composition (core remove_layer), one
    /// command, undone as any.
    func atlasDisconnect(_ node: AtlasNodeDTO) {
        guard node.type == "layer", let into = node.into,
              let layer = node.layer, let core, let projectId else { return }
        guard !refusedForAgent() else { return }
        let only = asset(id: into)?.layers?.count == 1
        let r = core.layerOp(project: projectId, asset: into, "remove",
                             layer: layer)
        if !r.ok {
            flash(r.message, level: .error)
            return
        }
        if atlasSelected == node.id { atlasSelected = nil }
        if stageStack.map({ composedTarget($0.id) }) == into {
            let was = selectedLayer == layer
            if was { selectedLayer = "" }
            layersChanged()
            // The only layer gone: the blank in its place is selected.
            if was, only, let blank = asset(id: into)?.layers?.first {
                selectedLayer = blank.id
            }
        } else {
            reloadAssets()
        }
    }

    /// The compositions `id` may go on as a new layer: a picture, a clip,
    /// a markup or a composition on a timeline or a still -- a sound on a
    /// timeline or a composition of sound, a clip's sound there too --
    /// never itself; the core refuses the rest (a recursion).
    func atlasDestinations(for id: String) -> [AssetDTO] {
        guard let a = asset(id: id), a.kind != "text" else { return [] }
        return assets.filter { d in
            guard d.isComposition, d.id != id, task(of: d.id) == nil
            else { return false }
            if d.kind == "audio" {
                return a.kind == "audio" || a.kind == "video"
            }
            if d.assetClass == "still" { return a.kind != "audio" }
            return true
        }
    }

    /// Placed on `onto` as a new layer, on top (core instantiate) -- where
    /// the player is when it is the stage's, else from its start.
    func atlasPlace(_ id: String, onto: String) {
        guard let core, let projectId, id != onto else { return }
        guard !refusedForAgent() else { return }
        var extra: [String: Any] = ["asset": id, "onto": onto]
        let onStage = stageStack.map { composedTarget($0.id) } == onto
        if onStage && clipOnStage { extra["offset"] = videoFrame }
        if onStage && pagedOnStage { extra["offset"] = stagePage }
        let r = core.assetOp(project: projectId, "instantiate", extra)
        if !r.ok {
            flash(r.message, level: .error)
            return
        }
        if onStage { layersChanged() } else { reloadAssets() }
    }

    /// Opened in the editor as the Assets list opens it: a text in the
    /// Prompt Editor, a composition made active, anything else viewed.
    func atlasOpenInEditor(_ id: String) {
        guard let a = asset(id: id) else { return }
        show(.editor)
        if a.kind == "text" {
            openTextAsset(a)
            return
        }
        retreatFromEditor()
        if let t = task(of: a.id) {
            showTask(t.job)
        } else if a.isComposition {
            viewedAsset = nil
            viewReturn = nil
            setActive(a)
        } else {
            viewAsset(a)
        }
    }

    /// What plays in place: a clip, a sound -- a timeline or a composition
    /// of sound as the core renders it (made once, kept).
    func atlasPlayable(_ a: AssetDTO) -> Bool {
        if a.isComposition { return a.isTimeline }
        return (a.kind == "video" || a.kind == "audio") && a.url != nil
    }

    func atlasPlay(_ id: String) {
        atlasStop()
        guard let a = asset(id: id), atlasPlayable(a), let core,
              let projectId else { return }
        atlasPlaying = id
        if !a.isComposition, let url = a.url {
            startAtlasPlayer(url)
            return
        }
        atlasPreparing = true
        Task { [weak self] in
            let url = await Task.detached(priority: .userInitiated) {
                core.renderedPath(project: projectId, asset: id)
            }.value
            guard let self, self.atlasPlaying == id else { return }
            self.atlasPreparing = false
            guard let url else {
                self.atlasPlaying = nil
                self.flash(String(localized: "\(a.name) could not be drawn to play."), level: .error)
                return
            }
            self.startAtlasPlayer(url)
        }
    }

    private func startAtlasPlayer(_ url: URL) {
        let p = AVPlayer(url: url)
        atlasPlayer = p
        // Played to its end: the card shows its picture again.
        atlasEndWatch = NotificationCenter.default.addObserver(
            forName: AVPlayerItem.didPlayToEndTimeNotification,
            object: p.currentItem, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.atlasStop() }
            }
        p.play()
    }

    func atlasStop() {
        atlasPlayer?.pause()
        atlasPlayer = nil
        atlasPlaying = nil
        atlasPreparing = false
        if let w = atlasEndWatch {
            NotificationCenter.default.removeObserver(w)
            atlasEndWatch = nil
        }
    }

    /// What the title bar covers of the scroll view: its visible rect
    /// runs under it (from -52 at the top), its container -- the
    /// viewport -- does not, nor does ScrollPosition count it.
    var atlasInsets: CGSize {
        CGSize(width: max(0, atlasVisible.width - atlasViewport.width),
               height: max(0, atlasVisible.height - atlasViewport.height))
    }

    /// A point of the scroll view's frame (a pinch's) in the viewport.
    func atlasViewportPoint(_ p: CGPoint) -> CGPoint {
        CGPoint(x: p.x - atlasInsets.width, y: p.y - atlasInsets.height)
    }

    /// The window's title bar -- its toolbar -- high: what the content
    /// view runs under (the full-size content's top).
    var titlebarHeight: CGFloat {
        guard let w = editorWindow else { return 52 }
        return max(0, w.frame.height - w.contentLayoutRect.height)
    }

    /// The graph's own point (unzoomed) under `p`, a point of the
    /// viewport.
    func atlasGraphPoint(at p: CGPoint) -> CGPoint {
        let z = atlasZoom
        let i = atlas?.inset(zoom: z, in: atlasViewport) ?? .zero
        // Where it is scrolled to, as ScrollPosition counts it.
        let x = atlasVisible.minX + atlasInsets.width
        let y = atlasVisible.minY + atlasInsets.height
        return CGPoint(x: (x + p.x - i.width) / z,
                       y: (y + p.y - i.height) / z)
    }

    /// Zoomed by `f` about the viewport's middle: the zoom buttons'.
    func atlasZoom(by f: CGFloat) {
        setAtlasZoom(atlasZoom * f)
    }

    /// Zoomed to `z` (20% to 200%), the graph's `point` -- else the one
    /// under `at`, else the viewport's middle -- kept under `at`: where a
    /// pinch's fingers are, the middle for the buttons. Scrolled in the
    /// same change, so nothing slides off to the top left first.
    func setAtlasZoom(_ z: CGFloat, at: CGPoint? = nil,
                      point: CGPoint? = nil) {
        let z1 = min(2, max(0.2, z))
        guard let g = atlas else {
            atlasZoom = z1
            return
        }
        let v = atlasViewport
        let p = at ?? CGPoint(x: v.width / 2, y: v.height / 2)
        let c = point ?? atlasGraphPoint(at: p)
        atlasZoom = z1
        let i = g.inset(zoom: z1, in: v)
        let size = g.content(zoom: z1, in: v)
        let o = CGPoint(
            x: min(max(0, c.x * z1 + i.width - p.x),
                   max(0, size.width - v.width)),
            y: min(max(0, c.y * z1 + i.height - p.y),
                   max(0, size.height - v.height)))
        atlasPosition.scrollTo(point: o)
        // What comes into sight, at once: its cards drawn with it.
        let pad = atlasInsets
        atlasVisible.origin = CGPoint(x: o.x - pad.width,
                                      y: o.y - pad.height)
    }

    /// The whole graph in sight, in the middle -- at most actual size.
    func atlasFit() {
        guard let g = atlas, g.size.width > 0, g.size.height > 0
        else { return }
        let v = atlasViewport
        atlasZoom = min(2, max(0.2, min(1, v.width / g.size.width,
                                        v.height / g.size.height)))
        atlasPosition.scrollTo(point: .zero)
        let pad = atlasInsets
        atlasVisible.origin = CGPoint(x: -pad.width, y: -pad.height)
    }

    /// An op's name, as the menus say what made an asset.
    static func atlasOpTitle(_ op: String) -> String {
        switch op {
        case "generate-image": String(localized: "Generate Picture")
        case "edit-image": String(localized: "Edit Picture")
        case "generate-video": String(localized: "Generate Clip")
        case "generate-audio": String(localized: "Generate Song")
        case "generate-speech": String(localized: "Speak")
        case "enhance-prompt": String(localized: "Enhance Prompt")
        case "transcribe-audio": String(localized: "Transcribe")
        case "summarize-video": String(localized: "Summarize Video")
        case "separate-audio": String(localized: "Separate")
        case "upscale-video", "upscale-image":
            String(localized: "Render Upscaled")
        case "flatten": String(localized: "Flatten")
        default: op
        }
    }

    static func atlasOpSymbol(_ op: String) -> String {
        switch op {
        case "generate-image": "photo.badge.plus"
        case "edit-image": "wand.and.stars"
        case "generate-video": "film"
        case "generate-audio": "music.note"
        case "generate-speech": "waveform"
        case "enhance-prompt": "sparkles"
        case "transcribe-audio": "text.bubble"
        case "summarize-video": "text.below.photo"
        case "separate-audio": "slider.vertical.3"
        case "upscale-video", "upscale-image":
            "arrow.up.left.and.arrow.down.right"
        case "flatten": "square.stack.3d.down.forward"
        default: "gearshape"
        }
    }

    /// An input's part in what it went into.
    static func atlasRole(_ role: String) -> String {
        switch role {
        case "prompt": String(localized: "Prompt")
        case "base": String(localized: "Base")
        case "reference": String(localized: "Reference")
        case "first": String(localized: "First frame")
        case "last": String(localized: "Last frame")
        case "continue": String(localized: "Continued")
        case "voice": String(localized: "Voice")
        default: role
        }
    }
}

// MARK: - The screen

struct AtlasView: View {
    @Bindable var model: AppModel
    private struct Shown: Identifiable {
        let i: Int
        let id: String
    }
    /// A pinch under way: the zoom it began at, where the fingers were
    /// (in the viewport), the graph's point there -- kept under them.
    private struct Pinch {
        let zoom: CGFloat
        let at: CGPoint
        let point: CGPoint
    }
    @State private var pinch: Pinch?

    var body: some View {
        ZStack {
            if let g = model.atlas, !g.graph.nodes.isEmpty {
                graph(g)
            } else if model.atlas != nil {
                empty
            } else {
                ProgressView().controlSize(.small)
            }
            if let id = model.atlasExpanded, let a = model.asset(id: id) {
                AtlasExpanded(model: model, asset: a)
                    .transition(.opacity)
            }
        }
        .overlay(alignment: .bottomTrailing) {
            if model.atlas.map({ !$0.graph.nodes.isEmpty }) == true,
               model.atlasExpanded == nil {
                zoomBar.padding(14)
            }
        }
        // The title bar's ground: the graph scrolled up goes UNDER the
        // bar, as the editor's stage stays below it -- the window's
        // toolbar has no background of its own (RootView), and a scroll
        // view draws its content into the safe area above it.
        // The bar's own height, from the window (a safe-area inset read
        // inside an overlay is none); drawn from the window's top.
        .overlay(alignment: .top) {
            GeometryReader { g in
                Color(nsColor: .windowBackgroundColor)
                    .frame(width: g.size.width,
                           height: model.titlebarHeight)
                    .offset(y: -g.frame(in: .global).minY)
            }
            .allowsHitTesting(false)
        }
        .background(Color(nsColor: .windowBackgroundColor))
        .onAppear { model.reloadAtlas(force: true) }
        .onDisappear { model.atlasStop() }
    }

    private var empty: some View {
        VStack(spacing: 8) {
            Image(systemName: "point.3.connected.trianglepath.dotted")
                .font(.system(size: 34))
                .foregroundStyle(.tertiary)
            Text("Nothing in the project yet")
                .font(.headline)
            Text("What you import, make and put together shows here, each thing beside what it was made from.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
                .frame(maxWidth: 380)
        }
    }

    private func graph(_ g: AtlasLayout) -> some View {
        let z = model.atlasZoom
        let v = model.atlasVisible
        let inset = g.inset(zoom: z, in: model.atlasViewport)
        let content = g.content(zoom: z, in: model.atlasViewport)
        // In the graph's own points, with room to scroll into.
        let seen = CGRect(x: (v.minX - inset.width) / z,
                          y: (v.minY - inset.height) / z,
                          width: v.width / z, height: v.height / z)
            .insetBy(dx: -240, dy: -240)
            .intersection(CGRect(origin: .zero, size: g.size))
        let shown = g.nodes(in: seen)
        let lit = model.atlasSelected.flatMap { g.index[$0] }
        // Each card LAID OUT at the zoom, not scaled after: its type, its
        // lines and its picture drawn at the size they are seen (a scale
        // effect zoomed their bitmaps).
        return ScrollView([.horizontal, .vertical]) {
            ZStack(alignment: .topLeading) {
                Color.clear
                    .frame(width: content.width, height: content.height)
                    .contentShape(Rectangle())
                    .onTapGesture { model.atlasSelected = nil }
                if !seen.isNull {
                    AtlasEdges(layout: g, rect: seen, zoom: z, lit: lit,
                               labels: z >= 0.6)
                        .frame(width: seen.width * z,
                               height: seen.height * z)
                        .offset(x: inset.width + seen.minX * z,
                                y: inset.height + seen.minY * z)
                        .allowsHitTesting(false)
                }
                // By the node's id: a graph read again keeps each card
                // its own (its picture, its hover).
                ForEach(shown.map { Shown(i: $0, id: g.graph.nodes[$0].id) })
                { s in
                    let f = g.frames[s.i]
                    node(g, s.i, zoom: z)
                        .frame(width: f.width * z, height: f.height * z)
                        .offset(x: inset.width + f.minX * z,
                                y: inset.height + f.minY * z)
                }
            }
            .frame(width: content.width, height: content.height,
                   alignment: .topLeading)
        }
        .scrollPosition($model.atlasPosition)
        .onScrollGeometryChange(for: CGRect.self) { $0.visibleRect }
            action: { _, r in model.atlasVisible = r }
        .onScrollGeometryChange(for: CGSize.self) { $0.containerSize }
            action: { _, s in model.atlasViewport = s }
        // About the fingers: the graph's point under them stays there.
        .gesture(MagnifyGesture()
            .onChanged { m in
                let at = model.atlasViewportPoint(m.startLocation)
                let p = pinch ?? Pinch(
                    zoom: model.atlasZoom, at: at,
                    point: model.atlasGraphPoint(at: at))
                pinch = p
                model.setAtlasZoom(p.zoom * m.magnification, at: p.at,
                                   point: p.point)
            }
            .onEnded { _ in pinch = nil })
    }

    @ViewBuilder
    private func node(_ g: AtlasLayout, _ i: Int,
                      zoom z: CGFloat) -> some View {
        let n = g.graph.nodes[i]
        if n.isAsset {
            if let id = n.asset, let a = model.asset(id: id) {
                AtlasAssetNode(model: model, node: n, asset: a, scale: z)
            }
        } else {
            AtlasOpNode(model: model, node: n, scale: z)
        }
    }

    /// Zoom: out, the zoom (a click: 100%), in, the whole graph in view.
    private var zoomBar: some View {
        HStack(spacing: 2) {
            Button { model.atlasZoom(by: 1 / 1.25) } label: {
                Image(systemName: "minus.magnifyingglass")
            }
            .help("Zoom Out")
            Button { model.setAtlasZoom(1) } label: {
                Text(verbatim: "\(Int((model.atlasZoom * 100).rounded()))%")
                    .monospacedDigit()
                    .frame(minWidth: 40)
            }
            .help("Actual Size")
            Button { model.atlasZoom(by: 1.25) } label: {
                Image(systemName: "plus.magnifyingglass")
            }
            .help("Zoom In")
            Button { model.atlasFit() } label: {
                Image(systemName: "arrow.up.left.and.down.right.magnifyingglass")
            }
            .help("Zoom to Fit")
        }
        .buttonStyle(.borderless)
        .padding(.horizontal, 8)
        .padding(.vertical, 5)
        .glassEffect(.regular, in: Capsule())
    }
}

/// The edges in sight: a curve from an output's right edge to an input's
/// left, lit with the node selected; a note dashed; an input's part
/// written by its end.
private struct AtlasEdges: View {
    let layout: AtlasLayout
    /// In the graph's own points; drawn at `zoom`, as the cards are.
    let rect: CGRect
    let zoom: CGFloat
    let lit: Int?
    let labels: Bool

    var body: some View {
        Canvas { ctx, _ in
            let z = zoom
            ctx.translateBy(x: -rect.minX * z, y: -rect.minY * z)
            for (k, e) in layout.ends.enumerated() {
                let a = layout.frames[e.0]
                let b = layout.frames[e.1]
                let box = CGRect(x: min(a.maxX, b.minX),
                                 y: min(a.midY, b.midY),
                                 width: abs(b.minX - a.maxX),
                                 height: abs(b.midY - a.midY))
                    .insetBy(dx: -40, dy: -40)
                guard box.intersects(rect) else { continue }
                let p0 = CGPoint(x: a.maxX * z, y: a.midY * z)
                let p1 = CGPoint(x: b.minX * z, y: b.midY * z)
                let edge = layout.graph.edges[k]
                let dx = max(36 * z, (p1.x - p0.x) / 2)
                let c1 = CGPoint(x: p0.x + dx, y: p0.y)
                let c2 = CGPoint(x: p1.x - dx, y: p1.y)
                var path = Path()
                path.move(to: p0)
                path.addCurve(to: p1, control1: c1, control2: c2)
                let on = lit == e.0 || lit == e.1
                let placing = edge.kind == "layer" || edge.kind == "into"
                let colour: Color = on ? .accentColor
                    : placing ? Color.accentColor.opacity(0.45)
                    : Color.secondary.opacity(0.55)
                let style = StrokeStyle(
                    lineWidth: max(0.75, (on ? 2.5 : 1.5) * z),
                    lineCap: .round,
                    dash: edge.kind == "note" ? [4 * z, 4 * z] : [])
                ctx.stroke(path, with: .color(colour), style: style)
                // The ends: a dot where it leaves, where it arrives.
                let r = max(1.5, 3 * z)
                for p in [p0, p1] {
                    ctx.fill(Path(ellipseIn: CGRect(x: p.x - r, y: p.y - r,
                                                    width: 2 * r,
                                                    height: 2 * r)),
                             with: .color(colour))
                }
                if labels, let role = edge.role, !role.isEmpty {
                    let t = Text(AppModel.atlasRole(role))
                        .font(.system(size: 9.5 * z, weight: .medium))
                        .foregroundStyle(on ? Color.accentColor : .secondary)
                    // Along its own curve, before the end: inputs
                    // meeting at one dot drew their words on one another.
                    let at = Self.point(0.62, p0, c1, c2, p1)
                    ctx.draw(t, at: CGPoint(x: at.x, y: at.y - 3 * z),
                             anchor: .bottom)
                }
            }
        }
    }

    /// The cubic's point at `t`.
    private static func point(_ t: CGFloat, _ a: CGPoint, _ b: CGPoint,
                              _ c: CGPoint, _ d: CGPoint) -> CGPoint {
        let u = 1 - t
        let w = (u * u * u, 3 * u * u * t, 3 * u * t * t, t * t * t)
        return CGPoint(x: w.0 * a.x + w.1 * b.x + w.2 * c.x + w.3 * d.x,
                       y: w.0 * a.y + w.1 * b.y + w.2 * c.y + w.3 * d.y)
    }
}

// MARK: - An asset's card

struct AtlasAssetNode: View {
    @Bindable var model: AppModel
    let node: AtlasNodeDTO
    let asset: AssetDTO
    /// The atlas's zoom: every measure of the card times it -- laid out
    /// at the size seen, never a bitmap scaled.
    let scale: CGFloat
    @State private var hovering = false
    @State private var image: CGImage?
    @State private var targeted = false
    @State private var lastClick = Date.distantPast

    private var selected: Bool { model.atlasSelected == node.id }
    private var playing: Bool { model.atlasPlaying == asset.id }
    private var destination: String? { node.destination }
    private func u(_ v: CGFloat) -> CGFloat { v * scale }
    /// Its picture's pixels: twice as many past actual size.
    private var pixels: Int { scale > 1 ? 736 : 368 }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            preview
                .frame(height: u(104))
                .frame(maxWidth: .infinity)
                .clipped()
            caption
                .padding(.horizontal, u(8))
                .padding(.vertical, u(6))
        }
        .background(Color.valtzPanel)
        .clipShape(RoundedRectangle(cornerRadius: u(10), style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: u(10), style: .continuous)
                .strokeBorder(border, lineWidth: max(0.5, u(
                    selected || targeted ? 2.5
                        : destination != nil ? 1.5 : 1)))
        }
        .overlay(alignment: .topLeading) { badges.padding(u(6)) }
        .overlay(alignment: .topTrailing) {
            if hovering && !playing {
                Button { model.atlasExpanded = asset.id } label: {
                    Image(systemName: "arrow.up.left.and.arrow.down.right")
                        .font(.system(size: u(10), weight: .semibold))
                        .padding(u(5))
                        .background(.ultraThinMaterial, in: Circle())
                }
                .buttonStyle(.plain)
                .help("Open Large")
                .padding(u(6))
            }
        }
        .shadow(color: .black.opacity(selected ? 0.25 : 0.12),
                radius: u(selected ? 8 : 4), y: u(2))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture { click() }
        .contextMenu { menu }
        .onDrag {
            NSItemProvider(object: (AppModel.assetPrefix + asset.id)
                as NSString)
        }
        // A destination takes an asset dropped on it.
        .onDrop(of: destination != nil ? [UTType.text] : [],
                isTargeted: $targeted) { providers in
            drop(providers)
        }
        .help(Text(verbatim: asset.name))
        .task(id: "\(asset.id)-\(asset.modified)-\(asset.head)-\(pixels)") {
            await loadPicture()
        }
    }

    private var border: Color {
        if selected || targeted { return .accentColor }
        if destination != nil { return Color.accentColor.opacity(0.6) }
        return Color(nsColor: .separatorColor)
    }

    // A click selects -- its edges lit -- two within the double-click
    // interval open it large (told by time: a double-click gesture beside
    // the single one holds every click back).
    private func click() {
        let now = Date()
        if now.timeIntervalSince(lastClick) < NSEvent.doubleClickInterval {
            model.atlasExpanded = asset.id
            lastClick = .distantPast
        } else {
            model.atlasSelected = node.id
            lastClick = now
        }
    }

    @ViewBuilder
    private var preview: some View {
        ZStack {
            if asset.kind == "text" {
                Color(nsColor: .textBackgroundColor)
                Text(verbatim: String((asset.text ?? asset.name).prefix(240)))
                    .font(.custom("Georgia", size: u(11)))
                    .foregroundStyle(.primary)
                    .lineLimit(6)
                    .padding(u(8))
                    .frame(maxWidth: .infinity, maxHeight: .infinity,
                           alignment: .topLeading)
            } else {
                Color.black.opacity(asset.kind == "audio" ? 0.75 : 0.9)
                if playing, let p = model.atlasPlayer, asset.kind != "audio" {
                    AtlasPlayerView(player: p, controls: false)
                } else if let image {
                    Image(decorative: image, scale: 2)
                        .resizable()
                        .interpolation(.high)
                        .aspectRatio(contentMode: .fit)
                } else {
                    Image(systemName: Self.symbol(asset))
                        .font(.system(size: u(26)))
                        .foregroundStyle(.white.opacity(0.35))
                }
            }
            if playing && model.atlasPreparing {
                ProgressView().controlSize(.small)
            }
            if model.atlasPlayable(asset) && (hovering || playing)
                && !(playing && model.atlasPreparing) {
                Button {
                    if playing { model.atlasStop() }
                    else { model.atlasPlay(asset.id) }
                } label: {
                    Image(systemName: playing ? "stop.fill" : "play.fill")
                        .font(.system(size: u(13), weight: .semibold))
                        .foregroundStyle(.white)
                        .frame(width: u(32), height: u(32))
                        .background(.black.opacity(0.45), in: Circle())
                }
                .buttonStyle(.plain)
                .help(playing ? Text("Stop") : Text("Play Here"))
            }
        }
        .overlay(alignment: .bottom) {
            if playing, let p = model.atlasPlayer {
                AtlasProgress(player: p, height: max(1, u(2)))
            }
        }
    }

    private var caption: some View {
        VStack(alignment: .leading, spacing: u(2)) {
            HStack(spacing: u(5)) {
                Image(systemName: Self.symbol(asset))
                    .font(.system(size: u(10.5)))
                    .foregroundStyle(destination != nil ? Color.accentColor
                                     : .secondary)
                Text(verbatim: asset.name)
                    .font(.system(size: u(12), weight: .medium))
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer(minLength: 0)
                if hovering && model.atlasRemovable(node) {
                    Button { model.atlasRemove(asset.id) } label: {
                        Image(systemName: "trash")
                            .font(.system(size: u(10.5)))
                    }
                    .buttonStyle(.borderless)
                    .help("Remove from Project: nothing is made from it, and nothing shows it")
                }
            }
            Text(verbatim: Self.subtitle(asset))
                .font(.system(size: u(10.5)))
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
    }

    @ViewBuilder
    private var badges: some View {
        HStack(spacing: u(4)) {
            if let d = destination {
                Text(Self.destinationName(d))
                    .badgeCapsule(Color.accentColor, scale: scale)
            }
            if node.project == true {
                Text("Project").badgeCapsule(.secondary, scale: scale)
            }
            if let t = model.task(of: asset.id) {
                Text(verbatim: t.badge).badgeCapsule(.orange, scale: scale)
            }
        }
    }

    @ViewBuilder
    private var menu: some View {
        Button("Open in Editor") { model.atlasOpenInEditor(asset.id) }
        Button("Open Large") { model.atlasExpanded = asset.id }
        if model.atlasPlayable(asset) {
            Button(playing ? "Stop" : "Play Here") {
                if playing { model.atlasStop() }
                else { model.atlasPlay(asset.id) }
            }
        }
        let onto = model.atlasDestinations(for: asset.id)
        if !onto.isEmpty {
            Menu("Place On") {
                ForEach(onto) { d in
                    Button(d.name) { model.atlasPlace(asset.id, onto: d.id) }
                }
            }
            .disabled(model.agentHolds)
        }
        Divider()
        Button("Remove from Project") { model.atlasRemove(asset.id) }
            .disabled(!model.atlasRemovable(node))
    }

    /// An asset dropped on a destination: on it, a new layer.
    private func drop(_ providers: [NSItemProvider]) -> Bool {
        guard destination != nil, let p = providers.first else {
            return false
        }
        let onto = asset.id
        _ = p.loadObject(ofClass: NSString.self) { s, _ in
            guard let s = s as? String else { return }
            Task { @MainActor in
                guard let id = AppModel.draggedAsset(s) else { return }
                model.atlasPlace(id, onto: onto)
            }
        }
        return true
    }

    private func loadPicture() async {
        guard let core = model.core, let project = model.projectId,
              asset.kind != "text" else { return }
        let px = pixels
        let key = "\(asset.id)-\(asset.modified)-\(asset.head)-\(px)"
        if let kept = model.atlasPictures[key] {
            image = kept
            return
        }
        // The one shown meanwhile stays until this one comes.
        let got: CGImage?
        if asset.kind == "audio", let url = asset.url {
            got = await AudioWaveform.image(url, width: px,
                                            height: px * 208 / 368)
        } else {
            let id = asset.id
            got = await Task.detached(priority: .utility) {
                core.thumbnail(project: project, asset: id, maxPixels: px)
                    .flatMap { AppModel.loadImage($0) }
            }.value
        }
        guard !Task.isCancelled, let got else { return }
        image = got
        model.keepAtlasPicture(key, got)
    }

    static func symbol(_ a: AssetDTO) -> String {
        if a.isComposition {
            return a.kind == "audio" ? "waveform.path"
                : a.isTimeline ? "film.stack" : "square.on.square"
        }
        if a.isMarkupAsset { return "pencil.and.scribble" }
        switch a.kind {
        case "video": return "film"
        case "audio": return "waveform"
        case "image": return "photo"
        case "text": return a.isPrompt ? "text.quote" : "doc.text"
        default: return "doc"
        }
    }

    static func destinationName(_ d: String) -> String {
        switch d {
        case "timeline": String(localized: "Timeline")
        case "still": String(localized: "Still")
        case "sound": String(localized: "Sound")
        default: d
        }
    }

    /// "Picture · 1024 × 768", "Clip · 5.2 s", "Timeline · 3 layers".
    static func subtitle(_ a: AssetDTO) -> String {
        if a.isComposition {
            let count = (a.layers ?? []).filter { !$0.isEmpty }.count
            let n = String(count)
            let layers = count == 1 ? String(localized: "1 layer")
                : String(localized: "\(n) layers")
            if a.isPaged {
                let p = String(a.pageCount)
                return String(localized: "\(p) pages · \(layers)")
            }
            if a.isTimeline, let len = a.length, let r = a.compositionRate {
                let s = InspectorView.duration(Double(len) / r.fps)
                return "\(s) · \(layers)"
            }
            return layers
        }
        if a.isPrompt { return String(localized: "Prompt") }
        if a.isMarkupAsset { return String(localized: "Markup") }
        guard let info = a.info else {
            return a.kind == "text" ? String(localized: "Text") : ""
        }
        switch a.kind {
        case "video":
            let s = InspectorView.duration(info.pictureSeconds ?? 0)
            let size = "\(info.frame.w) × \(info.frame.h)"
            return String(localized: "Clip · \(s) · \(size)")
        case "audio":
            let s = InspectorView.duration(info.seconds ?? 0)
            return String(localized: "Sound · \(s)")
        default:
            let size = "\(info.frame.w) × \(info.frame.h)"
            return String(localized: "Picture · \(size)")
        }
    }
}

extension AppModel {
    /// A card's picture kept as it scrolls out of sight and back -- a few
    /// thousand at most.
    func keepAtlasPicture(_ key: String, _ image: CGImage) {
        if atlasPictures.count > 3000 { atlasPictures.removeAll() }
        atlasPictures[key] = image
    }
}

private extension Text {
    func badgeCapsule(_ tint: Color, scale k: CGFloat) -> some View {
        font(.system(size: 9.5 * k, weight: .semibold))
            .foregroundStyle(.white)
            .padding(.horizontal, 6 * k)
            .padding(.vertical, 2 * k)
            .background(tint.opacity(0.85), in: Capsule())
    }
}

// MARK: - An op's card

struct AtlasOpNode: View {
    @Bindable var model: AppModel
    let node: AtlasNodeDTO
    /// The atlas's zoom, as an asset's card takes it.
    let scale: CGFloat
    @State private var hovering = false

    private func u(_ v: CGFloat) -> CGFloat { v * scale }

    private var selected: Bool { model.atlasSelected == node.id }
    private var isLayer: Bool { node.type == "layer" }
    private var composition: AssetDTO? { node.into.flatMap(model.asset(id:)) }
    private var layer: LayerDTO? {
        composition?.layers?.first { $0.id == node.layer }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: u(3)) {
            HStack(spacing: u(5)) {
                Image(systemName: symbol)
                    .font(.system(size: u(11)))
                    .foregroundStyle(isLayer ? Color.accentColor : .secondary)
                Text(verbatim: title)
                    .font(.system(size: u(11.5), weight: .semibold))
                    .lineLimit(1)
                Spacer(minLength: 0)
                if isLayer && (hovering || selected) {
                    Button { model.atlasDisconnect(node) } label: {
                        Image(systemName: "scissors")
                            .font(.system(size: u(10.5), weight: .semibold))
                    }
                    .buttonStyle(.borderless)
                    .disabled(!model.atlasCanDisconnect(node))
                    .help(disconnectHelp)
                }
            }
            Text(verbatim: detail)
                .font(.system(size: u(10.5)))
                .foregroundStyle(.secondary)
                .lineLimit(isLayer ? 1 : 2)
            if isLayer { lookIcons }
        }
        .padding(.horizontal, u(8))
        .padding(.vertical, u(7))
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .background(Color.valtzPanel.opacity(0.85),
                    in: RoundedRectangle(cornerRadius: u(8),
                                         style: .continuous))
        .overlay {
            RoundedRectangle(cornerRadius: u(8), style: .continuous)
                .strokeBorder(selected ? Color.accentColor
                              : Color(nsColor: .separatorColor),
                              style: StrokeStyle(
                                  lineWidth: max(0.5, u(selected ? 2 : 1)),
                                  dash: node.note == true
                                      ? [u(3), u(3)] : []))
        }
        .padding(.vertical, u(6))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture { model.atlasSelected = node.id }
        .contextMenu {
            if isLayer {
                Button(disconnectTitle) { model.atlasDisconnect(node) }
                    .disabled(!model.atlasCanDisconnect(node))
                if let c = composition {
                    Button("Open \(c.name) in Editor") {
                        model.atlasOpenInEditor(c.id)
                    }
                }
            } else if let into = node.into {
                Button("Open Large") { model.atlasExpanded = into }
            }
        }
        .help(Text(verbatim: isLayer ? "\(title) — \(detail)" : detail))
    }

    private var symbol: String {
        if isLayer {
            return composition?.kind == "audio" ? "slider.vertical.3"
                : "square.3.layers.3d.down.right"
        }
        return AppModel.atlasOpSymbol(node.op ?? "")
    }

    /// A layer's: what it does to what it shows -- a timeline's or a
    /// still's look and place, a sound's level and time; a recipe's: its
    /// op.
    private var title: String {
        guard isLayer else { return AppModel.atlasOpTitle(node.op ?? "") }
        return composition?.kind == "audio"
            ? String(localized: "Sound + Time")
            : String(localized: "Adjust + Place")
    }

    /// The layer and where it lies; a recipe's model, or a note's word.
    private var detail: String {
        if isLayer {
            var parts = [layer?.title ?? String(localized: "Layer")]
            if let c = composition {
                let first = node.offset ?? 0
                let run = node.duration ?? 0
                if c.isPaged {
                    let a = String(first + 1)
                    let b = String(run > 0 ? first + run : c.pageCount)
                    parts.append(a == b ? String(localized: "Page \(a)")
                                 : String(localized: "Pages \(a)–\(b)"))
                } else if c.isTimeline, first > 0 || run > 0 {
                    let a = String(first)
                    parts.append(run > 0
                        ? String(localized: "Frames \(a)–\(String(first + run))")
                        : String(localized: "From frame \(a)"))
                }
            }
            return parts.joined(separator: " · ")
        }
        if node.note == true {
            return String(localized: "A flat copy, as it was drawn: its original may go")
        }
        if let m = node.model, !m.isEmpty { return model.modelName(m) }
        return ""
    }

    /// What the layer does, small: its look's parts, keyed, its mask, its
    /// state.
    private var lookIcons: some View {
        HStack(spacing: u(5)) {
            ForEach(node.look ?? [], id: \.self) { l in
                if let s = Self.lookSymbol(l) {
                    Image(systemName: s)
                        .font(.system(size: u(9.5)))
                        .foregroundStyle(.secondary)
                        .help(Self.lookName(l))
                }
            }
            if (node.look ?? []).isEmpty {
                Text("As it is")
                    .font(.system(size: u(9.5)))
                    .foregroundStyle(.tertiary)
            }
        }
    }

    private var disconnectTitle: String {
        let name = composition?.name ?? ""
        return String(localized: "Disconnect: Remove from \(name)")
    }

    private var disconnectHelp: String {
        if layer?.locked == true {
            return String(localized: "The layer is locked: unlock it to take it off")
        }
        if (composition?.layers ?? []).count <= 1 {
            let name = composition?.name ?? ""
            return String(localized: "Disconnect: Remove from \(name), an empty layer in its place")
        }
        return disconnectTitle
    }

    static func lookSymbol(_ l: String) -> String? {
        switch l {
        case "adjust": "slider.horizontal.3"
        case "place": "crop"
        case "turn": "rotate.right"
        case "keyed": "diamond"
        case "speed": "gauge.with.dots.needle.67percent"
        case "sound": "speaker.wave.2"
        case "timed": "clock"
        case "mask": "circle.lefthalf.filled"
        case "hidden": "eye.slash"
        case "locked": "lock.fill"
        case "pinned": "pin.fill"
        default: nil
        }
    }

    static func lookName(_ l: String) -> String {
        switch l {
        case "adjust": String(localized: "Adjusted")
        case "place": String(localized: "Placed or cropped")
        case "turn": String(localized: "Turned")
        case "keyed": String(localized: "Keyed")
        case "speed": String(localized: "Its speed changed")
        case "sound": String(localized: "Its sound changed")
        case "timed": String(localized: "Timed: its marks or its start")
        case "mask": String(localized: "A mask of the layer beneath")
        case "hidden": String(localized: "Hidden")
        case "locked": String(localized: "Locked")
        case "pinned": String(localized: "Pinned to the version captured")
        default: l
        }
    }
}

// MARK: - Opened large

/// An asset opened large over the atlas: its picture; its clip or sound
/// played with the editor's controls (AtlasTransport: the scrubber, Loop,
/// the transport; a timeline as the core renders it); its words set as
/// the Prompt Editor sets them, Markdown drawn as it reads or as typed --
/// and what it was made from and what it goes into, each a click away.
struct AtlasExpanded: View {
    @Bindable var model: AppModel
    let asset: AssetDTO
    @State private var image: CGImage?
    @State private var drawing = false

    var body: some View {
        ZStack {
            Color.black.opacity(0.35)
                .ignoresSafeArea()
                .onTapGesture { close() }
            VStack(alignment: .leading, spacing: 12) {
                header
                content
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    .background(asset.kind == "text"
                                ? Color(nsColor: .textBackgroundColor)
                                : Color.black,
                                in: RoundedRectangle(cornerRadius: 8))
                    .clipShape(RoundedRectangle(cornerRadius: 8))
                if let pb = model.atlasPlayback {
                    AtlasTransport(playback: pb)
                        .frame(maxWidth: .infinity)
                }
                links
            }
            .padding(16)
            .frame(maxWidth: 980, maxHeight: 760)
            .background(Color.valtzPanel,
                        in: RoundedRectangle(cornerRadius: 14,
                                             style: .continuous))
            .shadow(color: .black.opacity(0.3), radius: 24, y: 8)
            .padding(36)
        }
        .task(id: "\(asset.id)-\(asset.modified)") { await load() }
        .onDisappear {
            model.atlasPlayback?.close()
            model.atlasPlayback = nil
        }
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: AtlasAssetNode.symbol(asset))
                .foregroundStyle(.secondary)
            VStack(alignment: .leading, spacing: 1) {
                Text(verbatim: asset.name)
                    .font(.title3.weight(.semibold))
                    .lineLimit(1)
                Text(verbatim: AtlasAssetNode.subtitle(asset))
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            if asset.kind == "text" { MarkdownToggle(model: model) }
            Button("Open in Editor") { model.atlasOpenInEditor(asset.id) }
            Button { close() } label: {
                Image(systemName: "xmark")
            }
            .keyboardShortcut(.cancelAction)
            .help("Close")
        }
    }

    @ViewBuilder
    private var content: some View {
        if asset.kind == "text" {
            AtlasTextView(text: asset.text ?? "",
                          markdown: model.promptMarkdown)
        } else if let pb = model.atlasPlayback, !pb.sound {
            AtlasPlayerView(player: pb.player, controls: false)
        } else if let image {
            Image(decorative: image, scale: 1)
                .resizable()
                .aspectRatio(contentMode: .fit)
        } else if drawing {
            ProgressView().controlSize(.small)
        } else {
            Image(systemName: AtlasAssetNode.symbol(asset))
                .font(.system(size: 40))
                .foregroundStyle(.white.opacity(0.3))
        }
    }

    /// What made it -- its op and inputs -- and what it goes into, each
    /// opened large in turn by a click.
    @ViewBuilder
    private var links: some View {
        let (from, op) = madeFrom
        let into = goesInto
        VStack(alignment: .leading, spacing: 6) {
            if !from.isEmpty || op != nil {
                HStack(spacing: 6) {
                    Text(op.map { AppModel.atlasOpTitle($0) }
                         ?? String(localized: "Made from"))
                        .font(.callout.weight(.medium))
                        .foregroundStyle(.secondary)
                    chips(from)
                }
            }
            if !into.isEmpty {
                HStack(spacing: 6) {
                    Text("Goes into")
                        .font(.callout.weight(.medium))
                        .foregroundStyle(.secondary)
                    chips(into)
                }
            }
        }
    }

    private func chips(_ ids: [String]) -> some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 6) {
                ForEach(ids, id: \.self) { id in
                    if let a = model.asset(id: id) {
                        Button {
                            model.atlasExpanded = id
                            if let n = model.atlas?.assetNode[id] {
                                model.atlasSelected =
                                    model.atlas?.graph.nodes[n].id
                            }
                        } label: {
                            Label(a.name, systemImage: AtlasAssetNode.symbol(a))
                                .lineLimit(1)
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                    }
                }
            }
        }
    }

    /// The assets its op took, and the op (none: its layers').
    private var madeFrom: ([String], String?) {
        guard let g = model.atlas, let me = g.assetNode[asset.id] else {
            return ([], nil)
        }
        var op: String?
        var ids: [String] = []
        for e in g.ends where e.1 == me {
            let o = g.graph.nodes[e.0]
            if o.type == "op" { op = o.op }
            for f in g.ends where f.1 == e.0 {
                if let a = g.graph.nodes[f.0].asset, !ids.contains(a) {
                    ids.append(a)
                }
            }
        }
        return (ids, op)
    }

    /// What it is made into, or shown in.
    private var goesInto: [String] {
        guard let g = model.atlas, let me = g.assetNode[asset.id] else {
            return []
        }
        var ids: [String] = []
        for e in g.ends where e.0 == me {
            if let into = g.graph.nodes[e.1].into, !ids.contains(into) {
                ids.append(into)
            }
        }
        return ids
    }

    private func close() {
        model.atlasPlayback?.close()
        model.atlasPlayback = nil
        model.atlasExpanded = nil
    }

    private func load() async {
        model.atlasPlayback?.close()
        model.atlasPlayback = nil
        image = nil
        guard let core = model.core, let project = model.projectId,
              asset.kind != "text" else { return }
        let id = asset.id
        model.atlasStop()
        if model.atlasPlayable(asset) {
            drawing = true
            let url: URL?
            if asset.isComposition {
                url = await Task.detached(priority: .userInitiated) {
                    core.renderedPath(project: project, asset: id)
                }.value
            } else {
                url = asset.url
            }
            drawing = false
            guard let url, !Task.isCancelled else { return }
            // In the editor's units: a clip's frames at its rate (a
            // timeline's own), a sound's milliseconds.
            let sound = asset.kind == "audio"
            let units: FrameRate = sound ? .milliseconds
                : asset.compositionRate ?? asset.info?.frameRate
                    ?? .fallback
            model.atlasPlayback = AtlasPlayback(url: url, units: units,
                                                sound: sound)
            // A sound shows its waveform where a clip shows its picture.
            if sound {
                image = await AudioWaveform.image(url, width: 1600,
                                                  height: 520)
            }
            return
        }
        drawing = true
        image = await Task.detached(priority: .userInitiated) {
            core.thumbnail(project: project, asset: id, maxPixels: 1600)
                .flatMap { AppModel.loadImage($0) }
        }.value
        drawing = false
    }
}

/// A text opened large: its words as the Prompt Editor sets them --
/// its type, its paragraphs' spacing -- and its Markdown drawn as it
/// reads, its markers hidden (PromptEditor.paintMarkdown), or as typed:
/// the editor's switch, MarkdownToggle. Read only; selectable.
struct AtlasTextView: NSViewRepresentable {
    let text: String
    let markdown: Bool

    final class Coordinator {
        var text: String?
        var markdown: Bool?
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
            tv.isRichText = false
            tv.textContainerInset = NSSize(width: 14, height: 14)
            tv.font = PromptEditor.font
        }
        return sv
    }

    func updateNSView(_ sv: NSScrollView, context: Context) {
        let c = context.coordinator
        guard let tv = sv.documentView as? NSTextView,
              c.text != text || c.markdown != markdown else { return }
        c.text = text
        c.markdown = markdown
        if tv.string != text { tv.string = text }
        PromptEditor.styleParagraphs(tv)
        // No caret: every marker hidden.
        PromptEditor.paintMarkdown(
            tv, on: markdown, caret: NSRange(location: NSNotFound, length: 0))
    }
}

// MARK: - Players

/// A player's picture: AVKit's view, its controls inline (opened large)
/// or none (a card).
struct AtlasPlayerView: NSViewRepresentable {
    let player: AVPlayer
    let controls: Bool

    func makeNSView(context: Context) -> AVPlayerView {
        let v = AVPlayerView()
        v.controlsStyle = controls ? .inline : .none
        v.videoGravity = .resizeAspect
        v.player = player
        return v
    }

    func updateNSView(_ v: AVPlayerView, context: Context) {
        if v.player !== player { v.player = player }
    }
}

/// How far a card's player is, a line along its foot.
private struct AtlasProgress: View {
    let player: AVPlayer
    var height: CGFloat = 2

    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.1)) { _ in
            GeometryReader { g in
                let d = player.currentItem?.duration.seconds ?? 0
                let t = player.currentTime().seconds
                let f = d.isFinite && d > 0 ? min(1, max(0, t / d)) : 0
                Rectangle()
                    .fill(Color.accentColor)
                    .frame(width: g.size.width * f, height: height)
                    .frame(maxHeight: .infinity, alignment: .bottom)
            }
        }
        .frame(height: height)
        .allowsHitTesting(false)
    }
}
