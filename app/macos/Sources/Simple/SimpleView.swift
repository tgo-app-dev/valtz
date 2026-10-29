import SwiftUI
import AppKit

/// Simple mode: a prompt in the middle of the window, nothing else until
/// there is something to show.
///
///   idle        the Valtz mark above the prompt, both centred
///   generating  the mark breathes until the first preview frame
///   showing     the live preview / result in a framed card above the
///               prompt, with glass compare controls beneath it
///   immersive   the Prompt Editor (DESIGN §10c): the prompt card grown
///               to the window's height at its own width, its tabs down
///               its left side; the stage small at the top left
///
/// Every change of state is one animated layout change (AppModel.motion).
/// The stage is ONE view, drawn behind the column where a slot says --
/// above the prompt, or at the top left -- so going into the editor moves
/// it there (its canvas and player carry on) rather than making another.
struct SimpleView: View {
    @Bindable var model: AppModel
    /// Where this view is in the window: the stage's drag ring stays in
    /// it, out of the title bar and the markup toolbar above.
    @State private var area: CGRect = .null

    /// The prompt column's width: the card's, in either mode.
    static let columnWidth: CGFloat = 680

    var body: some View {
        let immersive = model.promptImmersive
        // Sizes from the view's own geometry, in the same layout pass: the
        // card's width, the stage's place, change together and animate
        // together (a measured size would come a pass late).
        GeometryReader { geo in
            ZStack(alignment: .topLeading) {
                // The whole view, the column centred in it: the editor's
                // top left is the window's, not the column's.
                column(immersive, size: geo.size)
                    .frame(width: geo.size.width, height: geo.size.height)
                if immersive {
                    topLeft(geo.size)
                }
            }
        }
        .backgroundPreferenceValue(StageSlotKey.self) { slot in
            GeometryReader { proxy in
                if let slot, model.stageVisible {
                    let r = proxy[slot]
                    ResultStage(model: model, area: area, small: immersive)
                        .frame(width: r.width, height: r.height)
                        .offset(x: r.minX, y: r.minY)
                        .transition(.asymmetric(
                            insertion: .scale(scale: 0.94)
                                .combined(with: .opacity),
                            removal: .opacity))
                }
            }
        }
        .animation(AppModel.motion, value: model.stageVisible)
        .animation(AppModel.motion, value: model.openPanel)
        .animation(AppModel.motion, value: model.timelineOpen)
        .animation(AppModel.motion, value: model.viewOnly)
        .onGeometryChange(for: CGRect.self) {
            $0.frame(in: .global)
        } action: {
            area = $0
            model.editorRoom = $0.height
            model.roomMeasured()
        }
        // The prompt row's first picture, in an image prompt: its pencil
        // suggested -- the picture to edit, if it is one. Watched here:
        // the row itself comes with its first item.
        .onChange(of: model.promptAttachments.filter {
            $0.kind == "image"
        }.map(\.id)) { was, now in
            model.suggestBase(was: was, now: now)
        }
        .onChange(of: model.activeModality) { _, _ in
            model.baseHint = nil
        }
    }

    /// The prompt's column: the stage's slot (or the mark) above the
    /// composer -- or, in the editor, the composer alone, the window's
    /// height, a status bar's height short of its foot. There, while the
    /// card is split -- the assistant's suggestion beside the prompt -- it
    /// reaches right to the window's edge, or the inspector's, a status
    /// bar's height from it as from the foot, its left edge where it was.
    /// (It stayed its own width beside the inspector, which left it a
    /// column of empty room.) The agent's session splits nothing: it
    /// takes the card at its own width.
    private func column(_ immersive: Bool, size: CGSize) -> some View {
        let base = min(Self.columnWidth, size.width - 80)
        let wide = immersive && model.suggesting
        let card = wide ? max(base, size.width - StatusBar.height
                                    - (size.width - base) / 2)
                        : base
        return VStack(spacing: 0) {
            if !immersive {
                if model.stageVisible {
                    Color.clear
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .anchorPreference(key: StageSlotKey.self,
                                          value: .bounds) { $0 }
                        .onGeometryChange(for: CGFloat.self) {
                            $0.size.height
                        } action: {
                            model.stageRoom = $0
                            model.roomMeasured()
                        }
                        .padding(.bottom, 22)
                } else {
                    // The mark and the room around it: what the composer
                    // can grow into (AppModel.pictureRoom).
                    Spacer(minLength: 20)
                        .onGeometryChange(for: CGFloat.self) {
                            $0.size.height
                        } action: {
                            model.markRoom[0] = $0
                            model.roomMeasured()
                        }
                    // Ahead of the spacers for room, but it shrinks when a
                    // tall prompt (or open drawer) needs the height.
                    // The mark takes no drops: media go into the prompt,
                    // or onto a stage's layers.
                    HeroMark(sweep: model.markSweep)
                        // A fleet job's overlay shows the mark, in blue.
                        .opacity(model.fleetServing == nil ? 1 : 0)
                        .layoutPriority(1)
                        .onGeometryChange(for: CGFloat.self) {
                            $0.size.height
                        } action: {
                            model.markRoom[1] = $0
                            model.roomMeasured()
                        }
                        .padding(24)
                        .transition(.scale(scale: 0.8)
                            .combined(with: .opacity))
                        .padding(.bottom, 41 - 24)
                        .onAppear { model.startupSweep() }
                    // Before the first frame: what it is doing, at the
                    // width it has over the stage.
                    if model.isGenerating {
                        GenerationStatus(
                            model: model,
                            width: min(GenerationStatus.defaultWidth, base))
                            .padding(.bottom, 20)
                            .transition(.opacity)
                    }
                }
            }
            // The cards over the composer, and the composer: a notice
            // floats over the topmost of them.
            VStack(spacing: 0) {
                // View only: the stage alone (AppModel.viewOnly) -- the
                // notice still floats where the composer's top would be.
                if !immersive && !model.viewOnly {
                    if let f = model.memoryFailure, !model.suggesting {
                        MemoryFailureCard(model: model, failure: f)
                            .frame(maxWidth: Self.columnWidth)
                            .padding(.bottom, 14)
                            .transition(.move(edge: .bottom)
                                .combined(with: .opacity))
                    }
                    if !immersive, model.activeModality == .agent,
                       let a = model.agent, !model.suggesting {
                        AgentCard(model: model, shown: a)
                            .frame(maxWidth: Self.columnWidth)
                            .padding(.bottom, 14)
                            .transition(.move(edge: .bottom)
                                .combined(with: .opacity))
                    }
                    if model.suggesting {
                        SuggestionCard(prompt: model.enhanced?.prompt
                                           ?? model.enhanceDraft,
                                       writing: model.suggestionWriting,
                                       stats: model.agentShowsSpeed
                                           ? model.enhanced?.stats : nil,
                                       maxTextHeight: SuggestionCard.textCap(
                                           window: size.height),
                                       accept: model.acceptEnhanced,
                                       dismiss: model.dismissEnhanced,
                                       review: {
                                           withAnimation(AppModel.motion) {
                                               model.enterImmersivePrompt()
                                           }
                                       })
                            .frame(maxWidth: Self.columnWidth)
                            .padding(.bottom, 14)
                            .transition(.move(edge: .bottom)
                                .combined(with: .opacity))
                    }
                }
                if !model.viewOnly {
                ComposerStack(model: model, maxPromptHeight: size.height / 2,
                              immersive: immersive, cardWidth: card)
                    .onGeometryChange(for: CGFloat.self) { $0.size.width } action: {
                        model.composerWidth = $0
                        model.composerColumn = base
                    }
                    .frame(width: immersive ? card : nil)
                    // The timeline is wide: the window's width but the
                    // margins, where its switch back sits.
                    .frame(maxWidth: immersive ? nil
                           : model.timelineOpen ? max(base, size.width - 80)
                           : Self.columnWidth)
                    .frame(maxHeight: immersive ? .infinity : nil,
                           alignment: .top)
                    // Wider, it grows to the right: its left edge stays.
                    .offset(x: immersive ? (card - base) / 2 : 0)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
                }
            }
            // A NOTICE, floating between the stage (or the mark) and the
            // prompt or the timeline -- over neither's layout: nothing
            // moves as it comes and goes. It went under the composer in
            // secondary text, where it was missed. Over the stage's own bar
            // (a clip's Loop / Grab Frame / Continue, the compare's) and a
            // generation's status, it stands above them.
            .overlay(alignment: .top) {
                if !immersive, let n = model.notice {
                    // Its foot on the group's top (a frame of no height, the
                    // capsule standing up out of it), a gap above.
                    NoticeCapsule(notice: n, dismiss: model.dismissNotice)
                        .frame(maxWidth: min(Self.columnWidth, base))
                        .frame(height: 0, alignment: .bottom)
                        .offset(y: -noticeLift)
                        .transition(.move(edge: .bottom)
                            .combined(with: .opacity))
                        .id(n.text)
                }
            }
            if !immersive {
                if !model.stageVisible {
                    Spacer(minLength: 20)
                        .onGeometryChange(for: CGFloat.self) {
                            $0.size.height
                        } action: {
                            model.markRoom[2] = $0
                            model.roomMeasured()
                        }
                }
            }
        }
        .padding(.horizontal, 40)
        .padding(.top, immersive ? 0 : 12)
        .padding(.bottom, immersive ? StatusBar.height
                 : model.stageVisible ? 28 : 0)
    }

    /// How far over the composer a notice floats: clear of the stage's
    /// bar under the picture and of a generation's status.
    private var noticeLift: CGFloat {
        let clip = model.stage.video != nil && model.stage.clip == nil
        let bar = model.stageVisible && (clip || model.compareOn
            || model.stagePagesShown)
        // A clip's scrubber over its bar, but while the timeline is open.
        let scrubber = model.stageVisible && clip && !model.timelineOpen
        return 12 + (bar ? 40 : 0) + (scrubber ? 28 : 0)
            + (model.isGenerating ? 52 : 0)
    }

    /// In the editor, the space left of the card and its tabs: the stage
    /// there, at that width -- or, with nothing on it, a generation's
    /// progress. Too narrow a space, and it stays away.
    @ViewBuilder
    private func topLeft(_ size: CGSize) -> some View {
        let card = min(Self.columnWidth, size.width - 80)
        let room = (size.width - card) / 2 - PromptTabRail.outset - 28
        if room >= 72 {
            let aspect = max(0.2, model.stage.layoutAspect)
            let w = min(room, (size.height * 0.45) * aspect)
            // Its own transport under the picture, when it plays or has
            // pages: too small for a player's over it.
            let bar = model.stageHasTransport ? Self.smallBar : 0
            Group {
                if model.stageVisible {
                    Color.clear
                        .frame(width: w, height: w / aspect + bar)
                        .anchorPreference(key: StageSlotKey.self,
                                          value: .bounds) { $0 }
                } else if model.isGenerating {
                    CompactStatus(model: model)
                        .frame(width: room)
                        .transition(.opacity)
                }
            }
            .padding(.leading, Self.smallInset)
            // As far under the editor's toolbar as from the window's left
            // edge -- measured, so it never lies over the toolbar.
            .padding(.top, Self.smallInset
                     + max(0, model.editorToolbarBottom - area.minY))
        }
    }

    /// The small stage's margin, left and top.
    static let smallInset: CGFloat = 16
    /// Its transport's row, under the picture.
    static let smallBar: CGFloat = 30
}

/// Where the stage goes: the slot above the prompt, or the editor's top
/// left.
/// Media dropped on the stage are INSTANTIATED in its work: in the
/// selected layer when it is blank, else in a new one right above it --
/// an asset of the list, a file (imported first), or what a drag from
/// outside makes a file of (a browser's or Photos' picture, an app's
/// bytes, a link: DroppedMedia, imported as a copy). A project opens, as
/// anywhere in the window. The stage's own drag is not taken back.
/// Read off the drag's pasteboard (an onDrop closure is handed
/// providers, promises received, before it runs).
struct StageDrop: DropDelegate {
    let model: AppModel
    @Binding var targeted: Bool

    private var pasteboard: NSPasteboard { NSPasteboard(name: .drag) }

    func validateDrop(info: DropInfo) -> Bool {
        let pb = pasteboard
        if DroppedMedia.isStage(pb) { return false }
        let p = DroppedMedia.peek(pb)
        // View only, or an agent at work holding the project: a project
        // still opens; nothing else is taken.
        if model.viewOnly || model.agentHolds {
            return p.files.contains(where: AppModel.isProjectFile)
        }
        if !DroppedMedia.rows(pb).isEmpty { return true }
        return p.made > 0 || p.files.contains {
            AppModel.isProjectFile($0)
                || PromptTextView.kind(of: $0) != "file"
        }
    }

    func dropEntered(info: DropInfo) { targeted = true }
    func dropExited(info: DropInfo) { targeted = false }

    func performDrop(info: DropInfo) -> Bool {
        targeted = false
        let pb = pasteboard
        if model.viewOnly || model.agentHolds {
            let p = DroppedMedia.peek(pb)
            guard let project = p.files.first(where: AppModel.isProjectFile)
            else { return false }
            model.openDropped(project)
            return true
        }
        if let id = DroppedMedia.rows(pb).first {
            return model.dropAssetOnStage(id)
        }
        let intake = DroppedMedia.take(pb)
        if let project = intake.files.first(where: AppModel.isProjectFile) {
            model.openDropped(project)
            return true
        }
        if model.dropFilesOnStage(intake.files) { return true }
        guard intake.makes else { return false }
        Task { @MainActor in
            let made = await intake.made()
            if let e = intake.failed.first {
                model.flash(e, level: made.isEmpty ? .error : .warning)
            }
            // The first one, as a file dropped there; the rest into Assets.
            model.importMade(made) { ids in
                if let first = ids.first { model.dropAssetOnStage(first) }
            }
        }
        return true
    }
}

private struct StageSlotKey: PreferenceKey {
    static let defaultValue: Anchor<CGRect>? = nil
    static func reduce(value: inout Anchor<CGRect>?,
                       nextValue: () -> Anchor<CGRect>?) {
        value = nextValue() ?? value
    }
}

/// The Prompt Editor's small stage's transport, under its picture: a
/// clip, a timeline or a sound { |<, play / pause, >| } (Space too); a
/// still with pages { ‹, › }.
private struct SmallTransport: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 4) {
            if model.clipOnStage {
                let playing = model.videoRate != 0
                button("backward.end.fill", "Go to the start") {
                    model.seekStage(end: false)
                }
                button(playing ? "pause.fill" : "play.fill",
                       playing ? "Pause (Space)" : "Play (Space)") {
                    model.playVideo(rate: playing ? 0 : 1)
                }
                .shortcutHint(String(localized: "Space"))
                button("forward.end.fill", "Go to the end") {
                    model.seekStage(end: true)
                }
            } else {
                button("chevron.backward", "Previous page") {
                    model.goToPage(model.stagePage - 1)
                }
                .disabled(model.stagePage <= 0)
                button("chevron.forward", "Next page") {
                    model.goToPage(model.stagePage + 1)
                }
                .disabled(model.stagePage >= model.stagePages - 1)
            }
        }
        .frame(maxWidth: .infinity)
        .padding(.top, 4)
    }

    private func button(_ symbol: String, _ help: LocalizedStringKey,
                        action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 11, weight: .semibold))
                .frame(width: 28, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .help(help)
        .accessibilityLabel(Text(help))
    }
}

/// A still's PAGES under the stage, where a clip's transport is, while
/// the timeline that steps them is not shown -- editing or VIEW ONLY: ‹,
/// which page of how many, › (← → too; ⌘← ⌘→ or Home and End the first
/// and the last: AppModel.trimKey).
private struct StagePages: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 2) {
            TitleBarButton(symbol: "chevron.backward", title: "Previous Page",
                           help: "The page before") {
                model.goToPage(model.stagePage - 1)
            }
            .disabled(model.stagePage <= 0)
            .shortcutHint("←")
            Text("Page \(String(model.stagePage + 1)) of \(String(model.stagePages))")
                .monospacedDigit()
                .padding(.horizontal, 6)
            TitleBarButton(symbol: "chevron.forward", title: "Next Page",
                           help: "The page after") {
                model.goToPage(model.stagePage + 1)
            }
            .disabled(model.stagePage >= model.stagePages - 1)
            .shortcutHint("→")
        }
        .titleBarCapsule()
    }
}

/// A NOTICE (AppModel.flash) as it floats over the composer: its words
/// in a Liquid Glass capsule -- an error's tinted a mild red, a warning's
/// a mild yellow, its sign beside the words. A click lets it go.
private struct NoticeCapsule: View {
    let notice: Notice
    let dismiss: () -> Void

    private var symbol: String {
        switch notice.level {
        case .info: "info.circle.fill"
        case .warning: "exclamationmark.triangle.fill"
        case .error: "xmark.octagon.fill"
        }
    }

    /// The level's colour; nil for a plain notice.
    private var accent: Color? {
        switch notice.level {
        case .info: nil
        case .warning: Color(red: 0.95, green: 0.72, blue: 0.10)
        case .error: Color(red: 0.92, green: 0.26, blue: 0.24)
        }
    }

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: symbol)
                .foregroundStyle(accent ?? .secondary)
            Text(notice.text)
                .foregroundStyle(.primary)
                .multilineTextAlignment(.leading)
                .lineLimit(3)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.callout.weight(.medium))
        .padding(.horizontal, 18)
        .padding(.vertical, 10)
        // The glass BEHIND the words, not around them: content inside a
        // glass effect waits for the glass's own appearance, and a notice
        // comes and goes in seconds.
        .background {
            Capsule()
                .fill(.clear)
                .glassEffect(.regular.tint(accent?.opacity(0.22)),
                             in: Capsule())
        }
        // Its rim: the level's colour, or -- a plain notice, whose glass
        // over a white window all but vanishes -- a hairline.
        .overlay {
            Capsule().strokeBorder(accent?.opacity(0.5)
                                       ?? Color.primary.opacity(0.14),
                                   lineWidth: 1)
        }
        .shadow(color: .black.opacity(0.12), radius: 8, y: 2)
        .contentShape(Capsule())
        .onTapGesture(perform: dismiss)
        .help("Click to dismiss")
        .accessibilityElement(children: .combine)
    }
}

/// A generation's progress where the small stage would be: a bar and
/// its phase.
private struct CompactStatus: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            PhaseBar(fraction: model.generationPhase?.fraction)
                .progressViewStyle(.linear)
            Text(model.generationCaption)
                .font(.caption)
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
    }
}

/// The Valtz mark in glass with a soft shadow, rendered with Metal
/// (Mark/MarkView), about twice the size the painted mark was shown at.
/// A light sweeps across it at startup and when a generation is
/// requested; otherwise it is still. (The frame is larger than the mark:
/// it leaves room for the shadow; the mark itself is ~245 x 192 pt.)
private struct HeroMark: View {
    let sweep: Int

    var body: some View {
        MarkView(animating: false, material: .glass, sweep: sweep,
                 shadow: true)
            .aspectRatio(294.0 / 253.0, contentMode: .fit)
            .frame(maxWidth: 294, maxHeight: 253)
            .accessibilityElement()
            .accessibilityLabel("Valtz")
    }
}

/// The preview / result, framed, with the compare controls under it --
/// or, `small` (the Prompt Editor's top left), what it shows alone: A, a
/// player with its own controls, a thin progress bar; no compare, no
/// zoom.
/// The stage's player: a view of its own, so what changes with each
/// step of a look or a sound -- the clip's look, its stack's plan -- is
/// read here alone. Read in the stage's body, a grade wheel's drag on a
/// clip evaluated and laid out the whole stage again at every step.
private struct StagePlayer: View {
    @Bindable var model: AppModel
    let url: URL
    let small: Bool
    let growth: StageGrowth?

    var body: some View {
        // A voice-over's take plays once to the end, never over again.
        VideoPlayerView(url: url,
                        loops: model.loopClips && !model.voiceOverRecording,
                        // Not as a project opens: still, where saved.
                        autoplay: model.stageAutoplay,
                        muted: model.stageMuted,
                        rate: model.stageFrameRate,
                        command: model.videoCommand,
                        look: model.stackPlayback == nil
                            ? model.stageClipLook : nil,
                        stack: model.stackPlayback,
                        artwork: model.stageIsAudio ? model.stage.a : nil,
                        // None over the picture: the stage's transport
                        // is in the row under it (StageTransport), the
                        // small stage's under it too.
                        controls: false,
                        core: model.core,
                        onFrame: { model.videoFrameChanged($0) },
                        onLastFrame: { model.videoLastFrame = $0 },
                        onRate: { model.videoRate = $0 },
                        onMuted: { model.videoMuted = $0 },
                        onCue: { model.voiceOverCued($0) },
                        // The title bar's zoom (not the small stage's).
                        zoom: small ? nil : model.stage.zoomRequests,
                        growth: growth,
                        onViewport: { model.stage.setViewport($0) },
                        onShown: { model.stageVideoRect = $0 })
    }
}

private struct ResultStage: View {
    @Bindable var model: AppModel
    /// The editor's own area, in the window (SimpleView): the drag ring
    /// reaches no further.
    let area: CGRect
    var small = false
    @State private var dropTargeted = false
    /// The compare (or clip) bar under the card, in the window: the ring
    /// leaves it out.
    @State private var barFrame: CGRect = .null
    /// The card's width: the scrubber over the clip bar is as wide.
    @State private var cardWidth: CGFloat = 0

    /// How far outside the image the drag ring reaches.
    private let ringWidth: CGFloat = 28

    /// The guides' marks beside the card: guides shown over something of
    /// the project's they measure.
    /// The guides' marks set them by hand: not while view only (the
    /// lines still show).
    private var marksShown: Bool {
        model.showsGuides && !small && model.guideCanvas != nil
            && !model.viewOnly
    }
    private static let showsBand =
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT_RING"] != nil

    var body: some View {
        let ready = model.stageDragReady && model.canDragStage
        VStack(spacing: small ? 0 : 16) {
            GeometryReader { geo in
                // The guides' marks beside the card: room for them.
                let gutter = marksShown ? GuideMarks.gutter : .zero
                let room = CGSize(width: geo.size.width - 2 * gutter.width,
                                  height: geo.size.height - 2 * gutter.height)
                // Zoomed, the card grows with the picture (DESIGN §10)
                // -- not the small stage's, which is fitted.
                let fit = fitted(aspect: model.stage.layoutAspect, in: room)
                let growth = small ? nil
                    : StageGrowth(room: CGSize(width: max(0, room.width),
                                               height: max(0, room.height)),
                                  fit: fit)
                let size = growth.map { model.stage.card(in: $0) } ?? fit
                let lift = model.stageLift
                card(growth)
                    // Ready to drag: lifted -- a deeper, wider shadow --
                    // and a little faded.
                    .opacity((ready ? 0.82 : 1) * (1 - lift))
                    .shadow(color: .black.opacity(ready ? 0.36 : 0.22),
                            radius: (ready ? 40 : 24) + 30 * lift,
                            y: (ready ? 20 : 12) + 24 * lift)
                    // An asset viewed from the list is put down on the
                    // desk -- from above, larger -- and taken off again.
                    .scaleEffect(1 + 0.12 * lift)
                    .onGeometryChange(for: CGRect.self) {
                        $0.frame(in: .global)
                    } action: {
                        model.stageCardFrame = $0
                        cardWidth = $0.width
                    }
                    // Media dropped on the stage are INSTANTIATED in its
                    // work: in the selected layer when it is blank, else
                    // in a new one right above it (StageDrop).
                    .overlay {
                        if dropTargeted {
                            RoundedRectangle(
                                cornerRadius: model.stageCornerRadius,
                                style: .continuous)
                                .strokeBorder(Color.accentColor, lineWidth: 3)
                                .allowsHitTesting(false)
                        }
                    }
                    .onDrop(of: DroppedMedia.dropTypes,
                            delegate: StageDrop(model: model,
                                                targeted: $dropTargeted))
                    .overlay { dragRing }
                    // Over the ring: a click on a mark is the mark's.
                    .overlay(alignment: .topLeading) {
                        if marksShown, let c = model.guideCanvas,
                           let g = model.stageGuides,
                           let r = model.stage.video != nil
                               ? model.stageVideoRect
                                   ?? CGRect(origin: .zero, size: size)
                               : model.stageCanvasRect {
                            GuideMarks(model: model, size: size, rect: r,
                                       canvas: c, guides: g)
                        }
                    }
                    .animation(.smooth(duration: 0.2), value: ready)
                    .frame(width: size.width, height: size.height)
                    // A generation's progress over the card's foot, at its
                    // own width, not the card's: a narrow picture -- a
                    // portrait in a short window -- squeezed it into a tall
                    // capsule, its caption cut, its time on two lines.
                    // Wider than the card it lies over the stage around it,
                    // bounded by the prompt box (or the timeline) alone.
                    .overlay(alignment: .bottom) {
                        if !small && model.isGenerating {
                            GenerationStatus(
                                model: model,
                                width: min(GenerationStatus.defaultWidth,
                                           statusRoom(geo.size.width)))
                                .padding(.bottom, 10)
                                .transition(.opacity)
                        }
                    }
                    .position(x: geo.size.width / 2, y: geo.size.height / 2)
            }
            if small && model.stageHasTransport {
                SmallTransport(model: model)
                    .frame(height: SimpleView.smallBar)
                    .transition(.opacity)
            }
            if !small {
            // One view whatever it holds, the one bar fading into the
            // other in place: its frame and its going are the bar's. (A
            // Group's modifiers are each branch's: the compare bar going as
            // a clip's came could clear the frame the clip's had just set.)
            ZStack(alignment: .bottom) {
            if model.stage.video != nil && model.stage.clip == nil {
                VStack(spacing: 6) {
                    // Where the player is, over its transport -- as wide as
                    // the clip, the transport's width at least; the
                    // timeline's ruler scrubs while it is open.
                    if !model.timelineOpen {
                        StageScrubber(model: model)
                            .frame(maxWidth: max(360, cardWidth))
                            .transition(.opacity)
                    }
                    ClipBar(loops: model.loopClips,
                            toggle: { model.loopClips.toggle() },
                            // The timeline's transport is the fuller one.
                            transport: model.timelineOpen
                                ? nil : StageTransport(model: model),
                            // View only: nothing made from it.
                            // View only, or an agent at work holding the
                            // project: nothing made from it.
                            canContinue: model.canContinueClip
                                && !model.viewOnly && !model.agentHolds,
                            guides: model.guideLengths,
                            continueClip: { model.continueClip(seconds: $0) },
                            menuRequest: model.continueMenuRequest,
                            canGrab: model.clipOnStage && !model.isGenerating
                                && !model.viewOnly && !model.agentHolds,
                            grabFrame: model.grabFrame)
                }
                .transition(.opacity)
            } else {
                VStack(spacing: 6) {
                // A still's pages, where a clip's transport would be,
                // while the timeline that steps them is not shown.
                if model.stagePagesShown {
                    StagePages(model: model)
                        .transition(.opacity)
                }
                CompareBar(
                    mode: model.stage.mode,
                    hasB: model.stage.b != nil,
                    wipeLine: model.stage.wipeLine,
                    // Shown while the compare is on (the title bar's
                    // button, or a result that changed what was there).
                    takesPictures: model.compareOn,
                    current: model.currentSlot,
                    setMode: model.setStageMode,
                    toggleWipeLine: model.toggleStageWipeLine,
                    swap: model.swapStage,
                    returnToCurrent: model.returnToCurrent,
                    drop: { model.dropOnCompare($1, in: $0) })
                }
                .transition(.opacity)
            }
            }
            .onGeometryChange(for: CGRect.self) {
                $0.frame(in: .global)
            } action: {
                barFrame = $0
                model.stageBarFrame = $0
            }
            // Gone (the Prompt Editor's small stage has none): no click
            // kept by where it was.
            .onDisappear { model.stageBarFrame = .null }
            .transition(.opacity)
            }
        }
    }

    /// How wide the progress over the card may be: the prompt box's
    /// width -- the column's, at most -- or, while the timeline is open in
    /// its place, the timeline's (the stage's whole width).
    private func statusRoom(_ stage: CGFloat) -> CGFloat {
        model.timelineOpen ? stage : min(SimpleView.columnWidth, stage)
    }

    /// A clip's live preview loops; a finished clip plays; a picture is
    /// on the canvas (zoom, compare).
    @ViewBuilder
    private func media(_ growth: StageGrowth?) -> some View {
        if let clip = model.stage.clip {
            ClipView(clip: clip)
                .overlay {
                    if model.showsGuides && !small {
                        GuidesGrid(at: model.stageGuideFractions)
                    }
                }
                .overlay(alignment: .top) {
                    StageLabel(text: model.stage.aLabel)
                }
        } else if let url = model.stage.video {
            StagePlayer(model: model, url: url, small: small,
                        growth: growth)
                // The guides over the clip (View › Show Guides): the clip
                // fills the card, or lies where it is zoomed. Not over a
                // sound's waveform.
                .overlay {
                    if model.showsGuides && !small && !model.stageIsAudio {
                        GuidesGrid(at: model.stageGuideFractions,
                                   over: model.stageVideoRect)
                    }
                }
                // Markup on the clip (DESIGN §10a Markup): drawn at the
                // player's frame, the pointer in its canvas's pixels.
                // The Color panel's target tool takes the clip's pointer
                // the same way.
                .overlay {
                    let target = model.colorTargetReady
                    if !small && model.markupOnClip
                        && (model.markupReady || target) {
                        ClipMarkupView(
                            canvas: model.clipMarkupCanvas,
                            origin: model.clipMarkupOrigin,
                            shown: model.stageVideoRect,
                            overlay: model.markupOverlay,
                            cursor: target ? .crosshair
                                : model.markup.tool.cursor,
                            brushRadius: !target
                                && (model.markup.tool == .brush
                                    || model.markup.tool == .eraser)
                                ? model.markup.radius : 0,
                            brushSoftness: model.markup.softness,
                            onPointer: { e in
                                if model.colorTargetReady {
                                    model.colorTargetPointer(e)
                                } else {
                                    model.markupPointer(e)
                                }
                            },
                            onKey: { model.markupKey($0) },
                            canKey: { model.markupCan($0) })
                    }
                }
                // Not in the selected clip -- past its end, before its
                // start -- the timeline's black frames: a film's torn edge.
                .overlay {
                    if !small, let edge = model.clipEdge {
                        ClipEdgeMark(edge: edge)
                    }
                }
        } else {
            CompareCanvasView(
                imageA: model.stage.a, imageB: small ? nil : model.stage.b,
                surfaceA: model.stage.aSurface,
                surfaceB: small ? nil : model.stage.bSurface,
                fitA: model.stage.aFit, fitB: model.stage.bFit,
                wipeLine: model.stage.wipeLine,
                labelA: small ? "" : model.stage.aLabel,
                labelB: small ? "" : model.stage.bLabel,
                mode: small ? .a : model.stage.mode,
                fitRequest: model.stage.fitRequest,
                actualSizeRequest: model.stage.actualSizeRequest,
                zoomInRequest: model.stage.zoomInRequest,
                zoomOutRequest: model.stage.zoomOutRequest,
                unfitRequest: model.stage.unfitRequest,
                fitMargin: 1, growth: growth, background: nil,
                crop: model.stageCropPlacement,
                guides: model.showsGuides && !small,
                guidesAt: model.stageGuideFractions,
                onCanvasRect: { model.stageCanvasRect = $0 },
                // Markup's pointer -- or the Color panel's target tool's.
                markupActive: !small
                    && (model.markupReady || model.colorTargetReady),
                brushRadius: !model.colorTargetReady
                    && (model.markup.tool == .brush
                        || model.markup.tool == .eraser)
                    ? model.markup.radius : 0,
                brushSoftness: model.markup.softness,
                markupCursor: model.colorTargetReady ? .crosshair
                    : model.markup.tool.cursor,
                markupOverlay: model.markupOverlay,
                markupOrigin: model.stagePicture?.canvas.map {
                    CGPoint(x: $0.x, y: $0.y)
                } ?? .zero,
                onMarkupPointer: { e in
                    if model.colorTargetReady {
                        model.colorTargetPointer(e)
                    } else {
                        model.markupPointer(e)
                    }
                },
                onMarkupKey: { model.markupKey($0) },
                canMarkupKey: { model.markupCan($0) },
                onViewport: { model.stage.setViewport($0) },
                onModeFlip: { model.setStageMode($0) })
        }
    }

    private func card(_ growth: StageGrowth?) -> some View {
        media(growth)
            // What the stage shows when it is not the project's
            // composition (DESIGN §6a), named over its top left.
            .overlay(alignment: .topLeading) {
                if !small, let name = model.stageViewingName {
                    ViewingLabel(name: name)
                        .transition(.opacity)
                }
            }
            // An asset VIEWED from the list: ⓧ takes it off, the active
            // composition back (as Escape and a click elsewhere do).
            .overlay(alignment: .topTrailing) {
                if !small, model.viewingOnStage {
                    CloseViewingButton { model.endViewing() }
                        .transition(.opacity)
                }
            }
            // A clip's export, over the stage while it runs.
            .overlay {
                if model.exportShow != nil {
                    ExportOverlay(model: model, small: small)
                        .transition(.opacity)
                }
            }
            // A blank sound records into itself (DESIGN §7b).
            .overlay {
                if !small && model.showsCapture {
                    CaptureOverlay(model: model)
                        .transition(.opacity)
                }
            }
            // The small stage's progress: a thin bar in it. The stage's
            // is over the card, not in it (body).
            .overlay(alignment: .bottom) {
                if model.isGenerating && small {
                    PhaseBar(fraction: model.generationPhase?.fraction)
                        .progressViewStyle(.linear)
                        .padding(8)
                        .help(model.generationCaption)
                        .transition(.opacity)
                }
            }
            .background(Color(nsColor: .textBackgroundColor))
            // Rounded, or square to show the picture's own corners
            // (View › Round Stage Corners).
            .clipShape(RoundedRectangle(cornerRadius: model.stageCornerRadius,
                                        style: .continuous))
            .overlay(
                RoundedRectangle(cornerRadius: model.stageCornerRadius,
                                 style: .continuous)
                    .strokeBorder(Color.primary.opacity(0.14), lineWidth: 0.5))
    }

    /// The band just outside the image: hovering it readies the image to
    /// be dragged (the hand cursor), and a drag from it carries it --
    /// into the prompt, as the picture to edit or a reference. The image
    /// is the result, or the picture dropped on the stage to edit (so a
    /// base taken out of the prompt can go back in). Inside the image
    /// the canvas keeps the pointer for panning and zooming: the band's
    /// shape leaves the image out.
    ///
    /// Only where nothing else is: the band is cut to the editor's own
    /// area and leaves the compare bar out. Reaching 28 pt past a card
    /// that fills the stage, it lay over the markup toolbar's lower edge
    /// (drawn after it, so on top) and under the title bar -- where a
    /// SwiftUI hover still fires, tracking being by geometry alone -- and
    /// readied the drag from there.
    private var dragRing: some View {
        GeometryReader { g in
            let o = g.frame(in: .global).origin
            let local = { (r: CGRect) -> CGRect in
                r.isNull ? r : r.offsetBy(dx: -o.x, dy: -o.y)
            }
            // The guides' marks to the right and below: theirs.
            let marks = marksShown ? [
                CGRect(x: g.size.width - ringWidth, y: 0,
                       width: ringWidth, height: g.size.height),
                CGRect(x: 0, y: g.size.height - ringWidth,
                       width: g.size.width, height: ringWidth),
            ] : []
            let ring = RingShape(inset: ringWidth,
                                 radius: model.stageCornerRadius,
                                 open: local(area),
                                 cutouts: [local(barFrame)] + marks)
            let band = ring.path(in: CGRect(origin: .zero, size: g.size))
            Color.clear
                .contentShape(ring)
                // VALTZ_SNAPSHOT_RING=1: the band drawn, to see where it
                // reaches.
                .overlay {
                    if Self.showsBand {
                        ring.fill(Color.red.opacity(0.35))
                            .allowsHitTesting(false)
                    }
                }
                .onContinuousHover { phase in
                    switch phase {
                    case .active(let p):
                        model.stageDragReady = !model.navOpen
                            && band.contains(p)
                    case .ended:
                        model.stageDragReady = false
                    }
                }
                .pointerStyle(.grabIdle)
                .onDrag {
                    // Into the prompt, what it shows is CAPTURED -- every
                    // edit with it (AppModel.addStageToPrompt); anywhere
                    // else, the file of what it shows, flattened.
                    let provider = model.stageDragFile()
                        .map { NSItemProvider(object: $0 as NSURL) }
                        ?? NSItemProvider()
                    provider.registerObject(
                        AppModel.stagePrefix as NSString,
                        visibility: .ownProcess)
                    return provider
                } preview: {
                    dragPreview
                }
        }
        .padding(-ringWidth)
        .allowsHitTesting(model.canDragStage)
    }

    @ViewBuilder
    private var dragPreview: some View {
        if let img = model.stage.a {
            Image(decorative: img, scale: 1)
                .resizable()
                .scaledToFit()
                .frame(width: 180, height: 180)
                .clipShape(RoundedRectangle(cornerRadius: 8,
                                            style: .continuous))
        }
    }

    private func fitted(aspect: CGFloat, in box: CGSize) -> CGSize {
        fittedSize(aspect: aspect, in: box)
    }
}

/// The largest size of `aspect` inside `box`.
private func fittedSize(aspect: CGFloat, in box: CGSize) -> CGSize {
    guard box.width > 0, box.height > 0, aspect > 0 else { return box }
    let w = min(box.width, box.height * aspect)
    return CGSize(width: w, height: w / aspect)
}


/// Continue's menu while it is open (a scripted snapshot reads and closes
/// it).
@MainActor
enum ClipBarMenu {
    static weak var opened: NSMenu?
}

/// What a finished clip offers under the stage, in the compare bar's
/// style: whether it plays once or loops, its TRANSPORT (StageTransport;
/// none while the timeline, whose own is the fuller one, is open), a
/// still of the frame shown, and -- where a model reads clips (Ref2VA)
/// -- carrying it on from its last seconds, the guide's length chosen
/// from a menu.
private struct ClipBar: View {
    let loops: Bool
    let toggle: () -> Void
    var transport: StageTransport?
    var canContinue = false
    var guides: [AppModel.GuideLength] = []
    var continueClip: (Int) -> Void = { _ in }
    /// A scripted snapshot opening the menu (VALTZ_SNAPSHOT_CONTINUE_MENU).
    var menuRequest = 0
    var canGrab = false
    var grabFrame: () -> Void = {}
    @State private var anchor = MenuAnchor()

    var body: some View {
        HStack(spacing: 10) {
            TitleBarButton(symbol: "repeat", title: "Loop",
                           help: loops ? "Play the clip once"
                                       : "Play the clip again and again",
                           on: loops, action: toggle)
                .titleBarCapsule()
            if let transport {
                transport
                    .transition(.opacity)
            }
            if canGrab {
                TitleBarButton(symbol: "camera.viewfinder",
                               title: "Grab Frame",
                               help: "Grab this frame as a still, into the prompt",
                               on: false, action: grabFrame)
                    .titleBarCapsule()
            }
            if canContinue {
                TitleBarButton(symbol: "arrow.right.to.line",
                               title: "Continue",
                               help: "Continue the clip from its last seconds, picture and sound",
                               on: false, action: openMenu)
                    .background(MenuAnchorView(anchor: anchor))
                    .titleBarCapsule()
                    .onChange(of: menuRequest) { openMenu() }
            }
        }
        .transition(.opacity.combined(with: .scale(scale: 0.95)))
    }

    /// The guide's lengths: what was asked, and under it what the model
    /// takes ("2.3 s · 56 frames").
    private func openMenu() {
        guard !guides.isEmpty else { return }
        let menu = NSMenu()
        let head = NSMenuItem(
            title: String(localized: "Continue from the Clip's Last"),
            action: nil, keyEquivalent: "")
        head.isEnabled = false
        menu.addItem(head)
        for g in guides {
            let item = MenuAction.item(
                String(localized: "\(String(g.asked)) Seconds"),
                symbol: "arrow.right.to.line") { [continueClip] in
                continueClip(g.asked)
            }
            let s = String(format: "%.1f", g.seconds)
            let n = String(g.frames)
            item.subtitle = String(localized: "\(s) s · \(n) frames")
            menu.addItem(item)
        }
        ClipBarMenu.opened = menu
        if let v = anchor.view, v.window != nil {
            menu.popUp(positioning: nil,
                       at: NSPoint(x: 0, y: v.isFlipped ? v.bounds.maxY + 6
                                                        : -6),
                       in: v)
        }
    }
}

/// The stage's transport, in the row under a finished clip (or a sound,
/// a timeline) -- in place of AVKit's floating controls, which came and
/// went over the picture unsteadily and lay in the way:
///
///   🔈    |<   <<   ◁|   ▶ / ‖   |▷   >>   >|
///
/// the timeline's nine (Timeline.transport) less its backward play, play
/// and pause one button in the middle, the player's sound off or on at
/// its left. |< and >| go from event to event of the composition worked
/// on, else to the clip's start and end; a sound steps 10 ms (⌥ 1 ms).
/// Their keys are Valtz's (AppModel.trimKey, playerKey): ⌘← ← Space → ⌘→,
/// Home and End the start and the end.
struct StageTransport: View {
    @Bindable var model: AppModel

    var body: some View {
        TransportButtons(
            rate: model.videoRate, muted: model.videoMuted,
            sound: model.stageIsAudio, events: model.timelineAsset != nil,
            act: .init(
                mute: act(model.videoMuted ? "unmute" : "mute",
                          model.toggleMute),
                previous: act("previous event") {
                    model.stageEvent(next: false)
                },
                rewind: act("fast rewind", model.fastRewind),
                stepBack: act("step back") { model.stepVideo(-1) },
                playPause: act(model.videoRate != 0 ? "pause" : "play") {
                    model.playVideo(rate: model.videoRate != 0 ? 0 : 1)
                },
                stepForward: act("step forward") { model.stepVideo(1) },
                forward: act("fast forward", model.fastForward),
                next: act("next event") { model.stageEvent(next: true) }))
        // A voice-over's take under way: the player plays on, muted.
        .disabled(model.voiceOverRecording)
    }

    /// A button's action, said in the system log as it runs (stage-bar).
    private func act(_ name: String,
                     _ f: @escaping () -> Void) -> () -> Void {
        { [model] in
            model.noteBar("transport: \(name)")
            f()
        }
    }
}

/// The transport's buttons, as the stage's row and the Atlas's player
/// show them: the sound off or on, then |< << ◁| ▶/‖ |▷ >> >| -- |< >|
/// from event to event (`events`), else the start and the end; a sound
/// steps 10 ms. Their keys are the ⌘? labels' (AppModel.trimKey,
/// playerKey; atlasKey).
struct TransportButtons: View {
    struct Actions {
        var mute: () -> Void
        var previous: () -> Void
        var rewind: () -> Void
        var stepBack: () -> Void
        var playPause: () -> Void
        var stepForward: () -> Void
        var forward: () -> Void
        var next: () -> Void
    }
    let rate: Float
    let muted: Bool
    let sound: Bool
    let events: Bool
    let act: Actions

    var body: some View {
        HStack(spacing: 10) {
            // The player's sound, off or on: what plays heard or not, its
            // layers' own volumes as they are.
            TitleBarButton(
                symbol: muted ? "speaker.slash.fill" : "speaker.wave.2.fill",
                title: muted ? "Unmute" : "Mute",
                help: muted ? "Turn the sound on" : "Turn the sound off",
                on: muted, action: act.mute)
                .titleBarCapsule()
            buttons.titleBarCapsule()
        }
    }

    private var buttons: some View {
        let r = rate
        return HStack(spacing: 0) {
            TitleBarButton(
                symbol: "backward.end.fill", title: "Previous Event",
                help: events
                    ? "Previous event: a key, where a layer begins or ends, the start"
                    : "Go to the start",
                action: act.previous)
                .shortcutHint("⌘←")
            TitleBarButton(symbol: "backward.fill", title: "Fast Rewind",
                           help: "Fast rewind", on: r < -1,
                           action: act.rewind)
            TitleBarButton(
                symbol: "backward.frame.fill", title: "Step Back",
                help: sound ? "Back 10 ms (with ⌥, 1 ms)" : "Back one frame",
                action: act.stepBack)
                .shortcutHint("←")
            TitleBarButton(
                symbol: r != 0 ? "pause.fill" : "play.fill",
                title: r != 0 ? "Pause" : "Play",
                help: r != 0 ? "Pause (Space)" : "Play (Space)",
                action: act.playPause)
                .shortcutHint(String(localized: "Space"))
            TitleBarButton(
                symbol: "forward.frame.fill", title: "Step Forward",
                help: sound ? "On 10 ms (with ⌥, 1 ms)" : "On one frame",
                action: act.stepForward)
                .shortcutHint("→")
            TitleBarButton(symbol: "forward.fill", title: "Fast Forward",
                           help: "Fast forward", on: r > 1,
                           action: act.forward)
            TitleBarButton(
                symbol: "forward.end.fill", title: "Next Event",
                help: events
                    ? "Next event: a key, where a layer begins or ends, the end"
                    : "Go to the end",
                action: act.next)
                .shortcutHint("⌘→")
        }
    }
}

/// A caption over a clip's preview, as the canvas captions a picture
/// (which it draws itself): small white type on a dark plate, centred at
/// the top.
private struct StageLabel: View {
    let text: String

    var body: some View {
        if !text.isEmpty {
            Text(verbatim: text)
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.white)
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .background(.black.opacity(0.55),
                            in: RoundedRectangle(cornerRadius: 4))
                .padding(.top, 8)
                .allowsHitTesting(false)
        }
    }
}

/// ⓧ over the stage's top right while an asset from the list is viewed:
/// back to the composition being edited.
private struct CloseViewingButton: View {
    let close: () -> Void

    var body: some View {
        Button(action: close) {
            Image(systemName: "xmark.circle.fill")
                .font(.system(size: 20))
                .symbolRenderingMode(.palette)
                .foregroundStyle(.white, .black.opacity(0.55))
        }
        .buttonStyle(.plain)
        .padding(8)
        .help(String(localized: "Close the preview: back to the composition being edited"))
        .accessibilityLabel(Text("Close the Preview"))
    }
}

/// "Viewing <name>": the stage shows an asset that is not the project's
/// composition -- a result of another kind, an asset set active, the
/// picture to edit. Small, over its top left, out of the pointer's way.
private struct ViewingLabel: View {
    let name: String

    var body: some View {
        HStack(spacing: 4) {
            Image(systemName: "eye")
                .font(.system(size: 10, weight: .semibold))
            Text("Viewing \(name)")
                .font(.system(size: 11, weight: .medium))
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 7)
        .padding(.vertical, 3)
        .background(.black.opacity(0.55),
                    in: RoundedRectangle(cornerRadius: 4))
        .frame(maxWidth: 360, alignment: .leading)
        .padding(.top, 8)
        .padding(.leading, 8)
        .allowsHitTesting(false)
        .accessibilityLabel(Text("Viewing \(name)"))
    }
}

/// A frame with its middle cut out: the band around the result that the
/// drag handle lives in -- within `open` (the editor's area) and outside
/// each of `cutouts` (other controls), all in the band's coordinates.
private struct RingShape: Shape {
    let inset: CGFloat
    /// The image's corners, as the stage draws them.
    var radius: CGFloat = 10
    var open: CGRect = .infinite
    var cutouts: [CGRect] = []

    func path(in rect: CGRect) -> Path {
        let image = Path(roundedRect: rect.insetBy(dx: inset, dy: inset),
                         cornerSize: CGSize(width: radius, height: radius),
                         style: .continuous)
        guard !open.isNull else { return Path() }
        var p = Path(rect).subtracting(image)
            .intersection(Path(open.intersection(rect)))
        for c in cutouts where !c.isNull {
            p = p.subtracting(Path(c))
        }
        return p
    }
}

/// The assistant's rewrite, offered -- never applied without asking.
/// A generation refused for lack of memory: what vpipe asked for -- the
/// refused step part by part, each budget's need and room, the plan by
/// phase -- and what to change: a shorter clip, a smaller size, fewer
/// references, each also marked where it is set (the generation card's
/// rows, the reference row) while it is still as it failed.
struct MemoryFailureCard: View {
    @Bindable var model: AppModel
    let failure: MemoryFailure

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline, spacing: 8) {
                Image(systemName: "exclamationmark.triangle.fill")
                    .foregroundStyle(.orange)
                Text(failure.message)
                    .font(.callout.weight(.medium))
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 8)
                Button {
                    model.dismissMemoryFailure()
                } label: {
                    Image(systemName: "xmark")
                }
                .buttonStyle(.borderless)
                .help("Dismiss")
            }
            breakdown
            if !suggestions.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    Text("To make it fit")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                    HStack(spacing: 8) {
                        ForEach(suggestions, id: \.title) { s in
                            Button(action: s.action) {
                                HStack(spacing: 5) {
                                    Image(systemName: s.done
                                          ? "checkmark.circle.fill" : s.symbol)
                                    VStack(alignment: .leading, spacing: 0) {
                                        Text(s.title)
                                        Text(s.now)
                                            .font(.caption2)
                                            .foregroundStyle(.secondary)
                                    }
                                }
                            }
                            .buttonStyle(.glass)
                            .tint(s.done ? .green : .orange)
                            .help(s.help)
                        }
                    }
                    .buttonBorderShape(.roundedRectangle(radius: 10))
                }
            }
        }
        .padding(16)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 18,
                                                    style: .continuous))
    }

    /// What the refused step asked for, and each budget against it; the
    /// plan, by phase, beneath.
    private var breakdown: some View {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 3) {
            GridRow {
                Text(failure.step == "decode"
                     ? String(localized: "Decoding asked for")
                     : String(localized: "Denoising asked for"))
                    .foregroundStyle(.secondary)
                Text(MemoryFailure.size(failure.need))
                    .monospacedDigit()
                    .gridColumnAlignment(.trailing)
            }
            ForEach(failure.parts, id: \.name) { p in
                GridRow {
                    Text(MemoryFailure.partName(p.name))
                        .padding(.leading, 12)
                    Text(MemoryFailure.size(p.bytes)).monospacedDigit()
                }
            }
            ForEach(failure.gates, id: \.name) { g in
                GridRow {
                    Text(MemoryFailure.partName(g.name))
                    Text(String(localized: "\(MemoryFailure.size(g.have)) available"))
                        .monospacedDigit()
                    Text(g.ok ? String(localized: "enough")
                         : String(localized: "\(MemoryFailure.size(g.need)) wanted"))
                        .monospacedDigit()
                        .foregroundStyle(g.ok ? Color.secondary : .red)
                }
            }
            if failure.planPeak > 0 {
                GridRow {
                    Text("Planned before loading")
                        .foregroundStyle(.secondary)
                    Text(MemoryFailure.size(failure.planPeak)).monospacedDigit()
                    Text(String(localized: "of \(MemoryFailure.size(failure.ram))"))
                        .foregroundStyle(.secondary)
                }
                ForEach(failure.phases, id: \.name) { p in
                    GridRow {
                        Text(MemoryFailure.partName(p.name))
                            .padding(.leading, 12)
                            .fontWeight(p.name == failure.planPhase
                                        ? .semibold : .regular)
                        Text(MemoryFailure.size(p.bytes)).monospacedDigit()
                    }
                }
            }
        }
        .font(.caption)
    }

    private struct Suggestion {
        let title: String
        let now: String
        let symbol: String
        let help: String
        /// Lowered already: no longer as it failed.
        let done: Bool
        let action: () -> Void
    }

    private var suggestions: [Suggestion] {
        var out: [Suggestion] = []
        if let s = failure.seconds, model.activeModality == .video {
            out.append(Suggestion(
                title: String(localized: "A shorter clip"),
                now: String(localized: "was \(String(format: "%.1f", s)) s"),
                symbol: "timer",
                help: String(localized: "Length, in the generation card: the memory a clip needs grows with its frames"),
                done: !model.suggestsShorterClip,
                action: model.showMemorySuggestion))
        }
        if let w = failure.width, let h = failure.height {
            out.append(Suggestion(
                title: String(localized: "A smaller size"),
                now: String(localized: "was \(String(w)) × \(String(h))"),
                symbol: "arrow.down.right.and.arrow.up.left",
                help: String(localized: "Size, in the generation card: the memory grows with the pixels of each frame"),
                done: !model.suggestsSmallerSize,
                action: model.showMemorySuggestion))
        }
        if let n = failure.references, n > 0 {
            out.append(Suggestion(
                title: String(localized: "Fewer references"),
                now: String(localized: "was \(String(n))"),
                symbol: "photo.stack",
                help: String(localized: "The row over the prompt: each reference is read alongside the clip"),
                done: !model.suggestsFewerReferences,
                action: {}))
        }
        return out
    }
}

struct SuggestionCard: View {
    let prompt: String
    /// Still being written: its words as far as they have come.
    var writing = false
    /// What the helper read and wrote, how fast -- shown while Settings ›
    /// Agentic Helper › Show the Helper's Speed is on.
    var stats: AgentTurnStats?
    /// How tall its words stand before they scroll (`textCap`).
    var maxTextHeight: CGFloat = 160
    let accept: () -> Void
    let dismiss: () -> Void
    /// The Prompt Editor, the suggestion beside the prompt: read whole,
    /// edited, applied there.
    var review: (() -> Void)?

    /// The words' height in the box: whole lines, as many as a sixth of
    /// the window's height holds, three to ten. A long suggestion -- a
    /// clip's, in its model's sections -- scrolls rather than pushing the
    /// stage and the prompt out of the window.
    static func textCap(window: CGFloat) -> CGFloat {
        let line = SuggestionText.lineHeight
        let n = min(10, max(3, Int(window / 6 / line)))
        return CGFloat(n) * line
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 6) {
                Label("Suggested prompt", systemImage: "sparkles")
                if writing {
                    ProgressView().controlSize(.mini)
                    Text("Writing…")
                }
            }
            .font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
            if !prompt.isEmpty {
                SuggestionText(text: prompt, writing: writing,
                               maxHeight: maxTextHeight)
            }
            if let stats, !stats.isEmpty {
                AgentSpeedText(stats: stats)
            }
            HStack {
                if let review {
                    Button(action: review) {
                        Label("Prompt Editor",
                              systemImage: "rectangle.expand.vertical")
                    }
                    .buttonStyle(.glass)
                    .help("Read and edit the suggestion beside the prompt, in the Prompt Editor (⇧⌘E)")
                }
                Spacer()
                Button(writing ? "Stop" : "Dismiss", action: dismiss)
                    .buttonStyle(.glass)
                    .help(writing ? "Stop the assistant; the prompt stays as it is"
                                  : "Keep the prompt as it is")
                Button("Use Prompt", action: accept)
                    .buttonStyle(.glassProminent)
                    .disabled(writing)
            }
            .buttonBorderShape(.roundedRectangle(radius: 10))
        }
        .padding(16)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 18,
                                                    style: .continuous))
    }
}

/// The suggestion's words in the box's card: read only, selectable, as
/// tall as they wrap up to `maxHeight` -- past it they scroll, with the
/// compact prompt's scroll bar, there while they do. The assistant's
/// words are followed at the end as they come.
struct SuggestionText: NSViewRepresentable {
    let text: String
    var writing = false
    let maxHeight: CGFloat

    static let font = NSFont.preferredFont(forTextStyle: .callout)
    static let lineHeight = NSLayoutManager().defaultLineHeight(for: font)
    static var attributes: [NSAttributedString.Key: Any] {
        [.font: font, .foregroundColor: NSColor.labelColor]
    }

    @MainActor
    final class Coordinator {
        weak var textView: NSTextView?
        var shown = ""
        /// The words laid out apart, at the width SwiftUI offers: their
        /// height, as the text view will lay them.
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        let container = NSTextContainer(
            size: NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude))

        init() {
            container.lineFragmentPadding = 0
            layout.addTextContainer(container)
            storage.addLayoutManager(layout)
        }

        func height(of text: String, width: CGFloat) -> CGFloat {
            if storage.string != text {
                storage.setAttributedString(NSAttributedString(
                    string: text, attributes: SuggestionText.attributes))
            }
            container.size = NSSize(width: width,
                                    height: CGFloat.greatestFiniteMagnitude)
            layout.ensureLayout(for: container)
            return ceil(layout.usedRect(for: container).height)
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> SuggestionScrollView {
        // TextKit 1, as the measure's: the same lines.
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        storage.addLayoutManager(layout)
        let container = NSTextContainer(
            size: NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude))
        container.widthTracksTextView = true
        container.lineFragmentPadding = 0
        layout.addTextContainer(container)
        let tv = NSTextView(frame: .zero, textContainer: container)
        tv.isEditable = false
        tv.isSelectable = true
        tv.isRichText = false
        tv.drawsBackground = false
        tv.isVerticallyResizable = true
        tv.isHorizontallyResizable = false
        tv.minSize = .zero
        tv.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                            height: CGFloat.greatestFiniteMagnitude)
        tv.autoresizingMask = [.width]
        tv.textContainerInset = .zero
        tv.font = Self.font
        tv.textColor = .labelColor
        context.coordinator.textView = tv

        let scroll = SuggestionScrollView()
        scroll.documentView = tv
        scroll.drawsBackground = false
        scroll.automaticallyAdjustsContentInsets = false
        scroll.verticalScroller = KnobScroller()
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay
        return scroll
    }

    func updateNSView(_ scroll: SuggestionScrollView, context: Context) {
        let c = context.coordinator
        guard let tv = c.textView, text != c.shown else { return }
        let was = c.shown
        c.shown = text
        if !was.isEmpty && text.hasPrefix(was) {
            // Written on: what is new, at the end -- followed while the
            // assistant writes.
            tv.textStorage?.append(NSAttributedString(
                string: String(text.dropFirst(was.count)),
                attributes: Self.attributes))
            if writing { tv.scrollToEndOfDocument(nil) }
        } else {
            tv.textStorage?.setAttributedString(NSAttributedString(
                string: text, attributes: Self.attributes))
            tv.scrollRangeToVisible(NSRange(location: 0, length: 0))
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize,
                      nsView: SuggestionScrollView,
                      context: Context) -> CGSize? {
        guard let w = proposal.width, w > 0, w.isFinite else { return nil }
        let h = context.coordinator.height(of: text, width: w)
        return CGSize(width: w,
                      height: min(maxHeight, max(Self.lineHeight, h)))
    }
}

/// The suggestion's scroll view: cut short, a scroll bar that stays (the
/// legacy style, as the compact prompt's); whole, none. The legacy bar
/// narrows the text, which only makes it longer, so this settles.
final class SuggestionScrollView: NSScrollView {
    override func tile() {
        super.tile()
        fitScroller()
    }

    override func reflectScrolledClipView(_ clip: NSClipView) {
        super.reflectScrolledClipView(clip)
        fitScroller()
    }

    private func fitScroller() {
        guard let doc = documentView else { return }
        let cut = doc.frame.height > contentView.bounds.height + 0.5
        let style: NSScroller.Style = cut ? .legacy : .overlay
        if scrollerStyle != style {
            scrollerStyle = style
            autohidesScrollers = !cut
        }
    }
}
