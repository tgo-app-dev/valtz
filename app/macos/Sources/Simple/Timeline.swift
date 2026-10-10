import AppKit
import SwiftUI

// MARK: - The timeline, in the model

/// One row of the TIMELINE (DESIGN §10a Timeline): a layer of the
/// composition on the stage, top first, where it lies in the
/// composition's frames (a still's pages) -- or a FOLDER's header over
/// its layers, its span theirs together.
struct TimelineRow: Equatable, Identifiable {
    enum Kind: Equatable {
        case clip, sound, picture, markup, composition, still, blank
        case folder
    }
    /// A layer's id; a folder's "folder:<id>".
    var id: String
    var title: String
    var kind: Kind
    var visible: Bool
    /// Its first frame, and the frame after its last -- nil while it runs
    /// on to the end, as a picture does.
    var start: Int
    var end: Int?
    /// Its keys (look, speed, sound), on the timeline's frames.
    var keys: [Int]
    /// A sound alone: its waveform's file, and the source seconds its
    /// span shows.
    var wave: URL?
    var from = 0.0
    var until: Double?
    /// The scissors can cut it: a clip or a sound as imported or made, or
    /// a part cut from one.
    var cuttable: Bool
    /// Its block's end stretches it: a still -- a picture, a markup, a
    /// still composition -- whose length is its own (a clip's is its
    /// marks').
    var stretchable = false
    /// The folder it is in (a layer), or it is (a header); folded shut.
    var folder: String?
    var folded = false
    /// A folder's: where its layers play, together -- [start, end)
    /// spans, merged; its layers' ids, bottom first.
    var union: [Range<Int>] = []
    var members: [String] = []

    var isFolder: Bool { kind == .folder }
    /// Set in under a folder's header.
    var depth: Int { folder != nil && !isFolder ? 1 : 0 }
    /// The layer's id; nil for a folder's header.
    var layer: String? { isFolder ? nil : id }

    static func folderID(_ f: String) -> String { "folder:" + f }
}

/// What the timeline draws: its rows, its length and rate, where the
/// player is, the selected layer, its tools and zoom -- and its EVENTS,
/// where |< and >| go: a key, where a layer begins or ends, the timeline's
/// start and end.
struct TimelineModel: Equatable {
    var rows: [TimelineRow] = []
    var frames = 1
    var rate = FrameRate.fallback
    var paged = false
    var playhead = 0
    var selected = ""
    /// Layers ⌘-clicked into the selection (several: to group, merge).
    var chosen: Set<String> = []
    var cutting = false
    var zoom = 1.0
    var events: [Int] = []
}

extension AppModel {
    /// What the timeline shows: the active composition when it is on the
    /// stage and runs in time -- a timeline, or a still with pages.
    var timelineAsset: AssetDTO? {
        guard let s = stageStack, s.isTimeline || s.isPaged,
              activeComposition?.id == s.id else { return nil }
        return s
    }

    /// Its switch is offered beside the prompt.
    var timelineAvailable: Bool { !promptImmersive && timelineAsset != nil }

    /// The timeline is in the prompt's place.
    var timelineOpen: Bool { timelineWanted && timelineAvailable }

    /// The timeline in the prompt's place, or the prompt back: the
    /// generation card goes with the prompt.
    func showTimeline(_ on: Bool) {
        withAnimation(Self.motion) {
            timelineWanted = on
            if on && openPanel == .generate { openPanel = nil }
            if !on { timelineCutting = false }
        }
    }

    var timelineModel: TimelineModel {
        var m = TimelineModel()
        guard let a = timelineAsset else { return m }
        m.paged = a.isPaged
        m.rate = a.isPaged ? FrameRate(num: 1, den: 1) : stageFrameRate
        m.playhead = a.isPaged ? stagePage : videoFrame
        m.selected = activeLayer
        if selectedLayers.count > 1 { m.chosen = selectedLayers }
        m.cutting = timelineCutting
        m.zoom = timelineZoom
        let fps = m.rate.fps
        var spans: [String: StackSpan] = [:]
        if stackPlayback != nil, clipOnStage {
            for s in stackPlayback?.layers ?? [] { spans[s.id] = s }
        }
        var latest = 0
        for l in a.layerStack.reversed() {
            let src = source(of: l)
            // In a composition of sound everything is sound (a clip's).
            let kind = Self.timelineKind(l, src)
            var r = TimelineRow(id: l.id, title: l.title,
                                kind: a.kind == "audio" && kind != .blank
                                    ? .sound : kind,
                                visible: l.visible, start: 0, end: nil,
                                keys: [], cuttable: false)
            if a.isPaged {
                let span = a.pageSpan(of: l)
                r.start = span?.lowerBound ?? 0
                r.end = span.map { $0.upperBound + 1 } ?? 0
            } else if let sp = spans[l.id] {
                r.start = Int((sp.start * fps).rounded())
                r.end = sp.end.map { Int(($0 * fps).rounded()) }
                if sp.audioOnly, let f = sp.file {
                    r.wave = f
                    r.from = sp.from
                    r.until = sp.until
                }
            } else {
                // Before the core's plan lands: as recorded.
                let t = l.time ?? LayerTimeDTO()
                r.start = t.offset
                if t.duration > 0 { r.end = t.offset + t.duration }
            }
            // A layer's first frame is a key of its own: where its
            // tracks start.
            r.keys = Array(Set(timelineKeys(a, layer: l.id)
                .map { r.start + $0 } + (l.isEmpty ? [] : [r.start])))
                .sorted()
            r.cuttable = !a.isPaged && Self.cuttable(src)
            r.stretchable = a.kind != "audio"
                && [.picture, .markup, .still].contains(r.kind)
            r.folder = l.folder
            latest = max(latest, r.end ?? r.start)
            m.rows.append(r)
        }
        if a.isPaged {
            m.frames = a.pageCount
        } else {
            m.frames = max(1, a.timeline ?? a.length ?? latest)
        }
        m.rows = timelineFolders(m.rows, frames: m.frames,
                                 folders: a.layerFolders ?? [])
        // Where |< and >| stop.
        var ev: Set<Int> = [0, m.frames]
        for r in m.rows {
            ev.insert(r.start)
            if let e = r.end { ev.insert(m.paged ? e - 1 : e) }
            ev.formUnion(r.keys)
        }
        m.events = ev.sorted()
        return m
    }

    /// The layers' rows with their folders' headers: each over its layers
    /// (out of sight while it is folded), its span theirs together.
    private func timelineFolders(_ rows: [TimelineRow], frames: Int,
                                 folders: [LayerFolderDTO])
        -> [TimelineRow] {
        var out: [TimelineRow] = []
        var open: String?
        for r in rows {
            if let f = r.folder, f != open {
                open = f
                let members = rows.filter { $0.folder == f }
                var head = TimelineRow(
                    id: TimelineRow.folderID(f),
                    title: folders.first { $0.id == f }?.name ?? f,
                    kind: .folder, visible: members.contains(where: \.visible),
                    start: members.map(\.start).min() ?? 0, end: nil,
                    keys: [], cuttable: false)
                head.folder = f
                head.folded = isFolderFolded(f)
                head.members = members.reversed().map(\.id)
                // Their spans merged: where any of them plays.
                var spans = members.filter { $0.kind != .blank }.map {
                    $0.start..<max($0.start + 1, $0.end ?? frames)
                }.sorted { $0.lowerBound < $1.lowerBound }
                var merged: [Range<Int>] = []
                while let s = spans.first {
                    spans.removeFirst()
                    if let last = merged.last, s.lowerBound <= last.upperBound {
                        merged[merged.count - 1] =
                            last.lowerBound..<max(last.upperBound, s.upperBound)
                    } else {
                        merged.append(s)
                    }
                }
                head.union = merged
                head.end = merged.map(\.upperBound).max()
                out.append(head)
            } else if r.folder == nil {
                open = nil
            }
            if let f = r.folder, isFolderFolded(f) { continue }
            out.append(r)
        }
        return out
    }

    /// What a row is, by what its layer shows -- a part cut from a clip
    /// or a sound is that clip or sound.
    static func timelineKind(_ l: LayerDTO,
                             _ src: AssetDTO?) -> TimelineRow.Kind {
        if l.isMarkup { return .markup }
        guard let src else { return l.isEmpty ? .blank : .picture }
        if src.kind == "audio" { return .sound }
        if src.isTimeline {
            return src.layerStack.count == 1 && src.kind == "video"
                && (src.modifiers ?? []).isEmpty ? .clip : .composition
        }
        if src.isComposition { return .still }
        return src.kind == "video" ? .clip : .picture
    }

    /// A clip or a sound as imported or made, or a part the scissors cut
    /// (a timeline of one plain layer showing one): the core has the last
    /// word (Controller::split_layer).
    static func cuttable(_ src: AssetDTO?) -> Bool {
        guard let src, src.kind == "video" || src.kind == "audio" else {
            return false
        }
        if !src.isComposition { return true }
        return src.isTimeline && src.layerStack.count == 1
            && (src.modifiers ?? []).isEmpty
    }

    /// A layer's keys -- its look, speed and sound -- from its own start:
    /// the panels' tracks for the selected layer (what they hold now),
    /// the record's for the others. A track that changes nothing has
    /// none to show.
    func timelineKeys(_ a: AssetDTO, layer: String) -> [Int] {
        func frames<T>(_ k: Keyframes<T>) -> [Int] {
            k.isIdentity ? [] : k.keys.map(\.frame)
        }
        var out: Set<Int> = []
        if layer == activeLayer && keyedStage {
            out.formUnion(frames(clipAdjustKeys))
            out.formUnion(frames(clipCropKeys.place))
            out.formUnion(frames(clipCropKeys.turn))
            if clipOnStage {
                out.formUnion(frames(layerSpeed))
                out.formUnion(frames(layerSound))
            }
        } else {
            let crop = a.cropKeys(layer: layer)
            out.formUnion(frames(a.adjustKeys(layer: layer)))
            out.formUnion(frames(crop.place))
            out.formUnion(frames(crop.turn))
            out.formUnion(frames(a.speedKeys(layer: layer)))
            out.formUnion(frames(a.soundKeys(layer: layer)))
        }
        return out.sorted()
    }

    /// The player at frame `n` of the timeline -- a still's page.
    func timelineSeek(_ n: Int) {
        if timelineAsset?.isPaged == true {
            goToPage(n)
        } else {
            seekVideo(to: n)
        }
    }

    /// A frame on or back (a still's page; a sound's 10 ms, as the Trim
    /// panel steps it).
    func timelineStep(_ n: Int) {
        if timelineOpen && pagedOnStage && !clipOnStage {
            goToPage(stagePage + n)
        } else {
            stepVideo(n)
        }
    }

    /// |< and >|: the timeline's previous or next EVENT -- a key, where a
    /// layer begins or ends, its start or end.
    func timelineEvent(next: Bool) {
        let m = timelineModel
        let last = max(0, m.frames - 1)
        let at = Set(m.events.map { min(max(0, $0), last) }).sorted()
        let p = m.playhead
        if let to = next ? at.first(where: { $0 > p })
                         : at.last(where: { $0 < p }) {
            timelineSeek(to)
        }
    }

    /// The scissors: the layer's clip cut at timeline frame `frame` in
    /// two parts (core Controller::split_layer), the second on a new
    /// layer above it -- selected.
    func cutTimelineLayer(_ id: String, at frame: Int) {
        guard let core, let projectId, let a = timelineAsset,
              a.isTimeline else { return }
        flushPanels()
        let r = core.layerOp(project: projectId, asset: a.id, "split",
                             layer: id, extra: ["frame": frame])
        guard r.ok else {
            NSSound.beep()
            flash(r.message)
            return
        }
        timelineLayersChanged(select: r["layer"] as? String)
    }

    /// A still's block stretched by its end: it runs `length` frames (a
    /// still's pages), the timeline's length grown to hold it.
    func stretchTimelineLayer(_ id: String, length: Int) {
        guard let core, let projectId, let a = timelineAsset else { return }
        flushPanels()
        let r = core.layerOp(project: projectId, asset: a.id, "stretch",
                             layer: id, extra: ["length": max(1, length)])
        guard r.ok else {
            flash(r.message)
            return
        }
        timelineLayersChanged(select: id)
    }

    /// A clip dragged along the timeline: it starts at `offset` (a
    /// still's page), the timeline's length grown to hold it.
    func slideTimelineLayer(_ id: String, to offset: Int) {
        guard let core, let projectId, let a = timelineAsset else { return }
        flushPanels()
        let r = core.layerOp(project: projectId, asset: a.id, "slide",
                             layer: id, extra: ["offset": max(0, offset)])
        guard r.ok else {
            flash(r.message)
            return
        }
        timelineLayersChanged(select: id)
    }

    /// An asset dropped on the timeline: a NEW layer showing it, right
    /// above the row it was dropped on (none: on top), from where it was
    /// dropped -- selected.
    @discardableResult
    func dropOnTimeline(asset id: String, above layer: String?,
                        at frame: Int) -> Bool {
        guard let core, let projectId, let t = timelineAsset,
              let a = assets.first(where: { $0.id == id }) else {
            return false
        }
        guard ["image", "video", "audio"].contains(a.kind) else {
            flash(String(localized: "Text does not go on the timeline: put it in the prompt."))
            return false
        }
        var extra: [String: Any] = ["asset": id, "onto": t.id,
                                    "offset": max(0, frame),
                                    "new_layer": true]
        if let layer { extra["at"] = layer }
        let r = core.assetOp(project: projectId, "instantiate", extra)
        guard r.ok else {
            flash(r.message)
            return false
        }
        timelineLayersChanged(select: r["layer"] as? String)
        return true
    }

    /// Files dropped on the timeline: imported, then the first picture,
    /// clip or sound put there as an asset is.
    func dropFilesOnTimeline(_ urls: [URL], above layer: String?,
                             at frame: Int) -> Bool {
        guard let url = urls.first(where: {
            ["image", "video", "audio"].contains(PromptTextView.kind(of: $0))
        }) else { return false }
        if let a = asset(forFile: url) {
            return dropOnTimeline(asset: a.id, above: layer, at: frame)
        }
        importFiles([url])
        Task { @MainActor [weak self] in
            for _ in 0..<600 {
                guard let self else { return }
                if let a = self.asset(forFile: url) {
                    self.dropOnTimeline(asset: a.id, above: layer, at: frame)
                    return
                }
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
        return true
    }

    /// The timeline's layers changed in the core: the panels take the
    /// selected layer as it now is -- nothing of theirs written back over
    /// it -- and the layer named is selected.
    private func timelineLayersChanged(select id: String?) {
        reloadAssets()
        if let a = stageStack {
            if clipOnStage { loadTrim(a) }
            if keyedStage { loadClipTracks(a, layer: activeLayer) }
        }
        if let id, id != activeLayer { selectLayer(id) }
        if clipOnStage { refreshStackPlan() } else { recomposite() }
    }

    /// Zoomed in or out (a factor), never further out than the whole of
    /// it in half the view.
    static let timelineZoomOut = 0.5
    func zoomTimeline(to z: Double) {
        timelineZoom = min(16_384, max(Self.timelineZoomOut, z))
    }

    /// One row more or fewer in view.
    func timelineRows(by d: Int) {
        let n = min(Self.timelineRowRange.upperBound,
                    max(Self.timelineRowRange.lowerBound, timelineRows + d))
        guard n != timelineRows else { return }
        withAnimation(Self.motion) { timelineRows = n }
    }
}

// MARK: - The card

/// The TIMELINE (DESIGN §10a Timeline), in the prompt's place: a wide
/// card with the prompt card's shadow. On its left the layers, top
/// first, frozen there; along its top the time in the composition's own
/// frames, frozen there; between them the clips where they play -- a key
/// a dot, a sound its waveform's shadow -- and the player's line through
/// all of them. Its top-left corner holds the scissors and the zoom; its
/// right edge one row more or fewer; its foot the transport, |< and >|
/// going from event to event.
struct TimelineCard: View {
    @Bindable var model: AppModel
    @State private var peaks: [String: AudioWaveform.TimedPeaks] = [:]

    static let transportHeight: CGFloat = 48
    static let sideWidth: CGFloat = 30
    static let radius: CGFloat = 20

    /// Its height with `rows` rows in view.
    static func height(rows: Int) -> CGFloat {
        TimelineNSView.rulerHeight
            + CGFloat(rows) * TimelineNSView.rowHeight + 0.5
            + transportHeight
    }

    var body: some View {
        let m = model.timelineModel
        let shape = RoundedRectangle(cornerRadius: Self.radius,
                                     style: .continuous)
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                TimelineCanvas(model: m, peaks: peaks, actions: actions(m))
                    .accessibilityLabel(Text("Timeline"))
                Rectangle()
                    .fill(Color(nsColor: .separatorColor))
                    .frame(width: 0.5)
                rowButtons
                    .frame(width: Self.sideWidth)
            }
            .frame(height: TimelineNSView.rulerHeight
                   + CGFloat(model.timelineRows) * TimelineNSView.rowHeight)
            .overlay(alignment: .topLeading) {
                corner(m)
                    .frame(width: TimelineNSView.nameWidth,
                           height: TimelineNSView.rulerHeight)
            }
            Rectangle()
                .fill(Color(nsColor: .separatorColor))
                .frame(height: 0.5)
            transport(m)
                .frame(height: Self.transportHeight)
        }
        .clipShape(shape)
        .background(shape.fill(Color(nsColor: .textBackgroundColor)))
        .overlay(shape.strokeBorder(Color.primary.opacity(0.08),
                                    lineWidth: 0.5))
        .shadow(color: .black.opacity(0.13), radius: 18, y: 8)
        .task(id: m.rows.compactMap(\.wave?.path)) {
            for r in m.rows {
                guard let url = r.wave, peaks[url.path] == nil else {
                    continue
                }
                if let p = await AudioWaveform.timedPeaks(url) {
                    peaks[url.path] = p
                }
            }
        }
    }

    private func actions(_ m: TimelineModel) -> TimelineActions {
        TimelineActions(
            seek: { model.timelineSeek($0) },
            select: { id in
                if id != model.activeLayer { model.selectLayer(id) }
            },
            slide: { model.slideTimelineLayer($0, to: $1) },
            stretch: { model.stretchTimelineLayer($0, length: $1) },
            cut: { model.cutTimelineLayer($0, at: $1) },
            zoom: { model.zoomTimeline(to: $0) },
            stopCutting: { model.timelineCutting = false },
            toggleVisible: { id in
                if let r = m.rows.first(where: { $0.id == id }) {
                    model.setLayerVisible(id, !r.visible)
                }
            },
            moveLayer: { model.moveLayer($0, by: $1) },
            addLayer: { id in
                if id != model.activeLayer { model.selectLayer(id) }
                model.addLayer()
            },
            duplicateLayer: { model.duplicateLayer($0) },
            removeLayer: { model.removeLayer($0) },
            canRemove: { model.canRemoveLayer($0) },
            toggleFolded: { model.toggleFolderFolded($0) },
            toggleFolderVisible: { f in
                let shown = m.rows.first { $0.isFolder && $0.folder == f }?
                    .visible ?? true
                model.setFolderVisible(f, !shown)
            },
            place: { model.placeLayers($0, above: $1, folder: $2) },
            group: { id in
                if !(model.selectedLayers.count > 1
                     && model.selectedLayers.contains(id)) {
                    model.selectLayer(id)
                }
                model.groupSelectedLayers()
            },
            ungroup: { model.ungroupLayers($0) },
            extendSelection: { model.toggleLayerSelection($0) },
            renameLayer: { model.renameLayer($0, $1) },
            renameFolder: { model.renameLayerFolder($0, $1) },
            dropAsset: { model.dropOnTimeline(asset: $0, above: $1, at: $2) },
            dropFiles: {
                model.dropFilesOnTimeline($0, above: $1, at: $2)
            })
    }

    /// The scissors, then zoom out, in and to the whole timeline.
    private func corner(_ m: TimelineModel) -> some View {
        HStack(spacing: 0) {
            TimelineTool(symbol: "scissors", on: model.timelineCutting,
                         help: m.paged
                             ? "Pages cannot be cut: the scissors cut clips and sounds"
                             : model.timelineCutting
                             ? "Put the scissors away (Escape)"
                             : "Scissors: click a clip or a sound to cut it there in two") {
                model.timelineCutting.toggle()
            }
            .disabled(m.paged)
            Spacer(minLength: 4)
            TimelineTool(symbol: "minus.magnifyingglass", on: false,
                         help: "Zoom out") {
                model.zoomTimeline(to: model.timelineZoom / 1.6)
            }
            .disabled(model.timelineZoom <= AppModel.timelineZoomOut)
            TimelineTool(symbol: "plus.magnifyingglass", on: false,
                         help: "Zoom in") {
                model.zoomTimeline(to: model.timelineZoom * 1.6)
            }
            TimelineTool(symbol: "arrow.left.and.right", on: false,
                         help: "The whole timeline") {
                model.zoomTimeline(to: 1)
            }
            .disabled(model.timelineZoom == 1)
        }
        .padding(.horizontal, 6)
    }

    /// One row more, one fewer.
    private var rowButtons: some View {
        VStack(spacing: 2) {
            TimelineTool(symbol: "chevron.up", on: false,
                         help: "One more row") { model.timelineRows(by: 1) }
                .disabled(model.timelineRows
                          >= AppModel.timelineRowRange.upperBound)
            TimelineTool(symbol: "chevron.down", on: false,
                         help: "One row fewer") { model.timelineRows(by: -1) }
                .disabled(model.timelineRows
                          <= AppModel.timelineRowRange.lowerBound)
        }
        .frame(maxHeight: .infinity)
    }

    /// The transport -- |< and >| from event to event -- with where the
    /// player is on its left and the timeline's length on its right, in
    /// the composition's own frames (a still's pages).
    private func transport(_ m: TimelineModel) -> some View {
        let r = model.videoRate
        let timed = !m.paged
        return ZStack {
            HStack(spacing: 2) {
                // A blank layer above the selected one; the selected one
                // gone -- as the Layers section does.
                TimelineTool(symbol: "rectangle.stack.badge.plus", on: false,
                             help: "Add a blank layer above the selected one") {
                    model.addLayer()
                }
                TimelineTool(symbol: "rectangle.stack.badge.minus", on: false,
                             help: "Remove the selected layer") {
                    model.removeLayer(model.activeLayer)
                }
                .disabled(!model.canRemoveLayer(model.activeLayer))
                Text(verbatim: place(m.playhead, m))
                    .font(.callout.monospacedDigit())
                    .padding(.leading, 10)
                Spacer()
                Text(verbatim: length(m))
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
            .padding(.leading, 10)
            .padding(.trailing, 18)
            HStack(spacing: 4) {
                TransportButton(symbol: "backward.end.fill",
                                help: "Previous event: a key, where a layer begins or ends, the start",
                                on: false) {
                    model.timelineEvent(next: false)
                }
                TransportButton(symbol: "backward.fill", help: "Fast rewind",
                                on: r < -1, action: model.fastRewind)
                    .disabled(!timed)
                TransportButton(symbol: "play.fill", mirrored: true,
                                help: "Play backward", on: r == -1) {
                    model.playVideo(rate: -1)
                }
                .disabled(!timed)
                TransportButton(symbol: "backward.frame.fill",
                                help: m.paged ? "The page before (←)"
                                    : "Back one frame (←)",
                                on: false) {
                    model.timelineStep(-1)
                }
                TransportButton(symbol: "pause.fill", help: "Pause",
                                on: timed && r == 0) {
                    model.playVideo(rate: 0)
                }
                .disabled(!timed)
                TransportButton(symbol: "forward.frame.fill",
                                help: m.paged ? "The page after (→)"
                                    : "On one frame (→)",
                                on: false) {
                    model.timelineStep(1)
                }
                TransportButton(symbol: "play.fill", help: "Play",
                                on: r == 1) {
                    model.playVideo(rate: 1)
                }
                .disabled(!timed)
                TransportButton(symbol: "forward.fill", help: "Fast forward",
                                on: r > 1, action: model.fastForward)
                    .disabled(!timed)
                TransportButton(symbol: "forward.end.fill",
                                help: "Next event: a key, where a layer begins or ends, the end",
                                on: false) {
                    model.timelineEvent(next: true)
                }
            }
        }
        .controlSize(.small)
    }

    /// "00:00:01:12" -- a still's "Page 2".
    private func place(_ n: Int, _ m: TimelineModel) -> String {
        m.paged ? String(localized: "Page \(String(n + 1))")
                : m.rate.timecode(n)
    }

    /// "00:00:05:00" -- a still's "4 pages".
    private func length(_ m: TimelineModel) -> String {
        m.paged ? String(localized: "\(String(m.frames)) pages")
                : m.rate.timecode(m.frames)
    }
}

/// The switch beside the prompt to the timeline (its wedge pointing
/// right), and beside the timeline back to the prompt and its
/// generation settings (pointing left): a grey tab of its own.
struct TimelineSwitch: View {
    let toTimeline: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            VStack(spacing: 5) {
                Image(systemName: toTimeline ? "film.stack" : "text.cursor")
                    .font(.system(size: 11))
                Image(systemName: toTimeline ? "chevron.right"
                                             : "chevron.left")
                    .font(.system(size: 12, weight: .bold))
            }
            .foregroundStyle(.secondary)
            .frame(width: 24, height: 58)
            .background(Capsule().fill(Color.primary.opacity(
                hovering ? 0.12 : 0.07)))
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .help(toTimeline ? "Show the timeline in place of the prompt"
                         : "Back to the prompt and its generation settings")
        .accessibilityLabel(Text(toTimeline ? "Timeline" : "Prompt"))
    }
}

/// A small tool in the timeline's corner and side: its symbol, pressed in
/// while it is on.
private struct TimelineTool: View {
    let symbol: String
    let on: Bool
    let help: LocalizedStringKey
    let action: () -> Void
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 12))
                .frame(width: 24, height: 22)
                .background(RoundedRectangle(cornerRadius: 5).fill(
                    on ? Color.accentColor.opacity(0.25)
                       : Color.primary.opacity(hovering && enabled
                                               ? 0.08 : 0)))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(on ? Color.accentColor
                         : enabled ? Color.primary : Color.secondary)
        .opacity(enabled ? 1 : 0.5)
        .help(help)
        .accessibilityLabel(Text(help))
        .onHover { hovering = $0 }
    }
}

// MARK: - The view

/// What the timeline asks of the model.
struct TimelineActions {
    var seek: (Int) -> Void = { _ in }
    var select: (String) -> Void = { _ in }
    var slide: (String, Int) -> Void = { _, _ in }
    /// A still's block stretched: its length, in frames (pages).
    var stretch: (String, Int) -> Void = { _, _ in }
    var cut: (String, Int) -> Void = { _, _ in }
    var zoom: (Double) -> Void = { _ in }
    var stopCutting: () -> Void = {}
    var toggleVisible: (String) -> Void = { _ in }
    var moveLayer: (String, Int) -> Void = { _, _ in }
    var addLayer: (String) -> Void = { _ in }
    var duplicateLayer: (String) -> Void = { _ in }
    var removeLayer: (String) -> Void = { _ in }
    var canRemove: (String) -> Bool = { _ in false }
    /// Folders: folded or open, every layer shown or hidden, layers put
    /// in or out ({ids, above, folder}), a new one of a layer (and the
    /// others selected), one undone; ⌘-click's selection.
    var toggleFolded: (String) -> Void = { _ in }
    var toggleFolderVisible: (String) -> Void = { _ in }
    var place: ([String], String?, String) -> Void = { _, _, _ in }
    var group: (String) -> Void = { _ in }
    var ungroup: (String) -> Void = { _ in }
    var extendSelection: (String) -> Void = { _ in }
    /// A header's name typed over: a layer's, a folder's.
    var renameLayer: (String, String) -> Void = { _, _ in }
    var renameFolder: (String, String) -> Void = { _, _ in }
    var dropAsset: (String, String?, Int) -> Bool = { _, _, _ in false }
    var dropFiles: ([URL], String?, Int) -> Bool = { _, _, _ in false }
}

struct TimelineCanvas: NSViewRepresentable {
    var model: TimelineModel
    var peaks: [String: AudioWaveform.TimedPeaks]
    var actions: TimelineActions

    func makeNSView(context: Context) -> TimelineNSView { TimelineNSView() }

    func updateNSView(_ v: TimelineNSView, context: Context) {
        v.actions = actions
        if v.peakPaths != Set(peaks.keys) { v.peaks = peaks }
        v.model = model
    }
}

/// The timeline drawn and handled by AppKit: the layers' names frozen on
/// the left, the ruler frozen on top, the tracks scrolling under both
/// (sideways with a trackpad's swipe or ⇧ and the wheel; up and down
/// through more rows than are shown; ⌘ or a pinch zooms where the
/// pointer is). A click on the ruler, or between clips, puts the player
/// there; a clip is dragged along its row, its ends snapping to the
/// others' events; the scissors cut a clip where it is clicked. Assets
/// and files dropped on it go on a new layer above the row, from where
/// they were dropped.
@MainActor
final class TimelineNSView: NSView {
    static let nameWidth: CGFloat = 168
    static let rulerHeight: CGFloat = 26
    static let rowHeight: CGFloat = 22
    /// How near, in points, an end comes before it snaps.
    private static let snap: CGFloat = 8

    var model = TimelineModel() {
        didSet {
            guard model != oldValue else { return }
            modelChanged(from: oldValue)
            needsDisplay = true
        }
    }
    var actions = TimelineActions()
    var peaks: [String: AudioWaveform.TimedPeaks] = [:] {
        didSet { needsDisplay = true }
    }
    var peakPaths: Set<String> { Set(peaks.keys) }

    /// Points of time scrolled off the left; of rows off the top.
    private var scrollX: CGFloat = 0
    private var scrollY: CGFloat = 0
    /// The frame under the pointer as a zoom began, and where it was.
    private var zoomAnchor: (frame: Double, x: CGFloat)?

    private enum Drag {
        case scrub
        case slide(row: Int, downX: CGFloat, to: Int)
        /// A still's block's end dragged: it ends at `end`.
        case stretch(row: Int, end: Int)
        /// A header pressed -- a layer's, or a folder's with its layers
        /// -- `moving` once it has gone 3 points, to `spot`.
        case reorder(row: Int, downY: CGFloat, spot: Spot, moving: Bool)
    }
    /// Where a header dragged goes: the gap above row `gap` (the rows'
    /// count: under the last), or INTO the folder of row `into`.
    private enum Spot: Equatable {
        case gap(Int)
        case into(Int)
    }
    private var drag: Drag?
    private var hover: CGPoint?
    /// A header's name being typed over (a double-click): its field, and
    /// the row's id.
    private var nameField: NSTextField?
    private var naming: String?
    /// Where a drop would go: above which row (nil: on top), from which
    /// frame.
    private var dropAt: (row: Int?, frame: Int)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        registerForDraggedTypes([.string, .fileURL, .URL])
        setAccessibilityRole(.group)
    }

    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { true }
    /// A click in the window behind another works at once, as in any
    /// editor's timeline.
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    // MARK: Geometry

    private var trackWidth: CGFloat { max(1, bounds.width - Self.nameWidth) }

    /// Points a frame: the whole timeline across the tracks, times the
    /// zoom -- a frame at most 48 points wide (a page 600; a millisecond
    /// 2.4).
    private func scale(zoom: Double) -> CGFloat {
        let fit = (trackWidth - 12) / CGFloat(max(1, model.frames))
        let top: CGFloat = model.paged ? 600
            : min(48, 2400 / CGFloat(max(1, model.rate.fps)))
        return min(max(fit, top), fit * CGFloat(zoom))
    }

    private var scale: CGFloat { scale(zoom: model.zoom) }

    private func x(_ frame: Double) -> CGFloat {
        Self.nameWidth + CGFloat(frame) * scale - scrollX
    }

    private func frame(at x: CGFloat) -> Double {
        Double((x - Self.nameWidth + scrollX) / scale)
    }

    private var rowsHeight: CGFloat {
        CGFloat(model.rows.count) * Self.rowHeight
    }

    private func rowTop(_ i: Int) -> CGFloat {
        Self.rulerHeight + CGFloat(i) * Self.rowHeight - scrollY
    }

    private func row(at y: CGFloat) -> Int? {
        guard y >= Self.rulerHeight else { return nil }
        let i = Int(((y - Self.rulerHeight + scrollY) / Self.rowHeight)
            .rounded(.down))
        return i >= 0 && i < model.rows.count ? i : nil
    }

    /// How far it scrolls: past the end by a third of the view, so a clip
    /// can be dragged out beyond it.
    private var maxScrollX: CGFloat {
        max(0, CGFloat(model.frames) * scale + trackWidth * 0.33
            - trackWidth)
    }

    private var maxScrollY: CGFloat {
        max(0, rowsHeight - (bounds.height - Self.rulerHeight))
    }

    private func clampScroll() {
        scrollX = min(max(0, scrollX), maxScrollX)
        scrollY = min(max(0, scrollY), maxScrollY)
    }

    /// A row's block: from its start to its end -- the timeline's end for
    /// one that runs on -- moved where it is being dragged.
    private func block(_ i: Int) -> CGRect? {
        let r = model.rows[i]
        guard r.kind != .blank, !r.isFolder else { return nil }
        var start = r.start
        var end = r.end ?? max(model.frames, r.start + 1)
        if case .slide(let row, _, let to)? = drag, row == i {
            end += to - start
            start = to
        }
        if case .stretch(let row, let to)? = drag, row == i { end = to }
        // The row's whole height, as a cell of Flash's timeline.
        let x0 = x(Double(start)), x1 = x(Double(end))
        return CGRect(x: x0, y: rowTop(i), width: max(2, x1 - x0),
                      height: Self.rowHeight)
    }

    // MARK: Changes

    private func modelChanged(from old: TimelineModel) {
        let oldScale = scale(zoom: old.zoom)
        if model.zoom != old.zoom || model.frames != old.frames {
            // The frame under the pointer stays under it; else the
            // player's line where it is, if it shows; else the left edge.
            let a: (frame: Double, x: CGFloat)
            if let z = zoomAnchor {
                a = z
            } else {
                let px = Self.nameWidth + CGFloat(old.playhead) * oldScale
                    - scrollX
                a = px >= Self.nameWidth && px <= bounds.width
                    ? (Double(old.playhead), px)
                    : (Double(scrollX / oldScale), Self.nameWidth)
            }
            scrollX = CGFloat(a.frame) * scale - (a.x - Self.nameWidth)
            zoomAnchor = nil
        }
        if model.cutting != old.cutting {
            window?.invalidateCursorRects(for: self)
        }
        // The row being named gone (undone, removed): its field too.
        if let id = naming, !model.rows.contains(where: { $0.id == id }) {
            endNaming(commit: false)
        }
        clampScroll()
        // The player's line kept in view as it moves.
        if model.playhead != old.playhead, drag == nil {
            let px = x(Double(model.playhead))
            if px < Self.nameWidth || px > bounds.width - 16 {
                scrollX = CGFloat(model.playhead) * scale - trackWidth * 0.1
                clampScroll()
            }
        }
    }

    override func setFrameSize(_ newSize: NSSize) {
        super.setFrameSize(newSize)
        clampScroll()
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        let w = bounds.width, h = bounds.height
        let nameW = Self.nameWidth, rulerH = Self.rulerHeight
        let tracks = CGRect(x: nameW, y: rulerH, width: w - nameW,
                            height: max(0, h - rulerH))
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: tracks).addClip()
        drawTracks(tracks)
        NSGraphicsContext.restoreGraphicsState()
        drawRuler(CGRect(x: nameW, y: 0, width: w - nameW, height: rulerH))
        drawPlayhead(height: h)
        drawNames(CGRect(x: 0, y: rulerH, width: nameW,
                         height: max(0, h - rulerH)))
        // The corner (the tools sit on it), the ruler's hairline, and the
        // divider between the layers' headers and their time.
        Self.chrome.setFill()
        CGRect(x: 0, y: 0, width: nameW, height: rulerH).fill()
        NSColor.separatorColor.setFill()
        CGRect(x: 0, y: rulerH - 0.5, width: w, height: 0.5).fill()
        NSColor.labelColor.withAlphaComponent(0.16).setFill()
        CGRect(x: nameW - Self.divider, y: 0, width: Self.divider,
               height: h).fill()
        drawReorderMark()
    }

    /// The divider's width.
    static let divider: CGFloat = 3

    /// A header being dragged: where it would go -- a line across, set in
    /// when it goes into a folder; a folder it goes into, tinted.
    private func drawReorderMark() {
        guard case .reorder(let i, _, let spot, true)? = drag,
              let to = place(moving: i, to: spot) else { return }
        NSColor.controlAccentColor.setFill()
        switch spot {
        case .into(let h):
            NSColor.controlAccentColor.withAlphaComponent(0.25).setFill()
            CGRect(x: 0, y: rowTop(h), width: bounds.width,
                   height: Self.rowHeight).fill()
        case .gap(let g):
            let inset: CGFloat = to.folder.isEmpty || model.rows[i].isFolder
                ? 0 : Self.indent
            CGRect(x: inset, y: rowTop(g) - 1, width: bounds.width - inset,
                   height: 2).fill()
        }
    }

    /// The ruler's and the names' ground: a shade off the card.
    private static let chrome = NSColor(name: nil) { a in
        a.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua
            ? NSColor.white.withAlphaComponent(0.04)
            : NSColor.black.withAlphaComponent(0.03)
    }

    private func drawTracks(_ area: CGRect) {
        let n = model.rows.count
        // Rows, every other one shaded; the selected one tinted.
        for i in 0..<n {
            let r = CGRect(x: area.minX, y: rowTop(i), width: area.width,
                           height: Self.rowHeight)
            guard r.intersects(area) else { continue }
            if model.rows[i].id == model.selected
                || model.chosen.contains(model.rows[i].id) {
                NSColor.controlAccentColor.withAlphaComponent(0.1).setFill()
                r.fill()
            } else if model.rows[i].isFolder {
                NSColor.labelColor.withAlphaComponent(0.04).setFill()
                r.fill()
            } else if i % 2 == 1 {
                NSColor.labelColor.withAlphaComponent(0.025).setFill()
                r.fill()
            }
        }
        // Past the timeline's end.
        let endX = x(Double(model.frames))
        if endX < area.maxX {
            NSColor.labelColor.withAlphaComponent(0.06).setFill()
            CGRect(x: max(area.minX, endX), y: area.minY,
                   width: area.maxX - max(area.minX, endX),
                   height: area.height).fill()
            NSColor.separatorColor.setFill()
            CGRect(x: endX - 0.5, y: area.minY, width: 1,
                   height: area.height).fill()
        }
        if n == 0 {
            let s = NSAttributedString(
                string: String(localized: "Drop pictures, clips and sounds here"),
                attributes: [.font: NSFont.systemFont(ofSize: 12),
                             .foregroundColor: NSColor.tertiaryLabelColor])
            let sz = s.size()
            s.draw(at: CGPoint(x: area.midX - sz.width / 2,
                               y: area.minY + 10))
        }
        for i in 0..<n {
            if model.rows[i].isFolder {
                drawFolderSpan(i, area)
                continue
            }
            guard let b = block(i), b.intersects(area) else { continue }
            drawBlock(i, b, area)
        }
        // The rows' lines, over the blocks that fill them.
        NSColor.labelColor.withAlphaComponent(0.08).setFill()
        for i in 0..<n {
            CGRect(x: area.minX, y: rowTop(i) + Self.rowHeight - 0.5,
                   width: area.width, height: 0.5).fill()
        }
        drawGrid(area)
        drawCutLine()
        drawDropMark(area)
    }

    /// A line down from each of the ruler's labelled ticks, across every
    /// layer to the view's foot -- over the blocks, as the rows' lines
    /// are -- so what lines up in time is seen to.
    private func drawGrid(_ area: CGRect) {
        let major = tickSteps().0
        let first = max(0, Int(frame(at: area.minX)) / major * major)
        let last = Int(frame(at: area.maxX)) + major
        NSColor.labelColor.withAlphaComponent(0.1).setFill()
        var f = first
        while f <= last {
            let fx = x(Double(f)).rounded() + 0.5
            if fx >= area.minX {
                CGRect(x: fx - 0.5, y: area.minY, width: 1,
                       height: area.height).fill()
            }
            f += major
        }
    }

    /// A folder's row: where any of its layers plays, a grey band each
    /// span, its name in the first.
    private func drawFolderSpan(_ i: Int, _ area: CGRect) {
        let r = model.rows[i]
        for (k, s) in r.union.enumerated() {
            let x0 = x(Double(s.lowerBound)), x1 = x(Double(s.upperBound))
            let b = CGRect(x: x0, y: rowTop(i) + 4, width: max(2, x1 - x0),
                           height: Self.rowHeight - 8)
            guard b.intersects(area) else { continue }
            NSColor.labelColor.withAlphaComponent(r.visible ? 0.22 : 0.08)
                .setFill()
            b.fill()
            NSColor.labelColor.withAlphaComponent(0.35).setStroke()
            let edge = NSBezierPath(rect: b.insetBy(dx: 0.5, dy: 0.5))
            edge.lineWidth = 1
            edge.stroke()
            if k == 0 {
                let left = max(b.minX, area.minX) + 5
                let room = b.maxX - left - 5
                if room > 24 {
                    NSAttributedString(string: r.title, attributes: [
                        .font: NSFont.systemFont(ofSize: 9.5,
                                                 weight: .semibold),
                        .foregroundColor: NSColor.labelColor
                            .withAlphaComponent(0.75),
                    ]).draw(with: CGRect(x: left, y: b.minY + 0.5,
                                         width: room, height: 13),
                            options: [.usesLineFragmentOrigin,
                                      .truncatesLastVisibleLine])
                }
            }
        }
    }

    /// A block's colour by what it shows: muted, so the dots, the
    /// waveforms and the player's line stand out on them.
    private static func color(_ k: TimelineRow.Kind) -> NSColor {
        switch k {
        case .clip: NSColor(srgbRed: 0.38, green: 0.51, blue: 0.68, alpha: 1)
        case .sound: NSColor(srgbRed: 0.70, green: 0.55, blue: 0.37, alpha: 1)
        case .picture: NSColor(srgbRed: 0.54, green: 0.46, blue: 0.66,
                               alpha: 1)
        case .markup: NSColor(srgbRed: 0.69, green: 0.63, blue: 0.36,
                              alpha: 1)
        case .composition: NSColor(srgbRed: 0.36, green: 0.56, blue: 0.57,
                                   alpha: 1)
        case .still: NSColor(srgbRed: 0.45, green: 0.47, blue: 0.65,
                             alpha: 1)
        case .blank, .folder: .clear
        }
    }

    private func drawBlock(_ i: Int, _ b: CGRect, _ area: CGRect) {
        let r = model.rows[i]
        let base = Self.color(r.kind)
        let path = NSBezierPath(rect: b)
        base.withAlphaComponent(r.visible ? 0.85 : 0.3).setFill()
        path.fill()
        // A sound's waveform, as a shadow on it.
        if let url = r.wave, let p = peaks[url.path] {
            drawWave(p, r, b, area)
        }
        let selected = r.id == model.selected
        (selected ? NSColor.labelColor : base.shadow(withLevel: 0.25)
            ?? base).withAlphaComponent(selected ? 0.85 : 0.9).setStroke()
        let edge = NSBezierPath(rect: b.insetBy(dx: selected ? 1 : 0.5,
                                                dy: selected ? 1 : 0.5))
        edge.lineWidth = selected ? 2 : 1
        edge.stroke()
        // A still's end stretches it: a grip there.
        if r.stretchable, b.width > 16 {
            let gx = b.maxX - (selected ? 5 : 4.5)
            let grip = NSBezierPath()
            for dx: CGFloat in [-1.25, 1.25] {
                grip.move(to: CGPoint(x: gx + dx, y: b.minY + 5))
                grip.line(to: CGPoint(x: gx + dx, y: b.maxY - 9))
            }
            grip.lineWidth = 1
            (r.kind == .markup ? NSColor.black : NSColor.white)
                .withAlphaComponent(r.visible ? 0.5 : 0.25).setStroke()
            grip.stroke()
        }
        // Its name, from where it shows.
        let left = max(b.minX, area.minX) + 6
        let room = b.maxX - left - (r.stretchable ? 10 : 6)
        if room > 24 {
            let dark = r.kind == .markup
            let s = NSAttributedString(string: r.title, attributes: [
                .font: NSFont.systemFont(ofSize: 9.5, weight: .medium),
                .foregroundColor: dark ? NSColor.black.withAlphaComponent(0.8)
                                       : NSColor.white.withAlphaComponent(
                                           r.visible ? 0.95 : 0.6),
            ])
            s.draw(with: CGRect(x: left, y: b.minY + 1, width: room,
                                height: 12),
                   options: [.usesLineFragmentOrigin,
                             .truncatesLastVisibleLine])
        }
        // Its keys: solid dots along its foot.
        let y = b.maxY - 4.5
        for k in r.keys {
            var kx = x(Double(k))
            if case .slide(let row, _, let to)? = drag, row == i {
                kx += CGFloat(to - r.start) * scale
            }
            guard kx >= b.minX - 1, kx <= b.maxX + 1 else { continue }
            // Inside the block, a key at its very start too.
            kx = min(max(kx, b.minX + 4), b.maxX - 4)
            // The accent's, ringed in the card's ground to show on any
            // block.
            let dot = NSBezierPath(ovalIn: CGRect(x: kx - 3, y: y - 3,
                                                  width: 6, height: 6))
            NSColor.controlAccentColor.setFill()
            dot.fill()
            NSColor.textBackgroundColor.setStroke()
            dot.lineWidth = 1.25
            dot.stroke()
        }
    }

    /// The waveform of the source's span the block shows, its loudest
    /// at each couple of points, dark over the block.
    private func drawWave(_ p: AudioWaveform.TimedPeaks, _ r: TimelineRow,
                          _ b: CGRect, _ area: CGRect) {
        let fps = model.rate.fps
        let frames = Double((r.end ?? model.frames) - r.start)
        guard frames > 0 else { return }
        let seconds = frames / fps
        let until = r.until ?? (r.from + seconds)
        let k = (until - r.from) / seconds   // source seconds a second
        let lo = max(b.minX, area.minX), hi = min(b.maxX, area.maxX)
        guard hi > lo else { return }
        let mid = b.midY + 1, half = (b.height - 6) / 2
        let step: CGFloat = 2
        let wave = NSBezierPath()
        var px = lo
        while px < hi {
            let t0 = Double((px - b.minX) / scale) / fps
            let t1 = Double((px + step - b.minX) / scale) / fps
            let v = CGFloat(p.peak(from: r.from + t0 * k,
                                   to: r.from + t1 * k))
            let hh = max(0.5, v * half)
            wave.appendRect(CGRect(x: px, y: mid - hh, width: step * 0.75,
                                   height: hh * 2))
            px += step
        }
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: b).addClip()
        NSColor.black.withAlphaComponent(0.3).setFill()
        wave.fill()
        NSGraphicsContext.restoreGraphicsState()
    }

    /// The scissors over a clip: where it would cut.
    private func drawCutLine() {
        guard model.cutting, drag == nil, let h = hover,
              let i = row(at: h.y), model.rows[i].cuttable,
              let b = block(i), b.contains(h) else { return }
        let cx = x(Double(cutFrame(h.x, row: i)))
        let line = NSBezierPath()
        line.move(to: CGPoint(x: cx, y: b.minY - 2))
        line.line(to: CGPoint(x: cx, y: b.maxY + 2))
        line.lineWidth = 1.5
        line.setLineDash([3, 2], count: 2, phase: 0)
        NSColor.systemRed.setStroke()
        line.stroke()
    }

    /// A drop's place: a line where the new layer would go -- above the
    /// row -- and one at the frame it would start.
    private func drawDropMark(_ area: CGRect) {
        guard let d = dropAt else { return }
        NSColor.controlAccentColor.setFill()
        let y = d.row.map { rowTop($0) } ?? rowTop(0)
        CGRect(x: area.minX, y: y - 1, width: area.width, height: 2).fill()
        let dx = x(Double(d.frame))
        CGRect(x: dx - 1, y: area.minY, width: 2, height: area.height)
            .fill()
    }

    private func drawRuler(_ r: CGRect) {
        Self.chrome.setFill()
        r.fill()
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: r).addClip()
        let endX = x(Double(model.frames))
        if endX < r.maxX {
            NSColor.labelColor.withAlphaComponent(0.06).setFill()
            CGRect(x: max(r.minX, endX), y: r.minY,
                   width: r.maxX - max(r.minX, endX), height: r.height)
                .fill()
        }
        let (major, minor) = tickSteps()
        let first = max(0, Int(frame(at: r.minX)) / minor * minor)
        let last = Int(frame(at: r.maxX)) + minor
        let font = NSFont.monospacedDigitSystemFont(ofSize: 9.5,
                                                    weight: .regular)
        NSColor.secondaryLabelColor.setFill()
        var f = first
        while f <= last {
            let fx = x(Double(f)).rounded() + 0.5
            let isMajor = f % major == 0
            let len: CGFloat = isMajor ? 9 : 4
            CGRect(x: fx - 0.5, y: r.maxY - len, width: 1, height: len)
                .fill()
            if isMajor {
                let label = model.paged ? String(f + 1)
                                        : model.rate.timecode(f)
                let s = NSAttributedString(string: label, attributes: [
                    .font: font,
                    .foregroundColor: NSColor.secondaryLabelColor,
                ])
                // After its tick: a time, a page's number (the player's
                // head is in a page's middle).
                s.draw(at: CGPoint(x: fx + 3, y: r.minY + 3))
            }
            f += minor
        }
        NSGraphicsContext.restoreGraphicsState()
    }

    /// The ruler's labelled step and its small one, in frames: a label at
    /// least 90 points from the next (a millisecond's 100).
    private func tickSteps() -> (Int, Int) {
        if model.paged {
            let every = max(1, Int((40 / scale).rounded(.up)))
            return (every, every)
        }
        let fps = model.rate.fps
        let room: CGFloat = model.rate.isMilliseconds ? 104 : 88
        let perSecond = max(1, Int(fps.rounded()))
        var steps: [Int] = []
        if model.rate.isMilliseconds {
            steps = [10, 20, 50, 100, 200, 500]
        } else {
            steps = [1, 2, 5, 10].filter { $0 < perSecond }
        }
        steps += [1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600]
            .map { $0 * perSecond }
        let major = steps.first { CGFloat($0) * scale >= room }
            ?? steps.last ?? perSecond
        let minor = major % 5 == 0 ? major / 5
            : major % 2 == 0 ? major / 2 : major
        return (major, max(1, minor))
    }

    private func drawPlayhead(height h: CGFloat) {
        let p = Double(model.playhead) + (model.paged ? 0.5 : 0)
        let px = x(p).rounded() + 0.5
        guard px >= Self.nameWidth, px <= bounds.width else { return }
        if model.paged {
            // The page shown, tinted through every row.
            NSColor.systemRed.withAlphaComponent(0.06).setFill()
            CGRect(x: x(Double(model.playhead)), y: Self.rulerHeight,
                   width: scale, height: h - Self.rulerHeight).fill()
        }
        NSColor.systemRed.setFill()
        CGRect(x: px - 0.75, y: 4, width: 1.5, height: h - 4).fill()
        let head = NSBezierPath()
        head.move(to: CGPoint(x: px - 5, y: 2))
        head.line(to: CGPoint(x: px + 5, y: 2))
        head.line(to: CGPoint(x: px + 5, y: 9))
        head.line(to: CGPoint(x: px, y: 14))
        head.line(to: CGPoint(x: px - 5, y: 9))
        head.close()
        head.fill()
    }

    private func drawNames(_ area: CGRect) {
        Self.chrome.setFill()
        area.fill()
        NSGraphicsContext.saveGraphicsState()
        NSBezierPath(rect: area).addClip()
        for (i, r) in model.rows.enumerated() {
            let top = rowTop(i)
            let rr = CGRect(x: 0, y: top, width: area.width,
                            height: Self.rowHeight)
            guard rr.intersects(area) else { continue }
            if r.id == model.selected || model.chosen.contains(r.id) {
                NSColor.controlAccentColor.withAlphaComponent(0.16).setFill()
                rr.fill()
            } else if r.isFolder {
                NSColor.labelColor.withAlphaComponent(0.05).setFill()
                rr.fill()
            }
            if case .reorder(let from, _, _, true)? = drag, from == i {
                NSColor.labelColor.withAlphaComponent(0.08).setFill()
                rr.fill()
            }
            let tint: NSColor = r.visible ? .secondaryLabelColor
                                          : .tertiaryLabelColor
            // Shown or hidden, as the Layers section's eye (a folder's:
            // all of its layers).
            if let eye = Self.image(r.visible ? "eye" : "eye.slash",
                                    r.visible ? .labelColor
                                              : .tertiaryLabelColor) {
                Self.draw(eye, centredIn: Self.eyeRect(top: top))
            }
            var x = Self.iconX + CGFloat(r.depth) * Self.indent
            if r.isFolder {
                if let fold = Self.image(r.folded ? "chevron.right"
                                                  : "chevron.down",
                                         .secondaryLabelColor) {
                    Self.draw(fold, centredIn: Self.foldRect(top: top))
                }
                x += 12
            }
            if let img = Self.symbol(r.kind, tint) {
                Self.draw(img, centredIn: CGRect(x: x, y: top, width: 14,
                                                 height: Self.rowHeight))
            }
            let s = NSAttributedString(string: r.title, attributes: [
                .font: NSFont.systemFont(ofSize: 10.5,
                                         weight: r.isFolder ? .semibold
                                                            : .regular),
                .foregroundColor: r.visible ? NSColor.labelColor
                                            : NSColor.secondaryLabelColor,
            ])
            // Being named: its field is there instead.
            if r.id != naming {
                s.draw(with: CGRect(x: x + 17, y: top + 4.5,
                                    width: area.width - x - 17
                                        - Self.divider - 4,
                                    height: 14),
                       options: [.usesLineFragmentOrigin,
                                 .truncatesLastVisibleLine])
            }
            NSColor.labelColor.withAlphaComponent(0.08).setFill()
            CGRect(x: 0, y: rr.maxY - 0.5, width: area.width, height: 0.5)
                .fill()
        }
        NSGraphicsContext.restoreGraphicsState()
    }

    /// The headers' columns: the eye, a folder's fold, what a row is
    /// (set in under a folder).
    private static let iconX: CGFloat = 20
    static let indent: CGFloat = 12

    /// A row's eye: where a click shows or hides its layer (a folder's).
    private static func eyeRect(top: CGFloat) -> CGRect {
        CGRect(x: 3, y: top, width: 16, height: rowHeight)
    }

    /// A folder's fold: where a click folds it shut or opens it.
    private static func foldRect(top: CGFloat) -> CGRect {
        CGRect(x: iconX, y: top, width: 12, height: rowHeight)
    }

    private static func image(_ name: String, _ tint: NSColor) -> NSImage? {
        let cfg = NSImage.SymbolConfiguration(pointSize: 9, weight: .medium)
            .applying(.init(paletteColors: [tint]))
        return NSImage(systemSymbolName: name, accessibilityDescription: nil)?
            .withSymbolConfiguration(cfg)
    }

    private static func draw(_ img: NSImage, centredIn r: CGRect) {
        let sz = img.size
        img.draw(in: CGRect(x: r.midX - sz.width / 2,
                            y: r.midY - sz.height / 2,
                            width: sz.width, height: sz.height))
    }

    private static func symbol(_ k: TimelineRow.Kind,
                               _ tint: NSColor) -> NSImage? {
        let name = switch k {
        case .clip: "film"
        case .sound: "waveform"
        case .picture: "photo"
        case .markup: "pencil.tip"
        case .composition: "rectangle.stack"
        case .still: "photo.stack"
        case .blank: "square.dashed"
        case .folder: "folder"
        }
        return image(name, tint)
    }

    // MARK: Moving headers

    /// A header dragged to `spot`: the layers it moves (a folder's, all
    /// of them), the layer they go right above (nil: the bottom), the
    /// folder they go in -- or nil where it would not move.
    private func place(moving i: Int, to spot: Spot)
        -> (ids: [String], above: String?, folder: String)? {
        let rows = model.rows
        guard rows.indices.contains(i) else { return nil }
        let mv = rows[i]
        let ids = mv.isFolder ? mv.members : [mv.id]
        // The layer topmost under a row: a folder's, its top one.
        let anchor = { (r: TimelineRow) -> String? in
            r.isFolder ? r.members.last : r.id
        }
        switch spot {
        case .into(let h):
            // A layer into a folder, on top; not a folder into another.
            guard !mv.isFolder, rows.indices.contains(h),
                  let f = rows[h].folder, mv.folder != f else { return nil }
            return (ids, anchor(rows[h]), f)
        case .gap(let g):
            // Not next to itself (a folder: not within its own rows).
            if g == i || g == i + 1 { return nil }
            if mv.isFolder, (i + 1..<min(rows.count, i + 1
                    + (mv.folded ? 0 : mv.members.count))).contains(g) {
                return nil
            }
            let below = g < rows.count ? rows[g] : nil
            let above = g > 0 ? rows[g - 1] : nil
            var to = below.flatMap(anchor)
            // Between a folder's header (open) or one of its layers and
            // one of its layers: in it.
            var folder = ""
            if let a = above, let f = a.folder, !(a.isFolder && a.folded),
               let b = below, !b.isFolder, b.folder == f {
                folder = f
            }
            if mv.isFolder {
                // A folder goes between others, never into one: above the
                // one it was dropped in.
                if !folder.isEmpty, folder != mv.folder,
                   let h = rows.first(where: { $0.isFolder
                       && $0.folder == folder }) {
                    to = h.members.last
                }
                folder = mv.folder ?? ""
            }
            return (ids, to, folder)
        }
    }

    // MARK: Snapping

    /// What an end snaps to: the other rows' events, the timeline's start
    /// and end, the player's line.
    private func snapTargets(excluding i: Int?) -> [Int] {
        var t: Set<Int> = [0, model.frames, model.playhead]
        for (j, r) in model.rows.enumerated() where j != i && !r.isFolder {
            t.insert(r.start)
            if let e = r.end { t.insert(e) }
            t.formUnion(r.keys)
        }
        return Array(t)
    }

    /// The frame at `x`, snapped to an event within reach.
    private func snapped(_ x: CGFloat, excluding i: Int? = nil) -> Int {
        let f = frame(at: x)
        let reach = Double(Self.snap / scale)
        if let t = snapTargets(excluding: i)
            .min(by: { abs(Double($0) - f) < abs(Double($1) - f) }),
           abs(Double(t) - f) <= reach {
            return t
        }
        return Int(f.rounded())
    }

    /// Where the scissors cut: the frame edge nearest the pointer, or an
    /// event (the player's line) within reach.
    private func cutFrame(_ x: CGFloat, row i: Int) -> Int {
        snapped(x, excluding: i)
    }

    /// Where a row's block ends: its end, or the timeline's for one that
    /// runs on.
    private func end(of r: TimelineRow) -> Int {
        r.end ?? max(model.frames, r.start + 1)
    }

    /// Over a still's block's right end -- its last few points, or just
    /// past it: where a press stretches it.
    private func atStretchEnd(_ i: Int, _ p: CGPoint) -> Bool {
        guard model.rows.indices.contains(i), model.rows[i].stretchable,
              let b = block(i), p.y >= b.minY, p.y <= b.maxY else {
            return false
        }
        return p.x >= b.maxX - min(5, b.width / 3) && p.x <= b.maxX + 4
    }

    /// A row dragged by `dx` points: its start, its start or end snapped
    /// to an event within reach -- never before the timeline's start.
    private func slideStart(_ i: Int, dx: CGFloat) -> Int {
        let r = model.rows[i]
        let s = Double(r.start) + Double(dx / scale)
        let len = r.end.map { Double($0 - r.start) }
        let reach = Double(Self.snap / scale)
        var best: (d: Double, to: Double)?
        for t in snapTargets(excluding: i) {
            let tt = Double(t)
            for (d, to) in [(abs(tt - s), tt)]
                + (len.map { [(abs(tt - (s + $0)), tt - $0)] } ?? []) {
                if d <= reach, d < (best?.d ?? .infinity) {
                    best = (d, to)
                }
            }
        }
        return max(0, Int((best?.to ?? s).rounded()))
    }

    // MARK: Pointer

    override func mouseDown(with e: NSEvent) {
        window?.makeFirstResponder(self)
        let p = convert(e.locationInWindow, from: nil)
        if Self.tracing {
            print("snapshot: timeline-down \(Int(p.x)),\(Int(p.y)) row=\(row(at: p.y).map(String.init) ?? "-")")
        }
        if p.y < Self.rulerHeight {
            if p.x >= Self.nameWidth {
                drag = .scrub
                scrub(p.x)
            }
            return
        }
        let i = row(at: p.y)
        if p.x < Self.nameWidth {
            guard let i else { return }
            let r = model.rows[i]
            // The eye shows or hides it (a folder's, all its layers); a
            // folder's fold folds it; elsewhere a press selects a layer
            // (⌘ or ⇧: in or out of the selection), and a drag moves it --
            // a folder with its layers -- up or down the stack.
            if Self.eyeRect(top: rowTop(i)).contains(p) {
                if let f = r.folder, r.isFolder {
                    actions.toggleFolderVisible(f)
                } else {
                    actions.toggleVisible(r.id)
                }
                return
            }
            if r.isFolder, let f = r.folder,
               Self.foldRect(top: rowTop(i)).contains(p) {
                actions.toggleFolded(f)
                return
            }
            let mods = e.modifierFlags
            // A double-click on its name names it, as in the Layers
            // section (AppKit's click count: the second click of two).
            if e.clickCount == 2, !mods.contains(.command),
               !mods.contains(.shift) {
                beginNaming(i)
                return
            }
            if let id = r.layer {
                if mods.contains(.command) || mods.contains(.shift) {
                    actions.extendSelection(id)
                    return
                }
                actions.select(id)
            }
            drag = .reorder(row: i, downY: p.y, spot: .gap(i),
                            moving: false)
            return
        }
        guard let i else {
            drag = .scrub
            scrub(p.x)
            return
        }
        let r = model.rows[i]
        if r.isFolder {
            drag = .scrub
            scrub(p.x)
            return
        }
        // A still's block's end: dragged, it stretches the still.
        if !model.cutting, atStretchEnd(i, p) {
            actions.select(r.id)
            drag = .stretch(row: i, end: end(of: r))
            NSCursor.resizeLeftRight.set()
            return
        }
        if let b = block(i), b.contains(p) {
            actions.select(r.id)
            if model.cutting {
                if r.cuttable {
                    actions.cut(r.id, cutFrame(p.x, row: i))
                } else {
                    NSSound.beep()
                }
                return
            }
            drag = .slide(row: i, downX: p.x, to: r.start)
            NSCursor.closedHand.set()
        } else {
            actions.select(r.id)
            drag = .scrub
            scrub(p.x)
        }
    }

    override func mouseDragged(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        switch drag {
        case .scrub?:
            scrub(p.x)
        case .slide(let i, let downX, _)?:
            drag = .slide(row: i, downX: downX,
                          to: slideStart(i, dx: p.x - downX))
            needsDisplay = true
        case .stretch(let i, _)?:
            // Snapping to an event within reach; a frame at least; a
            // still's page within its pages.
            var to = max(model.rows[i].start + 1, snapped(p.x, excluding: i))
            if model.paged { to = min(to, model.frames) }
            drag = .stretch(row: i, end: to)
            needsDisplay = true
        case .reorder(let i, let downY, _, let moving)?:
            guard moving || abs(p.y - downY) > 3 else { return }
            // Over a folder's header's middle, a layer goes into it; else
            // into the gap between rows nearest the pointer.
            let at = (p.y - Self.rulerHeight + scrollY) / Self.rowHeight
            let r = Int(at.rounded(.down))
            let frac = at - at.rounded(.down)
            let spot: Spot
            if model.rows.indices.contains(r), model.rows[r].isFolder,
               !model.rows[i].isFolder, frac > 0.3, frac < 0.7 {
                spot = .into(r)
            } else {
                spot = .gap(min(max(0, Int(at.rounded())), model.rows.count))
            }
            drag = .reorder(row: i, downY: downY, spot: spot, moving: true)
            NSCursor.closedHand.set()
            needsDisplay = true
        case nil:
            break
        }
    }

    override func mouseUp(with e: NSEvent) {
        defer {
            drag = nil
            needsDisplay = true
            window?.invalidateCursorRects(for: self)
        }
        if case .slide(let i, _, let to)? = drag, i < model.rows.count,
           to != model.rows[i].start {
            actions.slide(model.rows[i].id, to)
        }
        if case .stretch(let i, let to)? = drag, i < model.rows.count,
           to != end(of: model.rows[i]) {
            actions.stretch(model.rows[i].id, to - model.rows[i].start)
        }
        // A header dropped: its layers where it went, in a folder or out.
        if case .reorder(let i, _, let spot, true)? = drag,
           let to = place(moving: i, to: spot) {
            actions.place(to.ids, to.above, to.folder)
        }
    }

    /// A layer's menu, on its header or its row: a blank layer above it,
    /// shown or hidden, removed.
    override func menu(for event: NSEvent) -> NSMenu? {
        let p = convert(event.locationInWindow, from: nil)
        guard let i = row(at: p.y) else { return nil }
        let r = model.rows[i]
        let m = NSMenu()
        // A folder's: every layer of it shown or hidden; undone.
        if r.isFolder, let f = r.folder {
            m.addItem(Self.item(r.visible ? String(localized: "Hide Layers")
                                          : String(localized: "Show Layers")) {
                [weak self] in self?.actions.toggleFolderVisible(f)
            })
            m.addItem(Self.item(String(localized: "Ungroup")) {
                [weak self] in self?.actions.ungroup(f)
            })
            return m
        }
        // Several chosen with it: they go in a folder together.
        let several = model.chosen.count > 1 && model.chosen.contains(r.id)
        if !several { actions.select(r.id) }
        m.addItem(Self.item(several
                            ? String(localized: "Group Selected Layers")
                            : String(localized: "Put in a New Folder")) {
            [weak self] in self?.actions.group(r.id)
        })
        if let f = r.folder,
           let head = model.rows.first(where: { $0.isFolder
               && $0.folder == f }) {
            m.addItem(Self.item(String(localized: "Take Out of Its Folder")) {
                [weak self] in
                self?.actions.place([r.id], head.members.last, "")
            })
        }
        m.addItem(.separator())
        m.addItem(Self.item(String(localized: "Add Layer Above")) { [weak self] in
            self?.actions.addLayer(r.id)
        })
        if r.kind != .blank {
            m.addItem(Self.item(String(localized: "Duplicate Layer")) {
                [weak self] in self?.actions.duplicateLayer(r.id)
            })
        }
        m.addItem(Self.item(r.visible ? String(localized: "Hide Layer")
                                      : String(localized: "Show Layer")) {
            [weak self] in self?.actions.toggleVisible(r.id)
        })
        m.addItem(.separator())
        let remove = Self.item(String(localized: "Remove Layer")) {
            [weak self] in self?.actions.removeLayer(r.id)
        }
        remove.isEnabled = actions.canRemove(r.id)
        m.addItem(remove)
        return m
    }

    /// A menu item running `run`.
    private static func item(_ title: String,
                             _ run: @escaping @MainActor () -> Void)
        -> NSMenuItem {
        let it = NSMenuItem(title: title, action: #selector(MenuRun.go(_:)),
                            keyEquivalent: "")
        let target = MenuRun(run)
        it.target = target
        it.representedObject = target  // kept alive with the item
        return it
    }

    private func scrub(_ px: CGFloat) {
        let last = max(0, model.frames - 1)
        let f = model.paged ? Int(frame(at: px).rounded(.down))
                            : Int(frame(at: px).rounded())
        actions.seek(min(max(0, f), last))
    }

    // MARK: Naming a header

    /// A row's name made a field, where it is drawn: its words selected.
    private func beginNaming(_ i: Int) {
        endNaming(commit: true)
        let r = model.rows[i]
        guard r.layer != nil || r.folder != nil else { return }
        let x = Self.iconX + CGFloat(r.depth) * Self.indent
            + (r.isFolder ? 12 : 0) + 14
        let f = NSTextField(string: r.title)
        f.font = .systemFont(ofSize: 10.5,
                             weight: r.isFolder ? .semibold : .regular)
        f.isBordered = true
        f.isBezeled = true
        f.bezelStyle = .squareBezel
        f.drawsBackground = true
        f.focusRingType = .none
        f.cell?.isScrollable = true
        f.cell?.wraps = false
        f.delegate = self
        f.frame = CGRect(x: x, y: rowTop(i) + 2,
                         width: Self.nameWidth - x - Self.divider - 2,
                         height: Self.rowHeight - 4)
        f.setAccessibilityLabel(String(localized: "Layer name"))
        addSubview(f)
        nameField = f
        naming = r.id
        needsDisplay = true
        // Its editing begun, the words selected (selectText ends any
        // editing under way: a makeFirstResponder before it was undone).
        f.selectText(nil)
    }

    /// The field gone: its name kept (Return, a click away) or not
    /// (Escape). A layer's emptied takes its default name back; a
    /// folder's is never empty.
    private func endNaming(commit: Bool) {
        guard let f = nameField, let id = naming else { return }
        nameField = nil
        naming = nil
        let text = f.stringValue.trimmingCharacters(in: .whitespaces)
        f.delegate = nil
        f.removeFromSuperview()
        needsDisplay = true
        if window?.firstResponder == nil
            || window?.firstResponder === window {
            window?.makeFirstResponder(self)
        }
        guard commit, let r = model.rows.first(where: { $0.id == id }),
              text != r.title else { return }
        if r.isFolder, let folder = r.folder {
            if !text.isEmpty { actions.renameFolder(folder, text) }
        } else if let layer = r.layer {
            actions.renameLayer(layer, text)
        }
    }

    /// Whether a header's name is being typed (the hooks).
    var isNaming: Bool { nameField != nil }

    override func scrollWheel(with e: NSEvent) {
        // The rows move: the name being typed is kept first.
        endNaming(commit: true)
        let mods = e.modifierFlags
        var dx = e.scrollingDeltaX, dy = e.scrollingDeltaY
        if !e.hasPreciseScrollingDeltas {
            dx *= 8
            dy *= 8
        }
        if mods.contains(.command) || mods.contains(.option) {
            let p = convert(e.locationInWindow, from: nil)
            zoom(by: exp(Double(dy) * 0.01), at: p.x)
            return
        }
        if mods.contains(.shift), dx == 0 {
            (dx, dy) = (dy, 0)
        }
        if abs(dx) >= abs(dy) || maxScrollY == 0 {
            scrollX -= abs(dx) >= abs(dy) ? dx : dy
        } else {
            scrollY -= dy
        }
        clampScroll()
        needsDisplay = true
    }

    override func magnify(with e: NSEvent) {
        let p = convert(e.locationInWindow, from: nil)
        zoom(by: 1 + Double(e.magnification), at: p.x)
    }

    private func zoom(by f: Double, at px: CGFloat) {
        let at = max(Self.nameWidth, px)
        zoomAnchor = (frame(at: at), at)
        actions.zoom(model.zoom * f)
    }

    override func keyDown(with e: NSEvent) {
        // Escape puts the scissors away.
        if e.keyCode == 53, model.cutting {
            actions.stopCutting()
            return
        }
        super.keyDown(with: e)
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        trackingAreas.forEach(removeTrackingArea)
        addTrackingArea(NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .mouseEnteredAndExited, .cursorUpdate,
                      .activeInKeyWindow, .inVisibleRect],
            owner: self))
    }

    override func mouseMoved(with e: NSEvent) {
        hover = convert(e.locationInWindow, from: nil)
        updateCursor()
        if model.cutting { needsDisplay = true }
    }

    override func mouseExited(with e: NSEvent) {
        hover = nil
        NSCursor.arrow.set()
        if model.cutting { needsDisplay = true }
    }

    override func cursorUpdate(with e: NSEvent) {
        hover = convert(e.locationInWindow, from: nil)
        updateCursor()
    }

    private func updateCursor() {
        if drag == nil, !model.cutting, let h = hover, let i = row(at: h.y),
           h.x >= Self.nameWidth, atStretchEnd(i, h) {
            NSCursor.resizeLeftRight.set()
            return
        }
        guard drag == nil, let h = hover, let i = row(at: h.y),
              h.x >= Self.nameWidth, let b = block(i), b.contains(h) else {
            NSCursor.arrow.set()
            return
        }
        if model.cutting {
            (model.rows[i].cuttable ? Self.scissors : .operationNotAllowed)
                .set()
        } else {
            NSCursor.openHand.set()
        }
    }

    private static let scissors: NSCursor = {
        let cfg = NSImage.SymbolConfiguration(pointSize: 16, weight: .medium)
            .applying(.init(paletteColors: [.labelColor]))
        guard let img = NSImage(systemSymbolName: "scissors",
                                accessibilityDescription: nil)?
            .withSymbolConfiguration(cfg) else { return .crosshair }
        return NSCursor(image: img, hotSpot: CGPoint(x: img.size.width / 2,
                                                     y: img.size.height / 2))
    }()

    // MARK: Drops

    private func carries(_ pb: NSPasteboard) -> Bool {
        PromptTextView.draggedAsset(pb) != nil
            || !PromptTextView.fileURLs(pb).isEmpty
    }

    private func place(of info: NSDraggingInfo) -> (row: Int?, frame: Int) {
        let p = convert(info.draggingLocation, from: nil)
        return (row(at: p.y), max(0, snapped(max(Self.nameWidth, p.x))))
    }

    override func draggingEntered(_ s: NSDraggingInfo) -> NSDragOperation {
        draggingUpdated(s)
    }

    override func draggingUpdated(_ s: NSDraggingInfo) -> NSDragOperation {
        guard carries(s.draggingPasteboard) else { return [] }
        let at = place(of: s)
        if dropAt?.row != at.row || dropAt?.frame != at.frame {
            dropAt = at
            needsDisplay = true
        }
        return .copy
    }

    override func draggingExited(_ s: NSDraggingInfo?) {
        dropAt = nil
        needsDisplay = true
    }

    override func performDragOperation(_ s: NSDraggingInfo) -> Bool {
        defer {
            dropAt = nil
            needsDisplay = true
        }
        let pb = s.draggingPasteboard
        let at = place(of: s)
        let layer = at.row.map { model.rows[$0].id }
        if let id = PromptTextView.draggedAsset(pb) {
            return actions.dropAsset(id, layer, at.frame)
        }
        let files = PromptTextView.fileURLs(pb)
        return !files.isEmpty && actions.dropFiles(files, layer, at.frame)
    }

    /// For the snapshot hooks: a frame's place in the window's points
    /// from its top left, at a row's middle.
    func windowPoint(frame f: Int, row i: Int) -> CGPoint? {
        guard let win = window else { return nil }
        let local = CGPoint(x: x(Double(f)) + 1,
                            y: rowTop(i) + Self.rowHeight / 2)
        let w = convert(local, to: nil)
        return CGPoint(x: w.x, y: win.frame.height - w.y)
    }

    /// For the snapshot hooks: a row's block's right end, in the window's
    /// points from its top left.
    func windowPoint(endOf i: Int) -> CGPoint? {
        guard let win = window, let b = block(i) else { return nil }
        let w = convert(CGPoint(x: b.maxX - 1, y: b.midY), to: nil)
        return CGPoint(x: w.x, y: win.frame.height - w.y)
    }

    /// For the snapshot hooks: a row's header in the window -- its eye,
    /// or its name -- or the gap above row `gap`, in points from the
    /// window's top left.
    func windowPoint(header i: Int, eye: Bool = false, fold: Bool = false,
                     gap: Double? = nil) -> CGPoint? {
        guard let win = window else { return nil }
        // A gap's line, or (a fraction) that far into a row.
        let y = gap.map {
            Self.rulerHeight + CGFloat($0) * Self.rowHeight - scrollY
        } ?? rowTop(i) + Self.rowHeight / 2
        let x: CGFloat = eye ? 11 : fold ? Self.iconX + 6 : 100
        let w = convert(CGPoint(x: x, y: y), to: nil)
        return CGPoint(x: w.x, y: win.frame.height - w.y)
    }

    /// The timeline on screen (the snapshot hooks find it), and whether
    /// its clicks are printed.
    static weak var shown: TimelineNSView?
    static let tracing =
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] != nil

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { Self.shown = self }
    }
}

/// A menu item's action as a closure.
@MainActor
private final class MenuRun: NSObject {
    let run: @MainActor () -> Void
    init(_ run: @escaping @MainActor () -> Void) { self.run = run }
    @objc func go(_ sender: Any?) { run() }
}

extension TimelineNSView: NSTextFieldDelegate {
    /// Return names it; Escape leaves it as it was.
    func control(_ control: NSControl, textView: NSTextView,
                 doCommandBy sel: Selector) -> Bool {
        if sel == #selector(NSResponder.cancelOperation(_:)) {
            endNaming(commit: false)
            return true
        }
        if sel == #selector(NSResponder.insertNewline(_:)) {
            endNaming(commit: true)
            return true
        }
        return false
    }

    /// A click away: kept.
    func controlTextDidEndEditing(_ obj: Notification) {
        endNaming(commit: true)
    }
}
