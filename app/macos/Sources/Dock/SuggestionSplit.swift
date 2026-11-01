import AppKit
import SwiftUI

/// The assistant's suggestion beside the prompt, in the Prompt Editor
/// (the dock grown): the text's room split in two halves, the prompt on
/// the left, the suggestion (SuggestionPane) on the right -- each with
/// its header, Apply and Reject on the suggestion's. The half slides in
/// from the card's right edge as the prompt makes room, and out again.
/// Outside the editor the suggestion floats over the dock instead
/// (SuggestionCard, DockView): the assistant's sparkles open it either
/// way, the card's Prompt Editor button carries it over.
///
/// The prompt keeps its place in either -- the same view, first in the
/// row, whatever comes and goes beside it -- so its text view, its caret
/// and the keyboard carry on as the half opens, and as the dock grows.
/// The halves are cut from the width the row is given (SplitRow), never
/// from a width it measured: a half sized by the row's own width made
/// the window's least width grow with it, at 820 points without end.
struct SuggestionSplit<Prompt: View>: View {
    @Bindable var model: AppModel
    /// The dock grown: the suggestion's half there to open.
    let expanded: Bool
    /// The card's height is the dock's (set, or grown): the row takes
    /// the room it is given, never asking for more.
    var fills = false
    @ViewBuilder var prompt: () -> Prompt

    /// Split: in the editor, with a suggestion written or being written.
    private var splitting: Bool { expanded && model.suggesting }

    var body: some View {
        SplitRow(share: splitting ? 0.5 : 0, fills: fills) {
            VStack(spacing: 0) {
                if expanded {
                    PaneHeader(title: "Prompt")
                        .frame(height: splitting ? PaneHeader.height : 0)
                        .opacity(splitting ? 1 : 0)
                        .clipped()
                        .accessibilityHidden(!splitting)
                }
                prompt()
            }
            if expanded {
                SuggestionPane(model: model)
                    .overlay(alignment: .leading) {
                        Rectangle()
                            .fill(Color(nsColor: .separatorColor))
                            .frame(width: 0.5)
                            .padding(.vertical, 14)
                    }
                    .allowsHitTesting(splitting)
                    .accessibilityHidden(!splitting)
            }
        }
        .clipped()
        .animation(AppModel.motion, value: splitting)
    }
}

/// Two views side by side, the second taking `share` of the width from
/// the right: laid out at half the width whatever its share, so it slides
/// in rather than squeezes (what is past the row's edge is cut by the
/// row). The row is as tall as the first is at its width -- or, filling
/// the card (`fills`), as the height it is given: the window's least
/// height is never the prompt's, which scrolls.
private struct SplitRow: Layout {
    var share: CGFloat
    var fills: Bool

    var animatableData: CGFloat {
        get { share }
        set { share = newValue }
    }

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews,
                      cache: inout ()) -> CGSize {
        guard let first = subviews.first else { return .zero }
        let w = proposal.width ?? first.sizeThatFits(.unspecified).width
        let a = first.sizeThatFits(ProposedViewSize(
            width: w * (1 - share), height: proposal.height))
        return CGSize(width: w,
                      height: fills ? proposal.height ?? a.height : a.height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize,
                       subviews: Subviews, cache: inout ()) {
        let left = bounds.width * (1 - share)
        subviews.first?.place(
            at: bounds.origin,
            proposal: ProposedViewSize(width: left, height: bounds.height))
        if subviews.count > 1 {
            subviews[1].place(
                at: CGPoint(x: bounds.minX + left, y: bounds.minY),
                proposal: ProposedViewSize(width: bounds.width / 2,
                                           height: bounds.height))
        }
    }
}

/// The assistant's rewrite, offered over the dock -- never applied
/// without asking.
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
