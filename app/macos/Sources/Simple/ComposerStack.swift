import SwiftUI
import UniformTypeIdentifiers

/// The prompt card, the tab above it, and the tray of cards tucked under
/// it.
///
///   modality tab  Video | Image, on a grey tab over the card's top-left
///                 corner: what Start makes, chosen before nearly every
///                 Start, so the generation card can stay shut
///   prompt card   the prompt: its staged media in a row on top
///                 (ReferenceRow), split from the text by a hairline, and
///                 the text, which mentions them; attach and the
///                 assistant sit in its bottom-right corner
///   tray          cards hanging from under the prompt card, each a
///                 header at its bottom edge -- Adjust, Crop (Trim, for
///                 a clip), and the generation brief (model · size ·
///                 speed) with Start -- on one row, docked right with a
///                 small gap between
///
/// A header's chevron pulls its card out like a drawer: the card's body
/// slides down from under the prompt card, pushing the header row down,
/// and the header stays joined to it by concave rounded corners. The
/// other card stays behind, its header riding along on the same row and
/// shaded by the open card. At most one is open.
///
/// The tray is narrower than the prompt card, so its edges start well
/// inside the card's rounded corners, and it casts a shallower shadow.
/// Its silhouettes are drawn in one layer behind the content, from the
/// headers' measured frames, so the open card, its header and the other
/// header can be stacked in the order a drawer implies. Each frame is
/// taken against the tray's own, measured the same way: while the
/// composer itself moves (the inspector opening, a result landing), an
/// anchor resolves in the layer's space with that move still to come --
/// a header read 150 pt right of where it showed, then slid back -- and
/// the tray's anchor carries the same offset, which cancels.
struct ComposerStack: View {
    @Bindable var model: AppModel
    /// How tall the prompt card may grow: half the window.
    var maxPromptHeight: CGFloat = 240
    /// The Prompt Editor (DESIGN §10c): the card alone, the height it is
    /// given, its top square against the prompt toolbar and its tabs
    /// down its left side; no modality tab, no tray.
    var immersive = false
    /// The card's width in the editor (SimpleView sets it: wider while a
    /// suggestion is beside the prompt and the inspector is shut).
    var cardWidth: CGFloat = 680
    @State private var promptHeight: CGFloat = 60
    @State private var promptLines = 1
    @State private var dropTargeted = false
    /// The attach capture's card, where it lies in the prompt card; and
    /// its GHOST flying into the thumbnail it became.
    @State private var captureFrame: CGRect = .zero
    @State private var captureGhost: CGRect?
    @State private var ghostFlying = false
    /// The prompt card's coordinates: the card and the row's thumbnails.
    static let cardSpace = "promptCard"
    /// The card last opened: it stays in front while it closes.
    @State private var lastPanel: ComposerPanel = .generate

    private let radius: CGFloat = 20
    private let tabRadius: CGFloat = 18
    /// The concave corners where an open card's header meets its body.
    private let fillet: CGFloat = 12
    /// How far the tray tucks under the prompt card.
    private let tuck: CGFloat = 22
    /// The tray's inset from each side of the card: past its corner.
    private let trayInset: CGFloat = 30
    /// The height of a header.
    private let tabHeight: CGFloat = 37
    /// Between the two headers.
    private let tabGap: CGFloat = 8

    /// The timeline in the prompt's place (DESIGN §10a Timeline): no
    /// modality tab, no generation card -- the other cards tucked under it
    /// as under the prompt.
    private var timeline: Bool { !immersive && model.timelineOpen }

    var body: some View {
        // The tab first: its foot, under the card, is drawn behind it.
        VStack(alignment: .leading, spacing: 0) {
            if !immersive && !timeline {
                ModalityTab(model: model)
                    .transition(.opacity)
            }
            ZStack(alignment: .top) {
                if !immersive {
                    tray
                        // Under the wide timeline, where it is under the
                        // prompt: its width, centred, its headers at the
                        // prompt's right -- not out at the timeline's.
                        .frame(maxWidth: timeline
                               ? SimpleView.columnWidth - 2 * trayInset
                               : .infinity)
                        .padding(.horizontal, trayInset)
                        .padding(.top, timeline
                                 ? TimelineCard.height(rows: model.timelineRows)
                                     - tuck
                                 : rowHeight + promptHeight - tuck)
                        .transition(.opacity)
                }
                // The prompt slides out to the left as the timeline comes
                // in from the right, and back.
                if timeline {
                    TimelineCard(model: model)
                        .overlay(alignment: .leading) {
                            TimelineSwitch(toTimeline: false) {
                                model.showTimeline(false)
                            }
                            .offset(x: -36)
                        }
                        .transition(.move(edge: .trailing)
                            .combined(with: .opacity))
                } else {
                    promptCard
                        .overlay(alignment: .trailing) {
                            if !immersive && model.timelineAvailable {
                                TimelineSwitch(toTimeline: true) {
                                    model.showTimeline(true)
                                }
                                .offset(x: 36)
                                .transition(.opacity)
                            }
                        }
                        .transition(.move(edge: .leading)
                            .combined(with: .opacity))
                }
            }
        }
        .onChange(of: model.openPanel) { _, p in
            // Custom's panel belongs to the generation card.
            if p != .generate { model.showsTuning = false }
            if let p { lastPanel = p }
            // The Crop card shows the whole canvas.
            if p == .crop { model.stage.fitRequest += 1 }
            model.panelChanged(to: p)
        }
        // The generation card goes with the prompt.
        .onChange(of: model.timelineOpen) { _, open in
            if open && model.openPanel == .generate {
                withAnimation(AppModel.motion) { model.openPanel = nil }
            }
            if !open { model.timelineCutting = false }
        }
        // The Adjust and Crop tabs go when the stage has nothing for them;
        // their cards go with them.
        .onChange(of: model.showsAdjust) { _, shows in
            if !shows && model.openPanel == .adjust {
                withAnimation(AppModel.motion) { model.openPanel = nil }
            }
            if !shows && lastPanel == .adjust { lastPanel = .generate }
        }
        .onChange(of: model.showsCrop) { _, shows in
            if !shows && model.openPanel == .crop {
                withAnimation(AppModel.motion) { model.openPanel = nil }
            }
            if !shows && lastPanel == .crop { lastPanel = .generate }
        }
        .onChange(of: model.showsTrim) { _, shows in
            if !shows && model.openPanel == .trim {
                withAnimation(AppModel.motion) { model.openPanel = nil }
            }
            if !shows && lastPanel == .trim { lastPanel = .generate }
        }
        // A new base brings its size and takes the stage; none gives the
        // stage back. Run from here, not from a didSet on the attachments:
        // a change made inside another property's mutation may never
        // reach the views.
        .onChange(of: model.baseAttachment?.url) { _, url in
            withAnimation(AppModel.motion) { model.baseChanged(to: url) }
        }
    }

    // MARK: Prompt card

    /// The ghost on its way, once both ends are known -- the card it
    /// was (shown first, where it stood) and the thumbnail it became.
    private func flyGhost() {
        guard !ghostFlying, captureGhost != nil,
              let to = model.capturedThumbFrame else { return }
        ghostFlying = true
        DispatchQueue.main.async {
            withAnimation(AppModel.motion) {
                captureGhost = to
            } completion: {
                captureGhost = nil
                ghostFlying = false
                model.capturedItem = nil
                model.capturedThumbFrame = nil
            }
        }
    }

    /// The ghost: the captured thumbnail's picture, once it is read (a
    /// card's grey until then), its corners a thumbnail's.
    private var captureGhostView: some View {
        let thumb = model.capturedItem.flatMap { model.referenceThumbs[$0] }
        return RoundedRectangle(cornerRadius: 8, style: .continuous)
            .fill(Color.primary.opacity(0.08))
            .overlay {
                if let thumb {
                    Image(decorative: thumb, scale: 1)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
    }

    /// The staged media's row and its hairline, while there are any.
    private var rowHeight: CGFloat {
        model.promptAttachments.isEmpty ? 0 : ReferenceRow.height + 0.5
    }

    /// The row of staged media over the text, which stays still while the
    /// text scrolls, and the text.
    private var promptCard: some View {
        VStack(spacing: 0) {
            if !model.promptAttachments.isEmpty {
                ReferenceRow(model: model)
                    // Clear, at the card's top-right corner: the words
                    // and the row. Not in the editor: a new prompt is
                    // its tabs' +, the old one kept.
                    .overlay(alignment: .trailing) {
                        // A capture card has the (x) while it is there.
                        if !immersive && model.attachCapture == nil {
                            clearButton.padding(.trailing, 12)
                        }
                    }
                    .transition(.opacity.combined(with: .move(edge: .top)))
            }
            // The attach menu's recorder or camera: a row of its own after
            // the references, which it joins when it has captured.
            if model.attachCapture != nil {
                AttachCaptureCard(model: model)
                    .onGeometryChange(for: CGRect.self) {
                        $0.frame(in: .named(Self.cardSpace))
                    } action: { captureFrame = $0 }
                    .padding(.horizontal, 14)
                    .padding(.top, model.promptAttachments.isEmpty ? 14 : 2)
                    .padding(.bottom, 12)
                    // Captured, it goes at once: its ghost flies on.
                    .transition(.asymmetric(
                        insertion: .opacity,
                        removal: model.capturedItem != nil
                            ? .identity : .opacity))
            }
            if !model.promptAttachments.isEmpty
                || model.attachCapture != nil {
                Rectangle()
                    .fill(Color(nsColor: .separatorColor))
                    .frame(height: 0.5)
                    .padding(.horizontal, 14)
            }
            textArea
        }
        .frame(maxHeight: immersive ? .infinity : nil, alignment: .top)
        .coordinateSpace(.named(Self.cardSpace))
        // A capture's card flying into the thumbnail it became.
        .overlay(alignment: .topLeading) {
            if let g = captureGhost {
                captureGhostView
                    .frame(width: g.width, height: g.height)
                    .offset(x: g.minX, y: g.minY)
                    .allowsHitTesting(false)
            }
        }
        .onChange(of: model.capturedItem) { _, item in
            // Captured: the ghost where the card was.
            ghostFlying = false
            if item != nil, captureFrame.width > 0 {
                captureGhost = captureFrame
                flyGhost()
            } else if item == nil {
                captureGhost = nil
            }
        }
        .onChange(of: model.capturedThumbFrame) { _, _ in flyGhost() }
        .clipShape(cardShape)
        .background(cardShape
            .fill(Color(nsColor: .textBackgroundColor)))
        .overlay(
            cardShape
                .strokeBorder(dropTargeted
                              ? Color.accentColor
                              : Color.primary.opacity(0.08),
                              lineWidth: dropTargeted ? 2 : 0.5))
        .shadow(color: .black.opacity(0.13), radius: 18, y: 8)
        // The editor's tabs, behind the card's left edge: a tray stood on
        // its side.
        .background(alignment: .topLeading) {
            if immersive {
                GeometryReader { g in
                    PromptTabRail(model: model, height: g.size.height)
                        .offset(x: -PromptTabRail.outset)
                }
                .transition(.opacity.combined(with: .offset(x: 30)))
            }
        }
        .animation(.smooth(duration: 0.2), value: dropTargeted)
    }

    /// In the editor with the assistant's suggestion: the card split in
    /// two halves (DESIGN §10c).
    private var splitting: Bool { immersive && model.suggesting }

    /// The text -- and, in the editor while there is a suggestion, beside
    /// it the suggestion's half, each with its header. The half opens
    /// and closes by its width, its own laid out whole and cut, so it
    /// slides away rather than squeezes.
    private var textArea: some View {
        HStack(spacing: 0) {
            VStack(spacing: 0) {
                if immersive {
                    PaneHeader(title: "Prompt")
                        .frame(height: splitting ? PaneHeader.height : 0)
                        .opacity(splitting ? 1 : 0)
                        .clipped()
                }
                promptText
            }
            if immersive {
                let half = max(0, cardWidth / 2)
                Rectangle()
                    .fill(Color(nsColor: .separatorColor))
                    .frame(width: splitting ? 0.5 : 0)
                    .padding(.vertical, 14)
                SuggestionPane(model: model)
                    .frame(width: half)
                    .frame(width: splitting ? half : 0, alignment: .leading)
                    .clipped()
                    .allowsHitTesting(splitting)
                    .accessibilityHidden(!splitting)
            }
        }
    }

    private var promptText: some View {
        PromptEditor(model: model, height: $promptHeight,
                     dropTargeted: $dropTargeted, lines: $promptLines,
                     compact: model.promptCompact,
                     maxHeight: maxPromptHeight,
                     highlight: model.searchText)
            .frame(height: immersive ? nil : promptHeight)
            .frame(maxHeight: immersive ? .infinity : nil)
            // Text scrolled into the padding fades out at the card's edge.
            .mask(scrollFade)
            // The text wraps before the corner buttons: the editor's,
            // attach and the assistant on one row while the box is short
            // -- four with Clear. (Fixed while it is, so a wrap that moves
            // a button up or down cannot change the wrap again.) In the
            // editor, the editor's alone in the top corner.
            .padding(.trailing, immersive ? 44 : clearInText ? 130 : 100)
            .overlay(alignment: .topLeading) {
                if model.prompt.isEmpty && model.promptAttachments.isEmpty {
                    // How the model Start runs reads a prompt.
                    // Clear of the corner buttons, as the text is.
                    Text(verbatim: model.promptHint)
                        .font(Font(PromptEditor.font))
                        .foregroundStyle(.tertiary)
                        .lineLimit(2)
                        .padding(.leading, PromptEditor.inset.width + 5)
                        .padding(.trailing, (immersive ? 44
                                             : clearInText ? 130 : 100)
                                 + PromptEditor.inset.width)
                        .padding(.top, PromptEditor.inset.height)
                        .allowsHitTesting(false)
                }
            }
            .overlay(alignment: .bottomTrailing) {
                if !immersive {
                    cornerButtons
                        .padding(.trailing, 12)
                        .padding(.bottom, 13)
                        .transition(.opacity)
                }
            }
            // Past two lines, its size and the editor's -- and, with no
            // row of media above, Clear beside them: the top-right corner.
            // In the editor, the editor's (back) alone.
            .overlay(alignment: .topTrailing) {
                if immersive || promptLines > 2 {
                    HStack(spacing: 2) {
                        if !immersive { sizeToggle }
                        // A locked prompt: a copy of it to change.
                        if immersive && model.activeTabReadOnly {
                            duplicateButton
                        }
                        // A tab only looked at never goes to the box.
                        if !model.activeTabDoNotApply { editorButton }
                        if clearInText { clearButton }
                    }
                    .padding(.trailing, 12)
                    .padding(.top, 13)
                    .transition(.opacity)
                }
            }
    }

    /// Something to clear: words, or media in the row.
    private var hasPromptContent: Bool {
        !model.prompt.isEmpty || !model.promptAttachments.isEmpty
    }

    /// Clear is in the text's corner (no row of media to hold it): at the
    /// top beside the size button past two lines; at the end of the
    /// corner buttons before -- a box that short has one right-hand row.
    /// Never in the Prompt Editor: there a new prompt is a new tab (its
    /// +), and the one written stays in its own.
    private var clearInText: Bool {
        !immersive && hasPromptContent && model.promptAttachments.isEmpty
            && model.attachCapture == nil
    }

    private var clearButton: some View {
        Button {
            withAnimation(AppModel.motion) { model.clearPrompt() }
        } label: {
            Image(systemName: "xmark.circle.fill")
                .font(.system(size: 14))
                .frame(width: 26, height: 26)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.tertiary)
        .help("Clear the prompt and its pictures")
        .accessibilityLabel(Text("Clear Prompt"))
        .transition(.opacity)
    }

    /// The card: its top-left corner square, where the modality tab
    /// stands on it and its left side runs on into the tab's. In the
    /// editor both top corners are square, against the prompt toolbar.
    private var cardShape: UnevenRoundedRectangle {
        UnevenRoundedRectangle(topLeadingRadius: 0,
                               bottomLeadingRadius: radius,
                               bottomTrailingRadius: radius,
                               topTrailingRadius: immersive ? 0 : radius,
                               style: .continuous)
    }

    /// A locked prompt -- something was made from it -- copied into a new
    /// tab, to edit.
    private var duplicateButton: some View {
        Button {
            withAnimation(AppModel.motion) {
                model.duplicatePromptTab(model.activePromptTab)
            }
        } label: {
            Image(systemName: "plus.square.on.square")
                .frame(width: 26, height: 26)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .help("Duplicate and Edit: something was made from this prompt, so it stays as it is -- its words in a new tab, to change")
        .accessibilityLabel(Text("Duplicate and Edit"))
    }

    /// Into the Prompt Editor -- the card grown to the window's height --
    /// or, in it, back (⇧⌘E).
    private var editorButton: some View {
        Button {
            withAnimation(AppModel.motion) { model.toggleImmersivePrompt() }
        } label: {
            Image(systemName: immersive ? "rectangle.compress.vertical"
                                        : "rectangle.expand.vertical")
                .frame(width: 26, height: 26)
                .contentShape(Rectangle())
                .contentTransition(.symbolEffect(.replace))
        }
        .buttonStyle(.borderless)
        .foregroundStyle(immersive ? Color.accentColor : .secondary)
        .help(immersive ? "Back to the stage, this prompt in the box (⇧⌘E)"
                        : "Edit the prompt the window's height, several at once (⇧⌘E)")
        .accessibilityLabel(Text("Prompt Editor"))
    }

    private var scrollFade: some View {
        let f: CGFloat = 6
        return VStack(spacing: 0) {
            LinearGradient(colors: [.clear, .black], startPoint: .top,
                           endPoint: .bottom)
                .frame(height: f)
            Rectangle()
            LinearGradient(colors: [.black, .clear], startPoint: .top,
                           endPoint: .bottom)
                .frame(height: f)
        }
        .padding(.vertical, PromptEditor.inset.height - f)
    }

    /// Past two lines: the whole prompt (the box grows with it, to half
    /// the window), or three rows with a scroll bar.
    private var sizeToggle: some View {
        Button {
            model.togglePromptCompact()
        } label: {
            Image(systemName: model.promptCompact
                  ? "arrow.up.left.and.arrow.down.right"
                  : "arrow.down.right.and.arrow.up.left")
                .frame(width: 26, height: 26)
                .contentShape(Rectangle())
                .contentTransition(.symbolEffect(.replace))
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .help(model.promptCompact ? "Show the whole prompt"
                                  : "Show three lines, with a scroll bar")
    }

    /// Attach and the assistant, in the card's bottom-right corner -- the
    /// editor's ahead of them, and Clear after, while the box is short
    /// (one right-hand row).
    private var cornerButtons: some View {
        HStack(spacing: 4) {
            if promptLines <= 2 { editorButton }
            AttachMenu(model: model, size: 26)

            Button(action: model.assistantPressed) {
                Group {
                    if model.enhanceJob != nil {
                        ProgressView().controlSize(.small)
                    } else {
                        Image(systemName: "sparkles")
                    }
                }
                .frame(width: 26, height: 26)
                .contentShape(Rectangle())
            }
            .buttonStyle(.borderless)
            .disabled(!model.assistantEnabled)
            .help(model.assistantHelp)
            if clearInText && promptLines <= 2 { clearButton }
        }
        .foregroundStyle(.secondary)
    }

    // MARK: Tray: two cards, headers at the bottom

    /// The content only -- the open card's body, then the header row;
    /// the cards themselves are drawn behind it (`cards`).
    private var tray: some View {
        VStack(spacing: 0) {
            drawer
            HStack(spacing: tabGap) {
                // The row keeps room on its left; the brief gives way
                // first when the window is narrow.
                Spacer(minLength: 72)
                // Only once the stage has a picture of its own to adjust.
                if model.showsAdjust {
                    adjustTab
                        .fixedSize()
                        .transition(.opacity)
                }
                if model.showsCrop {
                    cropTab
                        .fixedSize()
                        .transition(.opacity)
                }
                if model.showsTrim {
                    trimTab
                        .fixedSize()
                        .transition(.opacity)
                }
                if !timeline {
                    generateTab
                        .transition(.opacity)
                }
            }
        }
        // Added to the headers' anchors, not in place of them.
        .transformAnchorPreference(key: TabFrameKey.self,
                                   value: .bounds) { $0[.tray] = $1 }
        .backgroundPreferenceValue(TabFrameKey.self) { frames in
            GeometryReader { proxy in cards(frames, proxy) }
        }
    }

    /// The open card's body, its top behind the prompt card. Each panel
    /// sits at the bottom of its own box, clipped; opening one grows its
    /// box from nothing, so the panel rides down on the header row from
    /// under the prompt card (and back up as it closes) -- one layout
    /// animation, nothing to keep in step. Switching cards shrinks one
    /// box as the other grows.
    private var drawer: some View {
        VStack(spacing: 0) {
            drawerPanel(.adjust)
            drawerPanel(.crop)
            drawerPanel(.trim)
            drawerPanel(.generate)
        }
        .padding(.top, tuck)
    }

    private func drawerPanel(_ p: ComposerPanel) -> some View {
        let open = model.openPanel == p
        return panel(p)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.horizontal, 20)
            .padding(.vertical, 18)
            .fixedSize(horizontal: false, vertical: true)
            .frame(height: open ? nil : 0, alignment: .bottom)
            .clipped()
            .allowsHitTesting(open)
            .accessibilityHidden(!open)
            // Out of the Tab order too: a shut drawer's fields are still
            // laid out (at no height), and Tab would visit them unseen.
            .disabled(!open)
    }

    /// The cards: the open (or last opened) one in front of the others,
    /// each with its own shadow -- so the open card shades the headers
    /// behind it, and reads as pulled out in front of them. Each card
    /// keeps one identity and is raised, not moved to another list: a
    /// card re-inserted as it went behind faded in from the layer's final
    /// place -- a slab over the picture while a drawer opened.
    @ViewBuilder
    private func cards(_ frames: [TrayPart: Anchor<CGRect>],
                       _ proxy: GeometryProxy) -> some View {
        let front = model.openPanel ?? lastPanel
        // A header where it shows on the tray: against the tray's anchor,
        // not the layer's origin (see the type's comment).
        let origin = frames[.tray].map { proxy[$0].origin } ?? .zero
        let tab = { (p: ComposerPanel) -> CGRect? in
            frames[.tab(p)].map {
                proxy[$0].offsetBy(dx: -origin.x, dy: -origin.y)
            }
        }
        ZStack(alignment: .topLeading) {
            ForEach(ComposerPanel.allCases, id: \.self) { p in
                if let r = tab(p) {
                    card(r, drawer: p == front)
                        .zIndex(p == front ? 1 : 0)
                }
            }
        }
    }

    private func card(_ tab: CGRect, drawer: Bool) -> some View {
        TrayCard(rowTop: tab.minY, tabMinX: tab.minX, tabMaxX: tab.maxX,
                 tabHeight: tab.height, shutRowTop: tuck, drawer: drawer,
                 radius: tabRadius, fillet: fillet)
            .fill(Self.trayFill)
            .shadow(color: .black.opacity(0.09), radius: 7, y: 3)
    }

    private func chevron(open: Bool) -> some View {
        Image(systemName: "chevron.down")
            .font(.caption.weight(.semibold))
            .rotationEffect(.degrees(open ? 180 : 0))
            .foregroundStyle(.tertiary)
    }

    private var adjustTab: some View {
        let open = model.openPanel == .adjust
        return Button {
            toggle(.adjust)
        } label: {
            HStack(spacing: 8) {
                chevron(open: open)
                    .padding(.trailing, 2)
                Image(systemName: "slider.horizontal.3")
                    .accessibilityLabel(Text("Adjust"))
                if model.keyedStage ? !model.clipAdjustKeys.isIdentity
                                     : !model.adjustments.isIdentity {
                    Circle().fill(Color.accentColor).frame(width: 6, height: 6)
                        .help("Adjustments are applied")
                }
            }
            .font(.callout)
            .foregroundStyle(.secondary)
            .padding(.leading, 16)
            .padding(.trailing, 18)
            .frame(height: tabHeight)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(open ? "Hide adjustments"
              : model.adjustsBase ? "Adjust the picture to edit"
              : model.clipOnStage ? "Adjust the clip"
              : "Adjust the result")
        .anchorPreference(key: TabFrameKey.self, value: .bounds) {
            [.tab(.adjust): $0]
        }
    }

    /// Crop and rotate (a picture, or a clip -- keyed). A dot while it
    /// changes what the picture or clip gives.
    private var cropTab: some View {
        let changed = model.keyedStage ? !model.clipCropKeys.isIdentity
                                        : !model.crop.isIdentity
        return iconTab(.crop, symbol: "crop", name: "Crop", changed: changed,
                       dot: "The picture is cropped or turned",
                       show: model.clipOnStage ? "Crop and rotate the clip"
                                               : "Crop and rotate the picture",
                       hide: "Hide the crop")
    }

    /// A clip's (or a sound's) mark-in and mark-out.
    private var trimTab: some View {
        let sound = model.stageIsAudio
        return iconTab(.trim, symbol: "timeline.selection", name: "Trim",
                       changed: !model.trim.isIdentity,
                       dot: sound ? "The sound is trimmed"
                                  : "The clip is trimmed",
                       show: sound ? "Trim the sound" : "Trim the clip",
                       hide: "Hide the trim")
    }

    /// A card's header that is its icon alone (its name is its help and
    /// what VoiceOver says), and a dot while it changes something.
    private func iconTab(_ p: ComposerPanel, symbol: String,
                         name: LocalizedStringKey, changed: Bool,
                         dot: LocalizedStringKey, show: LocalizedStringKey,
                         hide: LocalizedStringKey) -> some View {
        let open = model.openPanel == p
        return Button {
            toggle(p)
        } label: {
            HStack(spacing: 8) {
                chevron(open: open)
                    .padding(.trailing, 2)
                Image(systemName: symbol)
                    .accessibilityLabel(Text(name))
                if changed {
                    Circle().fill(Color.accentColor).frame(width: 6, height: 6)
                        .help(dot)
                }
            }
            .font(.callout)
            .foregroundStyle(.secondary)
            .padding(.leading, 16)
            .padding(.trailing, 18)
            .frame(height: tabHeight)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(open ? hide : show)
        .anchorPreference(key: TabFrameKey.self, value: .bounds) {
            [.tab(p): $0]
        }
    }

    private var generateTab: some View {
        let open = model.openPanel == .generate
        return HStack(spacing: 12) {
            Button {
                toggle(.generate)
            } label: {
                HStack(spacing: 10) {
                    chevron(open: open)
                    // Short of room, the model's name gives way; the size
                    // and the preference stay whole.
                    HStack(spacing: 0) {
                        Text(summaryLead)
                            .lineLimit(1)
                            .truncationMode(.tail)
                        Text(verbatim: summaryTail)
                            .lineLimit(1)
                            .fixedSize()
                    }
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .contentTransition(.opacity)
                }
                .frame(height: tabHeight)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help(open ? "Hide options" : "Show options")

            StartButton(model: model)
        }
        .padding(.leading, 16)
        .padding(.trailing, 12)
        .frame(height: tabHeight)
        .anchorPreference(key: TabFrameKey.self, value: .bounds) {
            [.tab(.generate): $0]
        }
    }

    @ViewBuilder
    private func panel(_ p: ComposerPanel) -> some View {
        switch p {
        case .generate: DrawerSettings(model: model)
        case .adjust: AdjustPanel(model: model)
        case .crop: CropPanel(model: model)
        case .trim: TrimPanel(model: model)
        }
    }

    private func toggle(_ p: ComposerPanel) {
        withAnimation(AppModel.motion) {
            model.openPanel = model.openPanel == p ? nil : p
        }
    }

    /// The tray's fill: opaque, so a card hides what is behind it; a
    /// shade off the window in either appearance.
    static let trayFill = Color.valtzPanel

    /// What runs: "Edit · Qwen-Image 2.1", "Compose · …", "From picture ·
    /// MiniMax H3 FL2VA" or the model.
    private var summaryLead: String {
        guard model.modalityRuns else {
            return model.activeModality == .audio
                ? String(localized: "Audio · not available yet")
                : String(localized: "Video · not available yet")
        }
        // A clip from references (Ref2VA): continuing one, or drawing on
        // pictures, clips and sounds.
        if model.usesReferences {
            let name = model.modelName(model.videoReferenceModel)
            return model.promptAttachments.contains(where: \.continues)
                ? String(localized: "Continue · \(name)")
                : String(localized: "References · \(name)")
        }
        // Sound: a song, or speech -- in the row's voice.
        if model.activeModality == .audio {
            let name = model.modelName(model.audioModel)
            return model.speaks && model.speechVoice != nil
                ? String(localized: "Voice · \(name)") : name
        }
        if model.opensOnPicture, model.baseAttachment?.kind == "image" {
            return String(localized: "From picture · \(model.imageModelName)")
        }
        guard model.willEdit else { return model.imageModelName }
        // The base fills the size set here (its own, unless changed);
        // pictures without one are composed into a picture of it.
        return model.baseAttachment != nil
            ? String(localized: "Edit · \(model.editModelName)")
            : String(localized: "Compose · \(model.editModelName)")
    }

    /// " · 1.95 mp · Balanced": the size as megapixels, to keep the tab
    /// short (the card shows it whole). Decimal megapixels (1e6, as camera
    /// makers count), two places, written as text: no digit grouping or
    /// locale separator gets into it. "mp" is a unit symbol, the same in
    /// every language, so it is not in the String Catalog.
    private var summaryTail: String {
        // Speech: " · about 10 s · Med".
        if model.speaks {
            return " · " + model.speechLengthText + " · "
                + model.preference.label
        }
        // A song has no size: " · up to 2:00 · Med".
        if model.activeModality == .audio {
            return " · " + model.songLengthText + " · "
                + model.preference.label
        }
        let d = model.dimensions
        let mp = String(format: "%.2f", Double(d.width) * Double(d.height) / 1e6)
        // A clip's length too: " · 0.40 mp · 5.2 s · Balanced".
        let length = model.activeModality == .video
            ? " · " + model.clipDurationText : ""
        return " · \(mp) mp" + length + " · " + model.preference.label
    }

    /// Attach: media into the row -- or, for a song, lyrics from a text
    /// file. The prompt card's paperclip, and immersive editing's.
    static func attach(_ model: AppModel) {
        let panel = NSOpenPanel()
        panel.directoryURL = model.panelFolder(.attach)
        // A song's: lyrics, a text file a song (vpipe's songs-from-lyrics),
        // into the prompt after its words.
        if model.activeModality == .audio {
            panel.allowedContentTypes = [.plainText, .text]
            panel.message = String(localized: "Choose a lyrics file")
            if panel.runModal() == .OK, let url = panel.url {
                model.rememberPanel(.attach, chose: url)
                withAnimation(AppModel.motion) { model.addLyrics(from: url) }
            }
            return
        }
        panel.allowsMultipleSelection = true
        // A clip draws on sounds too (Ref2VA: a voice, a song).
        panel.allowedContentTypes = model.activeModality == .video
            ? [.image, .movie, .audio] : [.image, .movie]
        if panel.runModal() == .OK {
            model.rememberPanel(.attach, chose: panel.urls.first)
            model.addReferences(panel.urls)
        }
    }
}

/// The paperclip: a menu -- a file browsed for (a song's lyrics in the
/// Audio tab), or a sound recorded from the microphone or the system's
/// audio, or a still or a clip from the camera (AttachCapture), each
/// into the prompt's row. The prompt card's, and the Prompt Editor's.
struct AttachMenu: View {
    @Bindable var model: AppModel
    var size: CGFloat = 22

    var body: some View {
        Menu {
            Button(model.activeModality == .audio ? "Add Lyrics…"
                                                  : "Browse…") {
                ComposerStack.attach(model)
            }
            Divider()
            Section("Record Sound") {
                Button("From the Microphone") {
                    model.attachSound(system: false)
                }
                Button("From the System’s Audio") {
                    model.attachSound(system: true)
                }
            }
            .disabled(model.activeModality == .image)
            Section("Camera") {
                Button("Take a Photo or Video…") { model.attachCamera() }
            }
        } label: {
            Image(systemName: "paperclip")
                .frame(width: size, height: size)
                .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .disabled(model.attachCapture != nil)
        .help("Attach: a file, a sound recorded, a photo or a video from the camera")
    }
}

extension View {
    /// A composer row's label found by the title bar's search: marked as
    /// Find marks text, without moving the layout.
    /// A setting to change for a refused generation to fit (the memory
    /// card's suggestion): ringed in orange while it is as it failed.
    func suggestMark(_ on: Bool) -> some View {
        self
            .background {
                if on {
                    RoundedRectangle(cornerRadius: 4, style: .continuous)
                        .strokeBorder(Color.orange, lineWidth: 1.5)
                        .background(Color.orange.opacity(0.12),
                                    in: RoundedRectangle(cornerRadius: 4,
                                                         style: .continuous))
                        .padding(.horizontal, -4)
                        .padding(.vertical, -1)
                }
            }
            .help(on ? String(localized: "Lower this for the clip to fit in memory") : "")
    }

    func searchMark(_ on: Bool) -> some View {
        self
            .foregroundStyle(on ? AnyShapeStyle(Color.black)
                                : AnyShapeStyle(.secondary))
            .background {
                if on {
                    RoundedRectangle(cornerRadius: 4, style: .continuous)
                        .fill(Color(nsColor: .findHighlightColor))
                        .padding(.horizontal, -4)
                        .padding(.vertical, -1)
                }
            }
    }
}

/// What Start makes, Video, Image or Audio, on a tab above the prompt
/// card's top-left corner -- out of the generation card, so a Start needs no
/// card opened for it. Grey, as the tray's headers, and with no drawer:
/// rounded on top, its left side running straight down into the card's
/// (whose corner there is square), its right side joined to the card's
/// top edge by a concave corner. Its foot reaches a little under the
/// card, so no seam of the window shows between them.
private struct ModalityTab: View {
    @Bindable var model: AppModel
    @Namespace private var pill

    private let radius: CGFloat = 14
    private let fillet: CGFloat = 10
    /// How far it reaches under the card.
    private let tuck: CGFloat = 4

    var body: some View {
        HStack(spacing: 2) {
            ForEach(Modality.allCases) { segment($0) }
        }
        .padding(4)
        // Found by search: marked as Find marks text, inside the tab.
        .background {
            if model.searchHits.contains(SettingsRow.modality) {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(Color(nsColor: .findHighlightColor))
            }
        }
        .padding(.horizontal, 2)
        .padding(.top, 2)
        .padding(.bottom, 4)
        .background(alignment: .topLeading) {
            GeometryReader { g in
                ModalityTabShape(tabWidth: g.size.width,
                                 tabHeight: g.size.height,
                                 radius: radius, fillet: fillet)
                    .fill(ComposerStack.trayFill)
                    .frame(width: g.size.width + fillet,
                           height: g.size.height + tuck)
                    .shadow(color: .black.opacity(0.09), radius: 7, y: 3)
            }
        }
        .fixedSize()
        .accessibilityElement(children: .contain)
        .accessibilityLabel(Text("Create"))
    }

    /// One choice: its glyph and name, the chosen one on a white pill
    /// that slides to the other.
    private func segment(_ m: Modality) -> some View {
        let on = model.modality == m
        return Button {
            withAnimation(AppModel.motion) { model.setModality(m) }
        } label: {
            HStack(spacing: 6) {
                Image(systemName: m.symbol)
                Text(verbatim: m.label)
            }
            .font(.callout)
            .foregroundStyle(on ? AnyShapeStyle(.primary)
                                : AnyShapeStyle(.secondary))
            .padding(.horizontal, 12)
            .frame(height: 26)
            .background {
                if on {
                    RoundedRectangle(cornerRadius: 8, style: .continuous)
                        .fill(Color(nsColor: .textBackgroundColor))
                        .shadow(color: .black.opacity(0.1), radius: 1,
                                y: 0.5)
                        .matchedGeometryEffect(id: "pill", in: pill)
                }
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(on ? .isSelected : [])
        .help(m == .video ? "Create a video"
              : m == .audio ? "Create a song" : "Create an image")
    }
}

/// The modality tab's silhouette, in a box `fillet` wider than the tab
/// and reaching under the card: the tab (`tabWidth` x `tabHeight`, its
/// top corners rounded), the concave corner on its right where it meets
/// the card's top, and its foot behind the card.
private struct ModalityTabShape: Shape {
    var tabWidth: CGFloat
    var tabHeight: CGFloat
    var radius: CGFloat
    var fillet: CGFloat

    func path(in rect: CGRect) -> Path {
        roundedPolygon([
            (CGPoint(x: rect.maxX, y: rect.maxY), 0),
            (CGPoint(x: 0, y: rect.maxY), 0),
            (CGPoint(x: 0, y: 0), radius),
            (CGPoint(x: tabWidth, y: 0), radius),
            (CGPoint(x: tabWidth, y: tabHeight), fillet),
            (CGPoint(x: rect.maxX, y: tabHeight), 0),
        ])
    }
}

/// What the cards behind the content are drawn from: each header, and
/// the tray they are measured against.
private enum TrayPart: Hashable {
    case tray
    case tab(ComposerPanel)
}

private struct TabFrameKey: PreferenceKey {
    static var defaultValue: [TrayPart: Anchor<CGRect>] { [:] }
    static func reduce(value: inout [TrayPart: Anchor<CGRect>],
                       nextValue: () -> [TrayPart: Anchor<CGRect>]) {
        value.merge(nextValue()) { $1 }
    }
}

/// One card of the tray, in the tray's coordinates: a header from the
/// top of the tray down to the bottom of the header row, and -- for the
/// card in front, while its drawer is out -- the body above the row, the
/// tray's full width, joined to the header by concave corners.
///
/// Nothing in it animates: the frames it is given are where the headers
/// show, frame by frame, while the drawer slides or the composer moves
/// (`cards`) -- animating them again would trail the headers. The body's
/// bottom edge and the header stay joined, and switching cards moves the
/// join at once rather than sliding it across.
private struct TrayCard: Shape {
    var rowTop: CGFloat
    var tabMinX: CGFloat
    var tabMaxX: CGFloat
    var tabHeight: CGFloat
    /// `rowTop` with every drawer shut: the body has no height left.
    var shutRowTop: CGFloat
    var drawer: Bool
    var radius: CGFloat
    var fillet: CGFloat

    var animatableData: EmptyAnimatableData {
        get { EmptyAnimatableData() }
        set {}
    }

    func path(in rect: CGRect) -> Path {
        let bottom = rowTop + tabHeight
        let out = drawer ? rowTop - shutRowTop : 0
        // Clockwise from the top-right; the top is hidden behind the
        // prompt card, so its corners stay square.
        var c: [(CGPoint, CGFloat)]
        if out < 0.5 {
            c = [(CGPoint(x: tabMaxX, y: 0), 0),
                 (CGPoint(x: tabMaxX, y: bottom), radius),
                 (CGPoint(x: tabMinX, y: bottom), radius),
                 (CGPoint(x: tabMinX, y: 0), 0)]
        } else {
            // The joins grow in with the body, from nothing.
            let f = min(fillet, out)
            c = [(CGPoint(x: rect.maxX, y: 0), 0)]
            // A header flush with the tray's right edge continues it
            // straight down; otherwise the body rounds off first.
            if tabMaxX < rect.maxX - 0.5 {
                c += [(CGPoint(x: rect.maxX, y: rowTop), radius),
                      (CGPoint(x: tabMaxX, y: rowTop), f)]
            }
            c += [(CGPoint(x: tabMaxX, y: bottom), radius),
                  (CGPoint(x: tabMinX, y: bottom), radius),
                  (CGPoint(x: tabMinX, y: rowTop), f),
                  (CGPoint(x: rect.minX, y: rowTop), radius),
                  (CGPoint(x: rect.minX, y: 0), 0)]
        }
        return roundedPolygon(c)
    }
}

/// A closed polygon with each corner rounded to its own radius, convex
/// or concave alike (a tangent arc sits on the inside of the turn). It
/// starts halfway along the last edge, which must have square ends.
private func roundedPolygon(_ c: [(CGPoint, CGFloat)]) -> Path {
    var p = Path()
    guard let first = c.first?.0, let last = c.last?.0 else { return p }
    p.move(to: CGPoint(x: (first.x + last.x) / 2, y: (first.y + last.y) / 2))
    for (i, (pt, r)) in c.enumerated() {
        if r > 0 {
            p.addArc(tangent1End: pt, tangent2End: c[(i + 1) % c.count].0,
                     radius: r)
        } else {
            p.addLine(to: pt)
        }
    }
    p.closeSubpath()
    return p
}

/// The adjustments panel: eight sliders over the picture on the stage,
/// applied live -- a result, or the picture to edit, which the model then
/// receives adjusted (on top of a RAW's development). The values are
/// recorded on the image, applied when a file is made from it; the
/// original is kept. Each value is a field too: type one, or clear it
/// with its x; Reset clears them all.
private struct AdjustPanel: View {
    @Bindable var model: AppModel

    private let columns: [[ImageAdjustments.Key]] = [
        [.exposure, .contrast, .highlights, .shadows],
        [.temperature, .tint, .vibrance, .saturation],
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                if model.keyedStage {
                    // A clip (a still with pages): keyframes, the values
                    // between interpolated.
                    KeyframeBar(model: model, track: .adjust)
                } else {
                    Text(!model.canAdjust
                         ? "Adjustments wait for the image being made."
                         : model.adjustsBase
                         ? "Applied as the picture is edited and saved; the original is kept."
                         : "Applied when the image is saved; the original is kept.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                BypassButton(model: model, part: .adjust,
                             help: "Hold to see the picture without its adjustments")
                Button("Reset", action: model.resetAdjustments)
                    .disabled(model.keyedStage
                              ? model.clipAdjustKeys
                                  == Keyframes(start: ImageAdjustments())
                                  || model.layerOffStage != nil
                              : model.adjustments.isIdentity)
            }
            HStack(alignment: .top, spacing: 28) {
                ForEach(columns.indices, id: \.self) { c in
                    Grid(alignment: .leading, horizontalSpacing: 10,
                         verticalSpacing: 8) {
                        ForEach(columns[c]) { key in row(key) }
                    }
                    // Tab goes down a column, then on to the next.
                    .focusSection()
                }
            }
            // Greyed while the playhead is off the selected layer: its
            // values would change unseen.
            .disabled(!model.canEditLayerHere)
        }
        .controlSize(.small)
    }

    private func row(_ key: ImageAdjustments.Key) -> some View {
        GridRow {
            Text(key.label)
                .foregroundStyle(.secondary)
                .searchMark(model.searchHits.contains(key.rawValue))
                .gridColumnAlignment(.trailing)
                .onTapGesture(count: 2) { model.setAdjustment(key, 0) }
                .help("Double-click to reset")
            CenteredSlider(value: Binding(get: { model.adjustments[key] },
                                          set: { model.setAdjustment(key, $0) }),
                           range: key.range)
                .frame(minWidth: 110)
                .accessibilityLabel(Text(key.label))
            AdjustValueField(model: model, key: key)
        }
    }
}

/// One adjustment's value as a text box: typed values are taken on
/// Return or when the field is left (exposure in stops, the others
/// -100...+100), clamped to the slider's range; the x on its right clears
/// that value alone.
private struct AdjustValueField: View {
    @Bindable var model: AppModel
    let key: ImageAdjustments.Key
    @State private var text = ""
    /// What it last showed: text unlike it is being typed.
    @State private var shown = ""
    @FocusState private var focused: Bool

    var body: some View {
        let value = model.adjustments[key]
        HStack(spacing: 2) {
            // Named for VoiceOver by its row; the row shows the name.
            TextField(key.label, text: $text)
                .labelsHidden()
                .textFieldStyle(.plain)
                .multilineTextAlignment(.trailing)
                .font(.callout.monospacedDigit())
                .focused($focused)
                .onSubmit(commit)
                .frame(width: 42)
            Button {
                model.setAdjustment(key, 0)
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.caption)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.tertiary)
            .help("Reset \(key.label)")
            .opacity(value == 0 ? 0 : 1)
            .disabled(value == 0)
        }
        .padding(.leading, 6)
        .padding(.trailing, 3)
        .padding(.vertical, 2)
        .background(RoundedRectangle(cornerRadius: 5)
            .fill(Color(nsColor: .textBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 5)
            .strokeBorder(Color(nsColor: .separatorColor)))
        .onAppear { show(key.display(value)) }
        // It follows the slider and the frame a clip shows, focused or
        // not, unless a value is being typed in it.
        .onChange(of: value) { _, v in
            if text == shown { show(key.display(v)) }
        }
        .onChange(of: focused) { _, f in
            if !f { commit() }
        }
    }

    private func show(_ s: String) {
        text = s
        shown = s
    }

    private func commit() {
        if let v = key.parse(text) { model.setAdjustment(key, v) }
        show(key.display(model.adjustments[key]))
    }
}

extension View {
    /// Start's look -- prominent glass, a capsule, large: Start itself,
    /// and the capture card's Record, Take Photo, Stop and Attach.
    func startButtonStyle() -> some View {
        buttonStyle(.glassProminent)
            .buttonBorderShape(.capsule)
            .controlSize(.large)
    }
}

/// Start -- always: a task runs now, or after the tasks before it (the
/// TASK QUEUE, DESIGN §3a) -- and, while the stage watches a task, its
/// progress and Stop beside it.
private struct StartButton: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 6) {
            if model.isGenerating {
                Button(action: model.stop) {
                    ZStack {
                        ProgressView(value: progress)
                            .progressViewStyle(.circular)
                            .controlSize(.small)
                        Image(systemName: "stop.fill")
                            .font(.system(size: 7))
                    }
                }
                .buttonStyle(.glass)
                .buttonBorderShape(.circle)
                .keyboardShortcut(".", modifiers: .command)
                .help("Stop the task on the stage (⌘.)")
                .transition(.scale.combined(with: .opacity))
            }
            Button(action: model.start) {
                Text("Start")
                    .frame(minWidth: 70)
            }
            .buttonStyle(.glassProminent)
            .keyboardShortcut(.return, modifiers: .command)
            .disabled(!model.canStart)
            // A capsule: the rounder shape sits better inside the tab's
            // own rounded corner.
            .buttonBorderShape(.capsule)
            .help(model.tasks.isEmpty
                  ? "Start (⌘↩)"
                  : "Start (⌘↩): queued after the tasks before it")
        }
        .controlSize(.large)
    }

    /// The phase's count; a spinner while nothing is counted.
    private var progress: Double? { model.generationPhase?.fraction }
}

/// The model field: AUTO first -- the core's pick for what Start does
/// (generate, or edit when pictures are attached), named beside it --
/// then the installed models for the modality. One field for both: a
/// chosen model generates, and edits if it can.
struct ModelPicker: View {
    @Bindable var model: AppModel
    /// A width to take (the generation card's selectors share one); its
    /// own when nil.
    var width: CGFloat? = nil

    var body: some View {
        if let width {
            // The AppKit pop-up, set to fill the width it is given (as
            // FilledSegments, for the same reason).
            let options = model.modelOptions(for: model.activeModality)
            FilledPopUp(items: [("", autoLabel)]
                            + (options.isEmpty ? [] : [(nil, "")])
                            + options.map { ($0.model, $0.name) },
                        selection: $model.modelChoice, width: width)
                .accessibilityLabel(Text("Model"))
                .fixedSize()
        } else {
            Picker("Model", selection: $model.modelChoice) {
                Text(autoLabel).tag("")
                let options = model.modelOptions(for: model.activeModality)
                if !options.isEmpty { Divider() }
                ForEach(options, id: \.model) { o in
                    Text(o.name).tag(o.model)
                }
            }
            .labelsHidden()
            .fixedSize()
        }
    }

    /// "Auto (Krea 2 Turbo)": what Auto would run for the prompt as it
    /// is -- its edit pick when pictures are attached, else its generate
    /// pick.
    private var autoLabel: String {
        // A clip from references: Auto's video edit pick (Ref2VA); sound
        // in a voice the row holds, its audio edit pick (speech).
        let edits = model.activeModality == .video ? model.usesReferences
            : model.activeModality == .audio ? model.speechVoice != nil
                && !model.autoPick("edit").isEmpty
            : model.hasEditPictures && !model.autoPick("edit").isEmpty
        let id = model.autoPick(edits ? "edit" : "generate")
        // A quantized variant goes by its source's name.
        return id.isEmpty ? String(localized: "Auto")
                          : String(localized: "Auto (\(model.modelName(model.baseModel(id))))")
    }
}

/// The generation card's settings in the Prompt Editor (its toolbar's
/// button): the same rows in one popover. Tune's options open inside it,
/// between separators -- never a second popover -- and it scrolls past
/// the screen's room.
struct GenerationSettingsPopover: View {
    @Bindable var model: AppModel
    @State private var size: CGSize = .zero

    var body: some View {
        ScrollView(.vertical) {
            DrawerSettings(model: model, tuneInline: true)
                .padding(16)
                .fixedSize()
                .onGeometryChange(for: CGSize.self) { $0.size }
                    action: { size = $0 }
        }
        .scrollBounceBehavior(.basedOnSize)
        .frame(width: size.width > 0 ? size.width : nil,
               height: size.height > 0 ? min(size.height, 680) : nil)
        .animation(AppModel.motion, value: model.showsTuning)
    }
}

/// The drawer: the settings that do not need to be seen every time.
private struct DrawerSettings: View {
    @Bindable var model: AppModel
    /// In a popover of its own (the Prompt Editor's): Tune's options open
    /// INSIDE it, under Favor between separators, not as a second one.
    var tuneInline = false
    /// The selectors' one width: the widest one's own.
    @State private var selectorWidth: CGFloat = 0
    /// The labels' one width, across the two grids: the widest one's.
    @State private var labelWidth: CGFloat = 0

    var body: some View {
        // Every control starts at the column's leading edge, and every
        // selector -- the segmented ones and the model's -- is as wide as
        // the widest of them needs (FilledSegments), so they and the
        // fields beside them line up in any language. (Fixed 300-pt
        // frames centred what they held, and short labels widened their
        // column.)
        // Two grids -- Seed apart, so Tune's options can come between --
        // their labels one width (the widest's), so they line up.
        VStack(alignment: .leading, spacing: 12) {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 12) {
            GridRow {
                label("Model", row: SettingsRow.model)
                ModelPicker(model: model,
                            width: selectorWidth > 0 ? selectorWidth : nil)
                    .gridCellColumns(2)
            }
            if !model.modalityRuns {
                GridRow {
                    Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                    Text(model.activeModality == .audio
                         ? "Audio generation needs YuE2 (songs) or MOSS-TTS (speech): download one in Settings › Capabilities."
                         : "Video generation is not available yet.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                        .gridCellColumns(2)
                }
            }
            if model.speaks {
                speechRows
            } else if model.activeModality == .audio {
                audioRows
            } else {
                pictureRows
            }
            GridRow {
                label("Favor", row: SettingsRow.favor)
                selector("Favor", Preference.allCases.map { ($0, $0.label) },
                         tips: Preference.allCases.map(\.hint),
                         selection: optional(preference))
                // Custom's options, from the Custom segment itself (the
                // segments are equal: the middle of its share) -- not in
                // the Prompt Editor, whose popover shows them inside. AppKit's
                // popover, semi-transient: it stays open while Valtz is
                // left, for weights dragged in from the Finder.
                .appKitPopover(isPresented: Binding(
                                   get: {
                                       !tuneInline && model.showsTuning
                                           && !model.promptImmersive
                                   },
                                   set: {
                                       if !tuneInline { model.showsTuning = $0 }
                                   }),
                               anchor: customAnchor, arrowEdge: .top) {
                    TuningPanel(model: model)
                }
                .onAppear { model.watchTuningPresses() }
                if model.preference == .custom {
                    // What Custom is set to; a click opens it again (or
                    // shuts it, open).
                    Button {
                        model.toggleTuning()
                    } label: {
                        Text(model.preferenceHint)
                            .font(.callout)
                            .lineLimit(1)
                    }
                    .buttonStyle(.link)
                    .help("Change the custom settings")
                } else {
                    Text(model.preferenceHint)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
            }
        }
        .controlSize(.regular)
        // Tune's options, in the Prompt Editor's popover: between the
        // rows, a section of their own (as wide as they need: the rows'
        // columns stay as they are), separators above and below.
        if tuneInline && model.showsTuning {
            Divider()
            TuningPanel(model: model, embedded: true)
            Divider()
        }
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 12) {
            GridRow {
                label("Seed", row: SettingsRow.seed)
                HStack(spacing: 6) {
                    TextField("Random", text: $model.seedText)
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 120)
                    Button {
                        model.seedText = String(Int.random(in: 0..<2_147_483_647))
                    } label: { Image(systemName: "dice") }
                        .buttonStyle(.borderless)
                        .help("Pick a seed")
                }
                .gridCellColumns(2)
            }
        }
        .controlSize(.regular)
        }
        // Custom follows the model Start runs: its family's options.
        .onChange(of: model.runningModel) { _, _ in model.refreshTuning() }
    }

    /// A picture's or a clip's rows: its shape and size, a clip's length.
    @ViewBuilder
    private var pictureRows: some View {
        GridRow {
            label("Orientation", row: SettingsRow.orientation)
            // Glyphs: a square, a tall and a wide rectangle.
            selector("Orientation",
                     Orientation.allCases.map { ($0, $0.label) },
                     symbols: Orientation.allCases.map(\.symbol),
                     selection: optional(orientation))
                .gridCellColumns(2)
        }
        // A preset that is not square has a ratio to pick; a custom
        // size has its own.
        if model.showsAspectRatio {
            GridRow {
                label("Aspect ratio", row: SettingsRow.aspect)
                selector("Aspect ratio",
                         AspectRatio.allCases.map {
                             ($0, $0.label(model.orientation))
                         },
                         selection: optional($model.aspectRatio))
                    .gridCellColumns(2)
            }
            .transition(.opacity)
        }
        GridRow {
            label("Size", row: SettingsRow.size)
            selector("Size", SizeClass.allCases.map { ($0, $0.label) },
                     selection: sizeChoice)
            SizeField(model: model)
        }
        // A clip from references with a song among them: its soundtrack
        // is the song as it is (a music video), or the one generated with
        // the picture.
        if model.hasSongReference {
            GridRow {
                label("Soundtrack", row: SettingsRow.soundtrack)
                selector("Soundtrack",
                         [(true, String(localized: "The song")),
                          (false, String(localized: "Generated"))],
                         tips: [String(localized: "The song in the prompt, as it is, under the clip"),
                                String(localized: "The sound made with the clip")],
                         selection: optional($model.keepSongSound))
                    .gridCellColumns(2)
            }
            .transition(.opacity)
        }
        // A clip's length: a preset in seconds, or one typed in the
        // field beside them (frames or seconds); either is made as the
        // nearest length the model makes, which the field shows.
        if model.activeModality == .video && model.modalityRuns {
            GridRow {
                label("Length", row: SettingsRow.length)
                selector("Length",
                         AppModel.clipLengths.map {
                             ($0, String(localized: "\($0) s"))
                         },
                         selection: clipChoice)
                ClipLengthField(model: model)
            }
            .transition(.opacity)
        }
    }

    /// Speech's rows (MOSS-TTS, DESIGN §4g): whose voice it speaks in --
    /// the prompt's sound -- and about how long it runs.
    @ViewBuilder
    private var speechRows: some View {
        GridRow {
            label("Voice", row: SettingsRow.voice)
            Text(verbatim: model.speechVoiceText)
                .font(.callout)
                .foregroundStyle(.secondary)
                .lineLimit(2)
                .frame(maxWidth: 320, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
                .help("A sound (or a clip) in the prompt is the voice: up to its first 12 seconds are cloned")
                .gridCellColumns(2)
        }
        GridRow {
            label("Length", row: SettingsRow.speechLength)
            selector("Length",
                     [(0, String(localized: "Auto"))]
                        + AppModel.speechLengths.map {
                            ($0, String(localized: "\($0) s"))
                        },
                     tips: [String(localized: "As long as the words take")]
                        + AppModel.speechLengths.map {
                            String(localized: "About \($0) seconds: the model paces the words to it")
                        },
                     selection: speechLength)
            Text("The prompt's fields (Instruction, Quality, Language …) steer the whole of it")
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 230, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// Speech's Length row: 0 is Auto.
    private var speechLength: Binding<Int?> {
        Binding {
            model.speechSeconds ?? 0
        } set: { s in
            guard let s else { return }
            model.speechSeconds = s == 0 ? nil : s
        }
    }

    /// A song's rows: the score it plans first, how long it may run, and
    /// what it will sing -- the prompt's lyrics, under section headers.
    @ViewBuilder
    private var audioRows: some View {
        GridRow {
            label("Score", row: SettingsRow.songPlan)
            selector("Score", SongPlan.allCases.map { ($0, $0.label) },
                     tips: SongPlan.allCases.map(\.hint),
                     selection: optional($model.songPlan))
            Text(model.songPlan.hint)
                .font(.callout)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 230, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
        }
        // The model decides the length; a choice here is the most it may
        // run (a song past it is cut there).
        GridRow {
            label("Length", row: SettingsRow.songLength)
            selector("Length",
                     [(0, String(localized: "Auto"))]
                        + AppModel.songLengths.map {
                            ($0, AppModel.songTime(Double($0)))
                        },
                     tips: [String(localized: "As long as the model makes it")]
                        + AppModel.songLengths.map {
                            let t = AppModel.songTime(Double($0))
                            return String(localized: "At most \(t): a song past it is cut there")
                        },
                     selection: songLength)
            Text(model.songLengthHint)
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        GridRow {
            label("Lyrics", row: SettingsRow.lyrics)
            HStack(spacing: 8) {
                Text(verbatim: model.songLyricsText)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Button("Add Lyrics…", action: addLyrics)
                    .help("Add a lyrics file to the prompt: a text file, sectioned with [Verse] and [Chorus]")
            }
            .gridCellColumns(2)
        }
    }

    private func addLyrics() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.plainText, .text]
        panel.message = String(localized: "Choose a lyrics file")
        panel.directoryURL = model.panelFolder(.attach)
        if panel.runModal() == .OK, let url = panel.url {
            model.rememberPanel(.attach, chose: url)
            withAnimation(AppModel.motion) { model.addLyrics(from: url) }
        }
    }

    /// The Length row's choice: 0 is Auto.
    private var songLength: Binding<Int?> {
        Binding {
            model.songSeconds ?? 0
        } set: { s in
            guard let s else { return }
            model.songSeconds = s == 0 ? nil : s
        }
    }

    /// One of the card's selectors, at the width they share: its items'
    /// labels, or `symbols` in their place (the labels then their
    /// tooltips); `tips`, the tooltips of labelled ones.
    private func selector<T: Hashable>(
        _ name: LocalizedStringKey, _ items: [(T, String)],
        symbols: [String] = [], tips: [String] = [],
        selection: Binding<T?>
    ) -> some View {
        FilledSegments(items: items.map { (tag: $0.0, label: $0.1) },
                       symbols: symbols, tips: tips,
                       selection: selection, width: $selectorWidth)
            .accessibilityLabel(Text(name))
            .fixedSize()
    }

    private func optional<T>(_ b: Binding<T>) -> Binding<T?> {
        Binding { b.wrappedValue } set: { if let v = $0 { b.wrappedValue = v } }
    }

    /// Where the Custom segment is on the Favor selector.
    private var customAnchor: UnitPoint {
        let all = Preference.allCases
        let i = Double(all.firstIndex(of: .custom) ?? all.count - 1)
        return UnitPoint(x: (i + 0.5) / Double(all.count), y: 0)
    }

    private var preference: Binding<Preference> {
        Binding { model.preference } set: { model.choosePreference($0) }
    }

    private func label(_ s: LocalizedStringKey, row: String) -> some View {
        Text(s)
            .foregroundStyle(.secondary)
            .fixedSize()
            .onGeometryChange(for: CGFloat.self) { $0.size.width } action: {
                if $0 > labelWidth { labelWidth = $0 }
            }
            .frame(minWidth: labelWidth, alignment: .trailing)
            .searchMark(model.searchHits.contains(row))
            .suggestMark(model.memorySuggests(row))
            .gridColumnAlignment(.trailing)
    }

    /// Shown as chosen, or as a custom size is shaped; choosing one with a
    /// custom size turns it, or goes back to a preset.
    private var orientation: Binding<Orientation> {
        Binding {
            model.shownOrientation
        } set: { o in
            withAnimation(AppModel.motion) { model.setOrientation(o) }
        }
    }

    /// The preset in use; nil while the length is typed.
    private var clipChoice: Binding<Int?> {
        Binding {
            model.clipSeconds
        } set: { s in
            guard let s else { return }
            model.clipSeconds = s
            model.clipCustomFrames = nil
        }
    }

    private var sizeChoice: Binding<SizeClass?> {
        Binding {
            model.sizeChoice
        } set: { c in
            withAnimation(AppModel.motion) { model.sizeChoice = c }
        }
    }
}

/// A clip's length, as it will be made ("124 frames · 5.2 s"); type
/// another -- "100 frames", "7.5 s" -- to make it custom. It shows the
/// length again on Return or when it loses focus, as the model makes it,
/// or as it was if it could not be read; and follows the presets unless
/// a length is being typed.
private struct ClipLengthField: View {
    @Bindable var model: AppModel
    @State private var text = ""
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("Frames or seconds", text: $text)
            .textFieldStyle(.roundedBorder)
            .font(.callout.monospacedDigit())
            .multilineTextAlignment(.center)
            .frame(width: 150)
            .focused($focused)
            .onSubmit(commit)
            .onChange(of: focused) { _, now in
                if !now { commit() }
            }
            .onAppear { show(model.clipLengthText) }
            .onChange(of: model.clipLengthText) { _, s in
                if text == lastShown { show(s) }
            }
            .help("Type a length: frames (124 frames) or seconds (5 s)")
    }

    private func show(_ s: String) {
        text = s
        lastShown = s
    }

    private func commit() {
        if text != lastShown {
            withAnimation(AppModel.motion) {
                if !model.setClipLength(text) { NSSound.beep() }
            }
        }
        show(model.clipLengthText)
    }
}

/// The output size, as it will be made; type another to make it custom:
/// "1200x800", "1200 × 800" or "1200*800". It shows the size again on
/// Return or when it loses focus -- snapped to the model's grid, or as it
/// was if it could not be read -- and follows the other controls unless a
/// size is being typed.
private struct SizeField: View {
    @Bindable var model: AppModel
    @State private var text = ""
    /// What the field last showed: text that differs is being typed.
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("Width × height", text: $text)
            .textFieldStyle(.roundedBorder)
            .font(.callout.monospacedDigit())
            .multilineTextAlignment(.center)
            .frame(width: 112)
            .focused($focused)
            .onSubmit(commit)
            .onChange(of: focused) { _, now in
                if !now { commit() }
            }
            .onAppear { show(shown) }
            .onChange(of: shown) { _, s in
                if text == lastShown { show(s) }
            }
            .help("Type a size: width × height, as 1200x800 or 1200*800")
    }

    private func show(_ s: String) {
        text = s
        lastShown = s
    }

    /// Pixel sizes are identifiers, not quantities: no digit grouping.
    private var shown: String {
        let d = model.dimensions
        return "\(d.width) × \(d.height)"
    }

    private func commit() {
        if text != lastShown {
            withAnimation(AppModel.motion) {
                if !model.setCustomSize(text) { NSSound.beep() }
            }
        }
        show(shown)
    }
}

/// A segmented selector that FILLS the width it is given, its segments
/// equal. SwiftUI's segmented picker on macOS keeps its own width inside
/// a wider frame, so the generation card's selectors could not share one;
/// this is the same AppKit control, set to fill. Each reports the width
/// its labels need into `width`, which keeps the widest -- the width they
/// all take.
struct FilledSegments<T: Hashable>: NSViewRepresentable {
    let items: [(tag: T, label: String)]
    /// SF Symbols shown in place of the labels, which become the
    /// segments' tooltips and accessibility names.
    var symbols: [String] = []
    /// Tooltips for labelled segments.
    var tips: [String] = []
    @Binding var selection: T?
    @Binding var width: CGFloat

    func makeCoordinator() -> Coordinator { Coordinator() }

    @MainActor
    final class Coordinator: NSObject {
        var pick: (Int) -> Void = { _ in }
        @objc func changed(_ c: NSSegmentedControl) { pick(c.selectedSegment) }
    }

    func makeNSView(context: Context) -> NSSegmentedControl {
        let c = NSSegmentedControl(labels: items.map(\.label),
                                   trackingMode: .selectOne,
                                   target: context.coordinator,
                                   action: #selector(Coordinator.changed(_:)))
        c.segmentDistribution = .fillEqually
        c.setContentHuggingPriority(.defaultLow, for: .horizontal)
        return c
    }

    func updateNSView(_ c: NSSegmentedControl, context: Context) {
        if c.segmentCount != items.count {
            c.segmentCount = items.count
        }
        for (i, item) in items.enumerated() {
            if symbols.indices.contains(i) {
                if c.toolTip(forSegment: i) != item.label {
                    c.setLabel("", forSegment: i)
                    c.setImage(NSImage(systemSymbolName: symbols[i],
                                       accessibilityDescription: item.label),
                               forSegment: i)
                    c.setToolTip(item.label, forSegment: i)
                }
            } else {
                if c.label(forSegment: i) != item.label {
                    c.setLabel(item.label, forSegment: i)
                }
                let tip = tips.indices.contains(i) ? tips[i] : nil
                if c.toolTip(forSegment: i) != tip {
                    c.setToolTip(tip, forSegment: i)
                }
            }
        }
        let sel = selection.flatMap { s in items.firstIndex { $0.tag == s } }
        c.selectedSegment = sel ?? -1
        let tags = items.map(\.tag)
        let binding = $selection
        context.coordinator.pick = { i in
            if tags.indices.contains(i) { binding.wrappedValue = tags[i] }
        }
        c.isEnabled = context.environment.isEnabled
        // The width the labels need, equal segments: the shared width is
        // the widest of these.
        let need = ceil(c.intrinsicContentSize.width)
        if need > width {
            let shared = $width
            DispatchQueue.main.async {
                if need > shared.wrappedValue { shared.wrappedValue = need }
            }
        }
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView c: NSSegmentedControl,
                      context: Context) -> CGSize? {
        let own = c.intrinsicContentSize
        return CGSize(width: max(width, ceil(own.width)), height: own.height)
    }
}

/// A pop-up selector that fills the width it is given: items by tag (nil
/// for a separator), `selection` the tag shown.
struct FilledPopUp: NSViewRepresentable {
    let items: [(tag: String?, label: String)]
    @Binding var selection: String
    let width: CGFloat

    func makeCoordinator() -> Coordinator { Coordinator() }

    @MainActor
    final class Coordinator: NSObject {
        var pick: (String) -> Void = { _ in }
        @objc func changed(_ b: NSPopUpButton) {
            if let tag = b.selectedItem?.representedObject as? String {
                pick(tag)
            }
        }
    }

    func makeNSView(context: Context) -> NSPopUpButton {
        let b = NSPopUpButton(frame: .zero, pullsDown: false)
        b.target = context.coordinator
        b.action = #selector(Coordinator.changed(_:))
        b.setContentHuggingPriority(.defaultLow, for: .horizontal)
        return b
    }

    func updateNSView(_ b: NSPopUpButton, context: Context) {
        let menu = NSMenu()
        for item in items {
            if let tag = item.tag {
                let mi = NSMenuItem(title: item.label, action: nil,
                                    keyEquivalent: "")
                mi.representedObject = tag
                menu.addItem(mi)
            } else {
                menu.addItem(.separator())
            }
        }
        b.menu = menu
        if let i = menu.items.firstIndex(where: {
            ($0.representedObject as? String) == selection
        }) {
            b.selectItem(at: i)
        }
        let binding = $selection
        context.coordinator.pick = { binding.wrappedValue = $0 }
        b.isEnabled = context.environment.isEnabled
    }

    func sizeThatFits(_ proposal: ProposedViewSize, nsView b: NSPopUpButton,
                      context: Context) -> CGSize? {
        let own = b.intrinsicContentSize
        return CGSize(width: max(width, ceil(own.width)), height: own.height)
    }
}
