import SwiftUI

/// The composer: one card in the dock, under the timeline band.
///
///   create        Video | Image | Audio | Agent, a segmented picker:
///                 what Start makes, chosen before nearly every Start
///   media         the prompt's staged media in a row (ReferenceRow),
///                 the attach menu's recorder or camera after them
///                 (AttachCaptureCard), Clear at the row's end
///   prompt        the text, which mentions them (PromptEditor); its
///                 size, the Prompt Editor and Clear in its top corner
///   chips         Model, Shape, Length, Favor, Seed -- each a popover
///                 -- then Attach, the assistant and Start
///
/// While the dock has no height of its own the text grows the card, to
/// `maxPromptHeight`; once its splitter sets one (`fills`), that height
/// is the dock's and the text fills what the card leaves it, scrolling,
/// never asking for more -- the editor's own measure is not laid out,
/// so it cannot push the dock it sits in.
///
/// `immersive`, the Prompt Editor -- until Phase 5 makes it the dock
/// grown, SimpleView lays it out: the card alone, the height it is given,
/// its tabs down its left side, the suggestion splitting it; no picker,
/// no chips (its toolbar has them).
struct ComposerCard: View {
    @Bindable var model: AppModel
    /// How tall the text may grow the card.
    var maxPromptHeight: CGFloat = 240
    /// The dock's height is set: the text fills the card.
    var fills = false
    var immersive = false
    /// The card's width in the Prompt Editor (wider while it is split).
    var cardWidth: CGFloat = 680
    @State private var promptHeight: CGFloat = 60
    @State private var promptLines = 1
    @State private var dropTargeted = false
    /// The attach capture's card, where it lies in the prompt card; and
    /// its GHOST flying into the thumbnail it became.
    @State private var captureFrame: CGRect = .zero
    @State private var captureGhost: CGRect?
    @State private var ghostFlying = false
    /// The card's coordinates: the capture card and the row's thumbnails.
    nonisolated static let cardSpace = "promptCard"

    /// The text's room on its right, under the corner buttons: the same
    /// whether one or three show, so a button coming as the text wraps
    /// cannot wrap it again.
    private var cornerRoom: CGFloat { immersive ? 44 : 96 }

    var body: some View {
        VStack(spacing: 0) {
            if !immersive {
                header
            }
            media
            textArea
            if !immersive {
                GenerateChipRow(model: model)
                    .padding(.horizontal, VZ.Space.m)
                    .padding(.top, VZ.Space.xs)
                    .padding(.bottom, VZ.Space.m - 2)
            }
        }
        .frame(maxHeight: immersive || fills ? .infinity : nil,
               alignment: .top)
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
        .modifier(CardSurface(immersive: immersive,
                              dropTargeted: dropTargeted))
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
        // A new base brings its size and takes the stage; none gives the
        // stage back. Run from here, not from a didSet on the attachments:
        // a change made inside another property's mutation may never
        // reach the views.
        .onChange(of: model.baseAttachment?.url) { _, url in
            withAnimation(AppModel.motion) { model.baseChanged(to: url) }
        }
        // Custom follows the model Start runs: its family's options.
        .onChange(of: model.runningModel) { _, _ in model.refreshTuning() }
    }

    // MARK: Create

    /// What Start makes. An agent at work holds the project: its box
    /// alone, the others' making waits.
    private var header: some View {
        HStack {
            Picker("Create", selection: modality) {
                ForEach(model.modalities) { m in
                    Label(m.label, systemImage: m.symbol).tag(m)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            .help("What Start makes: a video (⌘1), an image (⌘2), a song (⌘3), or the agent at work on the document (⌘4)")
            Spacer(minLength: 0)
        }
        .padding(.horizontal, VZ.Space.m)
        .padding(.top, VZ.Space.m - 2)
        .padding(.bottom, VZ.Space.xs)
    }

    private var modality: Binding<Modality> {
        Binding {
            model.modality
        } set: { m in
            guard !model.agentHolds || m == .agent else {
                NSSound.beep()
                return
            }
            withAnimation(AppModel.motion) { model.setModality(m) }
        }
    }

    // MARK: Media

    /// The row of staged media over the text, which stays still while the
    /// text scrolls; the capture card after it; a hairline under them.
    @ViewBuilder
    private var media: some View {
        if !model.promptAttachments.isEmpty {
            ReferenceRow(model: model)
                // Clear, at the row's end: the words and the row. Not in
                // the editor: a new prompt is its tabs' +, the old one
                // kept. A capture card has the (x) while it is there.
                .overlay(alignment: .trailing) {
                    if !immersive && model.attachCapture == nil {
                        clearButton.padding(.trailing, VZ.Space.m)
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
                .padding(.top, model.promptAttachments.isEmpty ? 6 : 2)
                .padding(.bottom, VZ.Space.m)
                // Captured, it goes at once: its ghost flies on.
                .transition(.asymmetric(
                    insertion: .opacity,
                    removal: model.capturedItem != nil
                        ? .identity : .opacity))
        }
        if !model.promptAttachments.isEmpty || model.attachCapture != nil {
            Rectangle()
                .fill(Color(nsColor: .separatorColor))
                .frame(height: 0.5)
                .padding(.horizontal, 14)
        }
    }

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

    // MARK: Text

    /// In the editor with the assistant's suggestion, the card split in
    /// two halves (DESIGN §10c).
    private var splitting: Bool { immersive && model.suggesting }

    /// The agent's session, in the Agent tab.
    private var agentSession: AgentSession? {
        model.activeModality == .agent ? model.agent : nil
    }

    /// In the editor, the agent's session takes the card -- its whole
    /// width, the card its own: the request it began from is in it, the
    /// box emptied by Start. The box stays at its foot, a few lines, for
    /// what is said to it next (an answer, more to do).
    private var agentFills: Bool {
        immersive && agentSession != nil && !model.suggesting
    }

    /// The box's height under the agent's session: three lines.
    static let replyHeight: CGFloat = 96

    /// The text -- and, in the editor while there is a suggestion, beside
    /// it the suggestion's half, each with its header. The half opens
    /// and closes by its width, its own laid out whole and cut, so it
    /// slides away rather than squeezes. The agent's session goes over
    /// the text, which keeps its place (the same view: the caret, its
    /// words carry on).
    private var textArea: some View {
        VStack(spacing: 0) {
            if agentFills, let a = agentSession {
                AgentPane(model: model, shown: a)
                    .frame(maxHeight: .infinity)
                    .transition(.opacity)
                Rectangle()
                    .fill(Color(nsColor: .separatorColor))
                    .frame(height: 0.5)
                    .padding(.horizontal, 14)
            }
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
                        .frame(width: splitting ? half : 0,
                               alignment: .leading)
                        .clipped()
                        .allowsHitTesting(splitting)
                        .accessibilityHidden(!splitting)
                }
            }
            .frame(height: agentFills ? Self.replyHeight : nil)
        }
        .frame(maxHeight: immersive || fills ? .infinity : nil)
    }

    /// The box grows with the text, or -- the dock's height set, or the
    /// editor -- fills the room it is given.
    private var flexible: Bool { immersive || fills }

    private var promptText: some View {
        PromptEditor(model: model, height: $promptHeight,
                     dropTargeted: $dropTargeted, lines: $promptLines,
                     compact: !fills && model.promptCompact,
                     maxHeight: maxPromptHeight)
            // A voice-over takes no prompt: its words greyed, not typed
            // into or dropped on.
            .opacity(model.inVoiceOver ? 0.35 : 1)
            .allowsHitTesting(!model.inVoiceOver)
            .frame(height: flexible ? nil : promptHeight)
            .frame(minHeight: flexible ? 40 : nil,
                   maxHeight: flexible ? .infinity : nil)
            // Text scrolled into the padding fades out at the card's edge.
            .mask(scrollFade)
            .padding(.trailing, cornerRoom)
            // Beside the words, under the buttons: the text's too -- a
            // click, a drop.
            .background { PromptMargin(model: model) }
            .overlay(alignment: .topLeading) {
                if model.prompt.isEmpty && model.promptAttachments.isEmpty {
                    // How the model Start runs reads a prompt.
                    // Clear of the corner buttons, as the text is.
                    Text(verbatim: model.promptHint)
                        .font(Font(PromptEditor.font))
                        .foregroundStyle(.tertiary)
                        .lineLimit(2)
                        .padding(.leading, PromptEditor.inset.width + 5)
                        .padding(.trailing, cornerRoom
                                 + PromptEditor.inset.width)
                        .padding(.top, PromptEditor.inset.height)
                        .allowsHitTesting(false)
                }
            }
            .overlay(alignment: .topTrailing) {
                HStack(spacing: 2) {
                    // Past three lines, and growing with them: its size.
                    if !flexible && promptLines > 3 { sizeToggle }
                    // A locked prompt: a copy of it to change.
                    if immersive && model.activeTabReadOnly {
                        duplicateButton
                    }
                    // A tab only looked at never goes to the box.
                    if !model.activeTabDoNotApply { editorButton }
                    if clearInText { clearButton }
                }
                .padding(.trailing, VZ.Space.m)
                .padding(.top, 13)
                .foregroundStyle(.secondary)
            }
    }

    /// Something to clear: words, or media in the row.
    private var hasPromptContent: Bool {
        !model.prompt.isEmpty || !model.promptAttachments.isEmpty
    }

    /// Clear is in the text's corner when there is no row of media to
    /// hold it. Never in the Prompt Editor: there a new prompt is a new
    /// tab (its +), and the one written stays in its own.
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
        .shortcutHint("⇧⌘E")
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

    /// Past three lines: the whole prompt (the box grows with it), or
    /// three rows with a scroll bar.
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
        .help(model.promptCompact ? "Show the whole prompt"
                                  : "Show three lines, with a scroll bar")
        .accessibilityLabel(Text("Prompt Size"))
    }
}

/// The card's surface: in the dock, a dock card (concentric with the
/// dock); in the Prompt Editor, the card it has been, square on top
/// against the editor's toolbar. Its rim the accent while a drop is
/// over it.
private struct CardSurface: ViewModifier {
    let immersive: Bool
    let dropTargeted: Bool

    func body(content: Content) -> some View {
        if immersive {
            let shape = UnevenRoundedRectangle(topLeadingRadius: 0,
                                               bottomLeadingRadius: 20,
                                               bottomTrailingRadius: 20,
                                               topTrailingRadius: 0,
                                               style: .continuous)
            content
                .clipShape(shape)
                .background(shape.fill(Color(nsColor: .textBackgroundColor)))
                .overlay(shape.strokeBorder(dropTargeted
                                            ? Color.accentColor
                                            : Color.vzHairline,
                                            lineWidth: dropTargeted ? 2 : 0.5))
                .vzShadow(VZ.Elevation.dock)
        } else {
            DockCard { content }
                .overlay {
                    if dropTargeted {
                        VZ.Radius.concentric()
                            .stroke(Color.accentColor, lineWidth: 2)
                            .padding(1)
                    }
                }
        }
    }
}
