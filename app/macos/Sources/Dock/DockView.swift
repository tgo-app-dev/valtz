import SwiftUI

/// The DOCK, under the stage (DESIGN §6): the timeline band while a
/// timeline is on the stage (folded to its disclosure while not wanted),
/// over the composer. Not glass -- it is content, on the window, its
/// cards' corners concentric with it.
///
/// Its height is what its content asks -- the prompt growing it, to a
/// share of the column -- until the splitter over it sets one
/// (chrome.dockHeight). Set, that height is the dock's: with the band
/// open the composer keeps its own height (its text held to what the
/// dock leaves after a row of the band) and the band takes the rest, to
/// twelve rows; without it, the prompt fills the composer and scrolls.
/// Either way what the dock holds is worked out from the height set,
/// never from what its content measures, so nothing in it can push
/// back. A double-click on the splitter gives the height back to the
/// content.
///
/// In the Prompt Editor (`.expandedPrompt`) it is the composer grown to
/// the column but the stage's strip (StageColumn): its tabs along its
/// top, the suggestion splitting it. The same card as ever -- grown, not
/// another -- so the prompt's text view, its caret and the keyboard stay
/// in it as it grows and as it goes back.
///
/// A notice, the memory card and the assistant's suggestion float over
/// its top edge, over the stage's foot: nothing moves as they come and
/// go. The suggestion floats only while the dock is not grown: grown, it
/// is half the card (SuggestionSplit).
struct DockView: View {
    @Bindable var model: AppModel
    let layout: WorkspaceLayout
    /// The dock's height, set (the splitter's, held to its bounds); nil:
    /// its content's.
    let height: CGFloat?
    /// The stage column's height: the region and the dock share it.
    let columnHeight: CGFloat

    /// Between the band and the composer; around the dock.
    static let gap = VZ.Space.s
    static let inset = VZ.Space.l

    /// The dock's least and greatest heights in a column `h` tall: a
    /// prompt line and the chips, and all but the stage's least.
    static func bounds(column h: CGFloat) -> ClosedRange<CGFloat> {
        VZ.Size.dockMin...max(VZ.Size.dockMin, h - Self.stageLeast)
    }

    /// What the stage keeps of the column, at least.
    static let stageLeast: CGFloat = 240

    private var expanded: Bool { layout.dock == .expandedPrompt }

    var body: some View {
        // Grown, the dock fills what the strip leaves it.
        let fixed = height != nil || expanded
        VStack(spacing: Self.gap) {
            if model.timelineAvailable {
                TimelineBand(model: model, fills: fixed)
            }
            ComposerCard(model: model, maxPromptHeight: maxPromptHeight,
                         fills: fixed && !model.timelineOpen,
                         expanded: expanded)
                .onGeometryChange(for: CGFloat.self) { $0.size.height }
                    action: { model.dockComposerHeight = $0 }
        }
        .padding(.horizontal, Self.inset)
        .padding(.bottom, Self.inset)
        .frame(height: height, alignment: .top)
        .frame(maxHeight: expanded ? .infinity : nil, alignment: .top)
        .containerShape(.rect(cornerRadius: VZ.Radius.dock))
        .overlay(alignment: .top) { floating }
        .animation(AppModel.motion, value: model.timelineOpen)
        .animation(AppModel.motion, value: model.timelineAvailable)
        // The scissors go with the band.
        .onChange(of: model.timelineOpen) { _, open in
            if !open { model.timelineCutting = false }
        }
    }

    /// While the content sets the height, the text grows the dock to
    /// what leaves the stage its least -- the band's, the chips' and the
    /// card's own rows taken out -- and never past half the column.
    private var maxPromptHeight: CGFloat {
        // The height set, the band open: what a row of the band leaves.
        if let height {
            let room = height - Self.inset - Self.gap - Self.composerRows
                - TimelineCard.height(rows: 1)
            return max(40, room)
        }
        guard columnHeight > 0 else { return 240 }
        let band = model.timelineOpen
            ? TimelineCard.height(rows: AppModel.timelineDefaultRows)
                + Self.gap
            : model.timelineAvailable
            ? TimelineBand.foldedHeight + Self.gap : 0
        let room = columnHeight - Self.stageLeast - band - Self.inset
            - Self.composerRows
        return max(60, min(columnHeight / 2, room))
    }

    /// The composer's rows around its text: the picker, the chips.
    private static let composerRows: CGFloat = 96

    // MARK: Floating

    /// The cards over the dock's top edge -- the memory card, the
    /// suggestion -- and a notice over the topmost: their foot on the
    /// edge (a frame of no height, they standing up out of it), a gap
    /// above, lifted clear of the stage's bar.
    private var floating: some View {
        VStack(spacing: 14) {
            if let n = model.notice {
                NoticeCapsule(notice: n, dismiss: model.dismissNotice)
                    .transition(.move(edge: .bottom)
                        .combined(with: .opacity))
                    .id(n.text)
            }
            if let f = model.memoryFailure, !model.suggesting {
                MemoryFailureCard(model: model, failure: f)
                    .transition(.move(edge: .bottom)
                        .combined(with: .opacity))
            }
            if model.suggesting && !expanded {
                SuggestionCard(prompt: model.enhanced?.prompt
                                   ?? model.enhanceDraft,
                               writing: model.suggestionWriting,
                               stats: model.agentShowsSpeed
                                   ? model.enhanced?.stats : nil,
                               maxTextHeight: SuggestionCard.textCap(
                                   window: columnHeight),
                               accept: model.acceptEnhanced,
                               dismiss: model.dismissEnhanced,
                               review: {
                                   withAnimation(AppModel.motion) {
                                       model.enterImmersivePrompt()
                                   }
                               })
                    .transition(.move(edge: .bottom)
                        .combined(with: .opacity))
            }
        }
        .frame(maxWidth: StageRegion.columnWidth)
        .padding(.horizontal, Self.inset)
        .frame(height: 0, alignment: .bottom)
        .offset(y: -lift)
        .animation(AppModel.motion, value: model.notice?.text)
        .animation(AppModel.motion, value: model.memoryFailure != nil)
        .animation(AppModel.motion, value: model.suggesting)
        .animation(AppModel.motion, value: expanded)
    }

    /// How far over the dock they float: clear of the stage's bar under
    /// the picture (a clip's Loop / Grab Frame / Continue, a still's
    /// pages) and of a clip's scrubber over it. The strip over the grown
    /// dock has neither.
    private var lift: CGFloat {
        let clip = model.stage.video != nil && model.stage.clip == nil
        let shown = model.stageVisible && !expanded
        let bar = shown && (clip || model.stagePagesShown)
        // A clip's scrubber over its bar, but while the timeline is open.
        let scrubber = shown && clip && !model.timelineOpen
        return VZ.Space.m + (bar ? 40 : 0) + (scrubber ? 28 : 0)
    }
}

/// The splitter over the dock: a grip the column's width, the resize
/// cursor over it, a short bar in its middle. Dragged, it sets the
/// dock's height (DockView.bounds); a double-click gives the height back
/// to the dock's content. VoiceOver hears a splitter, its value the
/// dock's height, adjustable in steps.
struct DockSplitter: View {
    let label: String
    let value: String
    var changed: (CGFloat) -> Void
    var ended: () -> Void
    var reset: () -> Void

    static let height: CGFloat = VZ.Space.s + 2

    var body: some View {
        DragHandle(axis: .vertical, label: label, value: value,
                   changed: changed, ended: ended, reset: reset)
            .frame(height: Self.height)
            .frame(maxWidth: .infinity)
            .overlay {
                Capsule()
                    .fill(Color.secondary.opacity(0.35))
                    .frame(width: 36, height: 4)
                    .allowsHitTesting(false)
            }
            .help("Drag to change the dock's height; double-click to fit it to its content")
    }
}

/// A NOTICE (AppModel.flash) as it floats over the dock: its words
/// in a Liquid Glass capsule -- an error's tinted a mild red, a warning's
/// a mild yellow, its sign beside the words. A click lets it go.
struct NoticeCapsule: View {
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
