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
        .onGeometryChange(for: CGRect.self) {
            $0.frame(in: .global)
        } action: { area = $0 }
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
    /// assistant's suggestion is beside the prompt and the inspector is
    /// shut, the card reaches right to the window's edge, a status bar's
    /// height from it, its left edge where it was.
    private func column(_ immersive: Bool, size: CGSize) -> some View {
        let base = min(Self.columnWidth, size.width - 80)
        let wide = immersive && model.suggesting && !model.inspectorOpen
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
                        .padding(.bottom, 22)
                } else {
                    Spacer(minLength: 20)
                    // Ahead of the spacers for room, but it shrinks when a
                    // tall prompt (or open drawer) needs the height.
                    // The mark takes no drops: media go into the prompt,
                    // or onto a stage's layers.
                    HeroMark(sweep: model.markSweep)
                        // A fleet job's overlay shows the mark, in blue.
                        .opacity(model.fleetServing == nil ? 1 : 0)
                        .layoutPriority(1)
                        .padding(24)
                        .transition(.scale(scale: 0.8)
                            .combined(with: .opacity))
                        .padding(.bottom, 41 - 24)
                        .onAppear { model.startupSweep() }
                    // Before the first frame: what it is doing.
                    if model.isGenerating {
                        GenerationStatus(model: model)
                            .padding(.bottom, 20)
                            .transition(.opacity)
                    }
                }
                if let f = model.memoryFailure, !model.suggesting {
                    MemoryFailureCard(model: model, failure: f)
                        .frame(maxWidth: Self.columnWidth)
                        .padding(.bottom, 14)
                        .transition(.move(edge: .bottom)
                            .combined(with: .opacity))
                }
                if model.suggesting {
                    SuggestionCard(prompt: model.enhanced?.prompt
                                       ?? model.enhanceDraft,
                                   writing: model.suggestionWriting,
                                   accept: model.acceptEnhanced,
                                   dismiss: model.dismissEnhanced)
                        .frame(maxWidth: Self.columnWidth)
                        .padding(.bottom, 14)
                        .transition(.move(edge: .bottom)
                            .combined(with: .opacity))
                }
            }
            ComposerStack(model: model, maxPromptHeight: size.height / 2,
                          immersive: immersive, cardWidth: card)
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
            if !immersive {
                if let banner = model.banner {
                    Text(banner)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .padding(.top, 12)
                        .transition(.opacity)
                }
                if !model.stageVisible {
                    Spacer(minLength: 20)
                }
            }
        }
        .padding(.horizontal, 40)
        .padding(.top, immersive ? 0 : 12)
        .padding(.bottom, immersive ? StatusBar.height
                 : model.stageVisible ? 28 : 0)
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
            Group {
                if model.stageVisible {
                    Color.clear
                        .frame(width: w, height: w / aspect)
                        .anchorPreference(key: StageSlotKey.self,
                                          value: .bounds) { $0 }
                } else if model.isGenerating {
                    CompactStatus(model: model)
                        .frame(width: room)
                        .transition(.opacity)
                }
            }
            .padding(.leading, 16)
            .padding(.top, 16)
        }
    }
}

/// Where the stage goes: the slot above the prompt, or the editor's top
/// left.
private struct StageSlotKey: PreferenceKey {
    static let defaultValue: Anchor<CGRect>? = nil
    static func reduce(value: inout Anchor<CGRect>?,
                       nextValue: () -> Anchor<CGRect>?) {
        value = nextValue() ?? value
    }
}

/// A generation's progress where the small stage would be: a bar and
/// its phase.
private struct CompactStatus: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            ProgressView(value: model.generationPhase?.fraction)
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

    /// How far outside the image the drag ring reaches.
    private let ringWidth: CGFloat = 28
    private static let showsBand =
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT_RING"] != nil

    var body: some View {
        let ready = model.stageDragReady && model.canDragStage
        VStack(spacing: small ? 0 : 16) {
            GeometryReader { geo in
                let size = fitted(aspect: model.stage.layoutAspect,
                                  in: geo.size)
                let lift = model.stageLift
                card
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
                    } action: { model.stageCardFrame = $0 }
                    // Media dropped on the stage are INSTANTIATED in its
                    // work: in the selected layer when it is blank, else
                    // in a new one right above it (a file imported first).
                    .overlay {
                        if dropTargeted {
                            RoundedRectangle(
                                cornerRadius: model.stageCornerRadius,
                                style: .continuous)
                                .strokeBorder(Color.accentColor, lineWidth: 3)
                                .allowsHitTesting(false)
                        }
                    }
                    .dropDestination(for: URL.self) { urls, _ in
                        // An asset of the list arrives as a URL too.
                        if let id = AppModel.draggedAssets(urls).first {
                            return model.dropAssetOnStage(id)
                        }
                        return model.dropFilesOnStage(
                            urls.filter(\.isFileURL))
                    } isTargeted: { dropTargeted = $0 }
                    .dropDestination(for: String.self) { items, _ in
                        items.compactMap { AppModel.draggedAsset($0) }
                            .first.map { model.dropAssetOnStage($0) } ?? false
                    } isTargeted: { dropTargeted = $0 }
                    .overlay { dragRing }
                    .animation(.smooth(duration: 0.2), value: ready)
                    .frame(width: size.width, height: size.height)
                    .position(x: geo.size.width / 2, y: geo.size.height / 2)
            }
            if !small {
            Group {
            if model.stage.video != nil && model.stage.clip == nil {
                ClipBar(loops: model.loopClips,
                        toggle: { model.loopClips.toggle() },
                        canContinue: model.canContinueClip,
                        guides: model.guideLengths,
                        continueClip: { model.continueClip(seconds: $0) },
                        menuRequest: model.continueMenuRequest,
                        canGrab: model.clipOnStage && !model.isGenerating,
                        grabFrame: model.grabFrame)
            } else {
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
            }
            .onGeometryChange(for: CGRect.self) {
                $0.frame(in: .global)
            } action: { barFrame = $0 }
            .transition(.opacity)
            }
        }
    }

    /// A clip's live preview loops; a finished clip plays; a picture is
    /// on the canvas (zoom, compare).
    @ViewBuilder
    private var media: some View {
        if let clip = model.stage.clip {
            ClipView(clip: clip)
                .overlay {
                    if model.showsGuides && !small { GuidesGrid() }
                }
                .overlay(alignment: .top) {
                    StageLabel(text: model.stage.aLabel)
                }
        } else if let url = model.stage.video {
            VideoPlayerView(url: url, loops: model.loopClips,
                            rate: model.stageFrameRate,
                            command: model.videoCommand,
                            look: model.stackPlayback == nil
                                ? model.stageClipLook : nil,
                            stack: model.stackPlayback,
                            artwork: model.stageIsAudio ? model.stage.a : nil,
                            core: model.core,
                            onFrame: { model.videoFrameChanged($0) },
                            onRate: { model.videoRate = $0 })
                // The guides over the clip (View › Show Guides): the clip
                // fills the card. Not over a sound's waveform.
                .overlay {
                    if model.showsGuides && !small && !model.stageIsAudio {
                        GuidesGrid()
                    }
                }
                // Markup on the clip (DESIGN §10a Markup): drawn at the
                // player's frame, the pointer in its canvas's pixels.
                .overlay {
                    if !small && model.markupReady && model.markupOnClip {
                        ClipMarkupView(
                            canvas: model.clipMarkupCanvas,
                            origin: model.clipMarkupOrigin,
                            overlay: model.markupOverlay,
                            cursor: model.markup.tool.cursor,
                            brushRadius: model.markup.tool == .brush
                                || model.markup.tool == .eraser
                                ? model.markup.radius : 0,
                            brushSoftness: model.markup.softness,
                            onPointer: { model.markupPointer($0) },
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
                fitMargin: 1, background: nil,
                crop: model.stageCropPlacement,
                guides: model.showsGuides && !small,
                markupActive: !small && model.markupReady,
                brushRadius: model.markup.tool == .brush
                    || model.markup.tool == .eraser ? model.markup.radius : 0,
                brushSoftness: model.markup.softness,
                markupCursor: model.markup.tool.cursor,
                markupOverlay: model.markupOverlay,
                markupOrigin: model.stagePicture?.canvas.map {
                    CGPoint(x: $0.x, y: $0.y)
                } ?? .zero,
                onMarkupPointer: { model.markupPointer($0) },
                onMarkupKey: { model.markupKey($0) },
                canMarkupKey: { model.markupCan($0) },
                onViewport: { model.stage.setViewport($0) },
                onModeFlip: { model.setStageMode($0) })
        }
    }

    private var card: some View {
        media
            // What the stage shows when it is not the project's
            // composition (DESIGN §6a), named over its top left.
            .overlay(alignment: .topLeading) {
                if !small, let name = model.stageViewingName {
                    ViewingLabel(name: name)
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
            .overlay(alignment: .bottom) {
                if model.isGenerating {
                    if small {
                        ProgressView(value: model.generationPhase?.fraction)
                            .progressViewStyle(.linear)
                            .padding(8)
                            .help(model.generationCaption)
                            .transition(.opacity)
                    } else {
                        GenerationStatus(model: model)
                            .padding(.bottom, 10)
                            .transition(.opacity)
                    }
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
            let ring = RingShape(inset: ringWidth,
                                 radius: model.stageCornerRadius,
                                 open: local(area),
                                 cutouts: [local(barFrame)])
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
/// style: whether it plays once or loops, a still of the frame shown, and
/// -- where a model reads clips (Ref2VA) -- carrying it on from its last
/// seconds, the guide's length chosen from a menu. (Play, the scrubber,
/// volume and full screen float over the clip itself.)
private struct ClipBar: View {
    let loops: Bool
    let toggle: () -> Void
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
    let accept: () -> Void
    let dismiss: () -> Void

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
                Text(prompt)
                    .font(.callout)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            HStack {
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
