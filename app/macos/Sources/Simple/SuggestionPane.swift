import AppKit
import SwiftUI

/// The assistant's suggestion, in the Prompt Editor (DESIGN §10c): the
/// right half of the prompt card, the prompt it was made from on the left.
/// It opens as the assistant starts and fills as it writes (read only
/// until it is done; Reject then stops it). Once done it can be edited
/// before it is applied -- the edits are the suggestion
/// (`AppModel.enhanced`) -- then Apply makes it the prompt, Reject lets it
/// go; either way the half closes. Written as the prompt is: Georgia,
/// Markdown drawn as it reads, ⌘B / ⌘I / ⌘U.
struct SuggestionPane: View {
    @Bindable var model: AppModel

    var body: some View {
        let writing = model.suggestionWriting
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Label("Suggestion", systemImage: "sparkles")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                if writing {
                    ProgressView().controlSize(.mini)
                    Text("Writing…")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer(minLength: 8)
                Button("Reject") { model.dismissEnhanced() }
                    .help(writing ? "Stop the assistant; the prompt stays as it is"
                                  : "Keep the prompt as it is")
                Button("Apply") { model.acceptEnhanced() }
                    .buttonStyle(.borderedProminent)
                    .disabled(writing)
                    .help("Make this the prompt")
            }
            .controlSize(.small)
            .padding(.leading, PromptEditor.inset.width + 5)
            .padding(.trailing, 12)
            .frame(height: PaneHeader.height)
            SuggestionEditor(model: model)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}

/// A half's header: what it holds. The prompt's, beside the suggestion's.
struct PaneHeader: View {
    let title: LocalizedStringKey
    static let height: CGFloat = 36

    var body: some View {
        Text(title)
            .font(.caption.weight(.semibold))
            .foregroundStyle(.secondary)
            .lineLimit(1)
            .padding(.leading, PromptEditor.inset.width + 5)
            .frame(maxWidth: .infinity, maxHeight: .infinity,
                   alignment: .leading)
    }
}

/// The suggestion's text: plain, as the prompt's is, with its Markdown
/// drawn. While the assistant writes, its words as they come (read only,
/// following the end); then the suggestion, each edit written back.
/// Once the suggestion is gone it keeps its text, so the half closes
/// with it still there.
struct SuggestionEditor: NSViewRepresentable {
    let model: AppModel

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: SuggestionEditor
        weak var textView: SuggestionTextView?
        /// The suggestion as loaded, or as last written back.
        var shown: String?
        var markdownOn: Bool?
        var caret = NSRange(location: NSNotFound, length: 0)

        init(_ p: SuggestionEditor) { parent = p }

        func style() {
            guard let tv = textView else { return }
            PromptEditor.styleParagraphs(tv)
            caret = PromptEditor.caretParagraph(of: tv)
            let on = parent.model.promptMarkdown
            if markdownOn == false && !on { return }
            markdownOn = on
            PromptEditor.paintMarkdown(tv, on: on, caret: caret)
        }

        func textDidChange(_ notification: Notification) {
            // An input method's unconfirmed text is left alone until it is
            // committed or cancelled (as the prompt's, PromptEditor.sync):
            // restyled under it, it was laid out at the start of its CJK
            // run.
            guard let tv = textView, !tv.hasMarkedText() else { return }
            style()
            shown = tv.string
            parent.model.enhanced?.prompt = tv.string
        }

        func textViewDidChangeSelection(_ notification: Notification) {
            guard let tv = textView, !tv.hasMarkedText(),
                  parent.model.promptMarkdown else {
                return
            }
            if !NSEqualRanges(PromptEditor.caretParagraph(of: tv), caret) {
                style()
            }
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        // TextKit 1, as the prompt's: the same layout, the same look.
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        storage.addLayoutManager(layout)
        let container = NSTextContainer(
            size: NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude))
        container.widthTracksTextView = true
        layout.addTextContainer(container)
        let tv = SuggestionTextView(frame: .zero, textContainer: container)
        tv.delegate = context.coordinator
        tv.isRichText = true           // Markdown's styles live in it
        tv.importsGraphics = false
        tv.allowsUndo = true
        tv.drawsBackground = false
        tv.isVerticallyResizable = true
        tv.isHorizontallyResizable = false
        tv.minSize = .zero
        tv.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                            height: CGFloat.greatestFiniteMagnitude)
        tv.autoresizingMask = [.width]
        tv.textContainerInset = NSSize(width: PromptEditor.inset.width,
                                       height: 0)
        tv.font = PromptEditor.font
        tv.textColor = .labelColor
        tv.insertionPointColor = .controlAccentColor
        tv.typingAttributes = [.font: PromptEditor.font,
                               .foregroundColor: NSColor.labelColor,
                               .paragraphStyle: PromptEditor.latinParagraph]
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        let model = model
        tv.onFocus = { [weak tv] in model.promptTextView = tv }
        tv.onFocusChange = { [weak coordinator = context.coordinator] in
            coordinator?.style()
        }
        context.coordinator.textView = tv

        let scroll = NSScrollView()
        scroll.documentView = tv
        scroll.drawsBackground = false
        scroll.automaticallyAdjustsContentInsets = false
        scroll.contentInsets = NSEdgeInsets(top: 4, left: 0,
                                            bottom: PromptEditor.inset.height,
                                            right: 0)
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        let c = context.coordinator
        c.parent = self
        guard let tv = c.textView else { return }
        let writing = model.suggestionWriting
        if tv.isEditable == writing { tv.isEditable = !writing }
        let text = model.enhanced?.prompt
            ?? (writing ? model.enhanceDraft : nil)
        if let text, text != c.shown {
            let was = c.shown ?? ""
            c.shown = text
            if !was.isEmpty && text.hasPrefix(was) {
                // Written on: what is new, at the end -- followed while
                // the assistant writes.
                tv.textStorage?.append(NSAttributedString(
                    string: String(text.dropFirst(was.count)),
                    attributes: tv.typingAttributes))
                c.style()
                if writing { tv.scrollToEndOfDocument(nil) }
            } else {
                // A new suggestion: its words, from the top.
                tv.textStorage?.setAttributedString(NSAttributedString(
                    string: text, attributes: tv.typingAttributes))
                tv.setSelectedRange(NSRange(location: 0, length: 0))
                c.style()
                tv.scrollRangeToVisible(NSRange(location: 0, length: 0))
            }
        } else if c.markdownOn != nil, c.markdownOn != model.promptMarkdown {
            c.style()
        }
    }
}

/// The suggestion's text view: plain text pasted plain; ⌘B / ⌘I / ⌘U
/// are Markdown, as in the prompt.
final class SuggestionTextView: NSTextView {
    var onFocus: (() -> Void)?
    var onFocusChange: (() -> Void)?

    override func becomeFirstResponder() -> Bool {
        let took = super.becomeFirstResponder()
        if took {
            onFocus?()
            onFocusChange?()
        }
        return took
    }

    override func resignFirstResponder() -> Bool {
        let gave = super.resignFirstResponder()
        if gave { onFocusChange?() }
        return gave
    }

    override func paste(_ sender: Any?) {
        pasteAsPlainText(sender)
    }

    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if window?.firstResponder === self,
           event.modifierFlags.intersection(.deviceIndependentFlagsMask)
               == .command {
            switch event.charactersIgnoringModifiers {
            case "b": toggleMarkdown(.bold); return true
            case "i": toggleMarkdown(.italic); return true
            case "u": toggleMarkdown(.underline); return true
            default: break
            }
        }
        return super.performKeyEquivalent(with: event)
    }
}
