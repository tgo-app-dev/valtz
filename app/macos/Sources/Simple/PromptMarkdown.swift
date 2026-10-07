import AppKit

/// Markdown in the prompt (DESIGN §10c). The prompt stays PLAIN TEXT --
/// what the model is sent, what a prompt asset keeps -- and Markdown is a
/// way of writing it: `**bold**`, `*italic*` (or `_italic_`),
/// `***both***`, `<u>underline</u>` (as Typora writes it: Markdown has
/// none of its own), `~~struck~~`, `` `code` `` and `#` / `##` / `###`
/// headings. Drawn as it reads (AppModel.promptMarkdown) -- WYSIWYG --
/// the markers are hidden everywhere but in the paragraph the caret is
/// in, where they show, dimmed, so they can be edited; off, the text is
/// drawn as typed.
enum MarkdownStyle: CaseIterable {
    case bold, italic, underline

    var open: String {
        switch self {
        case .bold: "**"
        case .italic: "*"
        case .underline: "<u>"
        }
    }

    var close: String {
        switch self {
        case .bold: "**"
        case .italic: "*"
        case .underline: "</u>"
        }
    }
}

/// One Markdown span: what it styles and its markers.
struct MarkdownRun: Equatable {
    enum Kind: Equatable {
        case bold, italic, boldItalic, underline, strike, code
        case heading(Int)
    }
    let kind: Kind
    let content: NSRange
    let markers: [NSRange]

    /// Its whole extent, markers included.
    var whole: NSRange {
        markers.reduce(content) { NSUnionRange($0, $1) }
    }

    func styles(_ s: MarkdownStyle) -> Bool {
        switch (s, kind) {
        case (.bold, .bold), (.bold, .boldItalic): true
        case (.italic, .italic), (.italic, .boldItalic): true
        case (.underline, .underline): true
        default: false
        }
    }
}

enum PromptMarkdown {
    /// (pattern, kind, options): the markers are groups 1 and 3, the
    /// content group 2. Code first: nothing is read inside it.
    nonisolated(unsafe) private static let inline: [(NSRegularExpression,
                                                     MarkdownRun.Kind)] = {
        let p: [(String, MarkdownRun.Kind)] = [
            (#"(\*\*\*)(?=\S)(.+?)(?<=\S)(\*\*\*)"#, .boldItalic),
            (#"(?<!\*)(\*\*)(?=[^*\s])(.+?)(?<=[^*\s])(\*\*)(?!\*)"#, .bold),
            (#"(?<![\w_])(__)(?=[^_\s])(.+?)(?<=[^_\s])(__)(?![\w_])"#,
             .bold),
            (#"(?<![*\w])(\*)(?=[^*\s])(.+?)(?<=[^*\s])(\*)(?![*\w])"#,
             .italic),
            (#"(?<![\w_])(_)(?=[^_\s])(.+?)(?<=[^_\s])(_)(?![\w_])"#,
             .italic),
            (#"(~~)(?=\S)(.+?)(?<=\S)(~~)"#, .strike),
            (#"(<u>)(.+?)(</u>)"#, .underline),
        ]
        return p.map { (try! NSRegularExpression(pattern: $0.0), $0.1) }
    }()
    nonisolated(unsafe) private static let code = try! NSRegularExpression(
        pattern: #"(`)([^`\n]+)(`)"#)
    nonisolated(unsafe) private static let heading = try! NSRegularExpression(
        pattern: #"^(#{1,3})([ \t]+)(\S.*)$"#, options: .anchorsMatchLines)

    /// Every span in `text`, in no particular order. Spans do not cross a
    /// line; a heading is its line.
    nonisolated static func runs(in text: NSString) -> [MarkdownRun] {
        let all = NSRange(location: 0, length: text.length)
        var out: [MarkdownRun] = []
        for m in heading.matches(in: text as String, range: all) {
            out.append(MarkdownRun(
                kind: .heading(m.range(at: 1).length),
                content: m.range(at: 3),
                markers: [NSUnionRange(m.range(at: 1), m.range(at: 2))]))
        }
        var codes: [NSRange] = []
        for m in code.matches(in: text as String, range: all) {
            codes.append(m.range)
            out.append(MarkdownRun(kind: .code, content: m.range(at: 2),
                                   markers: [m.range(at: 1),
                                             m.range(at: 3)]))
        }
        for (re, kind) in inline {
            for m in re.matches(in: text as String, range: all)
            where !codes.contains(where: {
                NSIntersectionRange($0, m.range).length > 0 }) {
                out.append(MarkdownRun(kind: kind, content: m.range(at: 2),
                                       markers: [m.range(at: 1),
                                                 m.range(at: 3)]))
            }
        }
        return out
    }
}

extension PromptEditor {
    /// The prompt's type with Markdown's styles: Georgia's own bold and
    /// italic faces, a heading larger, code in the system's monospace --
    /// each with the base font's fallbacks for CJK (the system's, at the
    /// matching weight).
    @MainActor
    static func styledFont(bold: Bool, italic: Bool, scale: CGFloat = 1,
                           code: Bool = false) -> NSFont {
        let key = "\(bold)\(italic)\(scale)\(code)"
        if let f = styledFonts[key] { return f }
        let size = (font.pointSize * scale).rounded()
        let f: NSFont
        if code {
            f = .monospacedSystemFont(ofSize: size - 1,
                                      weight: bold ? .semibold : .regular)
        } else {
            let name = ["Georgia", "Georgia-Bold", "Georgia-Italic",
                        "Georgia-BoldItalic"][(bold ? 1 : 0)
                                              + (italic ? 2 : 0)]
            let system = NSFont.systemFont(ofSize: size,
                                           weight: bold ? .semibold
                                                        : .regular)
            if let g = NSFont(name: name, size: size) {
                let langs = Locale.preferredLanguages
                    + ["zh-Hans", "zh-Hant", "ja", "ko"]
                // The fallbacks at the face's weight: a cascade lists
                // families, so bold Chinese is asked for (PingFang's
                // Semibold), or it falls back to the regular.
                let cascade = (CTFontCopyDefaultCascadeListForLanguages(
                    system as CTFont, langs as CFArray)
                    as? [NSFontDescriptor] ?? []).map {
                    bold ? $0.withSymbolicTraits(.bold) : $0
                }
                f = NSFont(descriptor: g.fontDescriptor.addingAttributes(
                    [.cascadeList: cascade]), size: size) ?? g
            } else {
                f = system
            }
        }
        styledFonts[key] = f
        return f
    }

    @MainActor private static var styledFonts: [String: NSFont] = [:]

    /// A hidden marker's: no width to speak of.
    @MainActor static let hiddenFont = NSFont.systemFont(ofSize: 0.01)
}

extension NSTextView {
    /// The style on the selection, or off it: a span of that style around
    /// the selection (or selected whole, markers and all) loses its
    /// markers; otherwise the selection -- its edges' spaces left out --
    /// is wrapped, or, with nothing selected, the markers go in with the
    /// caret between them. One undoable change.
    func toggleMarkdown(_ style: MarkdownStyle) {
        guard let storage = textStorage else { return }
        let text = storage.string as NSString
        var sel = selectedRange()
        // A span of the style the selection (or caret) is in, or that is
        // selected whole, markers and all.
        let inside = { (r: MarkdownRun) in
            sel.location >= r.content.location
                && NSMaxRange(sel) <= NSMaxRange(r.content)
        }
        let whole = { (r: MarkdownRun) in
            sel.length > 0 && NSEqualRanges(sel, r.whole)
        }
        if let run = PromptMarkdown.runs(in: text).first(where: {
            $0.styles(style) && (inside($0) || whole($0))
        }) {
            // Off: its markers out (one star of three kept for the other
            // style of a bold italic).
            let keep = run.kind == .boldItalic
                ? (style == .bold ? "*" : "**") : ""
            let piece = NSMutableAttributedString(string: keep,
                                                  attributes: typingAttributes)
            piece.append(storage.attributedSubstring(from: run.content))
            piece.append(NSAttributedString(string: keep,
                                            attributes: typingAttributes))
            markdownReplace(run.whole, with: piece)
            let start = run.whole.location + (keep as NSString).length
            setSelectedRange(whole(run)
                ? NSRange(location: start, length: run.content.length)
                : NSRange(location: start + sel.location
                              - run.content.location,
                          length: sel.length))
            return
        }
        // Emphasis cannot start or end with a space: leave them out.
        while sel.length > 0,
              CharacterSet.whitespacesAndNewlines.contains(
                  Unicode.Scalar(text.character(at: sel.location)) ?? " ") {
            sel = NSRange(location: sel.location + 1, length: sel.length - 1)
        }
        while sel.length > 0,
              CharacterSet.whitespacesAndNewlines.contains(
                  Unicode.Scalar(text.character(at: NSMaxRange(sel) - 1))
                  ?? " ") {
            sel.length -= 1
        }
        let piece = NSMutableAttributedString(string: style.open,
                                              attributes: typingAttributes)
        piece.append(storage.attributedSubstring(from: sel))
        piece.append(NSAttributedString(string: style.close,
                                        attributes: typingAttributes))
        markdownReplace(sel, with: piece)
        setSelectedRange(NSRange(
            location: sel.location + (style.open as NSString).length,
            length: sel.length))
    }

    /// `range` replaced by `piece` as the text system's own edit: undoable,
    /// and the delegate syncs the model.
    private func markdownReplace(_ range: NSRange,
                                 with piece: NSAttributedString) {
        guard let storage = textStorage,
              shouldChangeText(in: range, replacementString: piece.string)
        else { return }
        storage.replaceCharacters(in: range, with: piece)
        didChangeText()
    }
}

extension PromptEditor {
    /// Each paragraph's spacing: a Latin one 1.1x, a CJK one the
    /// system's (latinParagraph); typing follows the one at the caret.
    @MainActor
    static func styleParagraphs(_ tv: NSTextView) {
        guard let storage = tv.textStorage else { return }
        let ns = storage.string as NSString
        storage.beginEditing()
        ns.enumerateSubstrings(
            in: NSRange(location: 0, length: ns.length),
            options: [.byParagraphs, .substringNotRequired]
        ) { _, _, enclosing, _ in
            guard enclosing.length > 0 else { return }
            let want = PromptEditor.hasCJK(ns.substring(with: enclosing))
                ? PromptEditor.cjkParagraph : PromptEditor.latinParagraph
            let have = storage.attribute(.paragraphStyle,
                                         at: enclosing.location,
                                         effectiveRange: nil)
                as? NSParagraphStyle
            if have != want {
                storage.addAttribute(.paragraphStyle, value: want,
                                     range: enclosing)
            }
        }
        storage.endEditing()
        let at = min(tv.selectedRange().location, ns.length)
        let para = ns.paragraphRange(for: NSRange(location: at,
                                                  length: 0))
        var typing = tv.typingAttributes
        typing[.paragraphStyle] = para.length > 0
            && PromptEditor.hasCJK(ns.substring(with: para))
            ? PromptEditor.cjkParagraph : PromptEditor.latinParagraph
        tv.typingAttributes = typing
    }

    /// The caret's paragraph -- none while the text is not being edited:
    /// every marker hidden.
    @MainActor
    static func caretParagraph(of tv: NSTextView) -> NSRange {
        guard tv.window?.firstResponder === tv else {
            return NSRange(location: NSNotFound, length: 0)
        }
        let ns = tv.string as NSString
        let sel = tv.selectedRange()
        let at = min(sel.location, ns.length)
        return ns.paragraphRange(for: NSRange(
            location: at, length: min(sel.length, ns.length - at)))
    }

    /// Markdown drawn as it reads (PromptMarkdown): every span's style;
    /// its markers dimmed in `caret`'s paragraph, hidden elsewhere. Off:
    /// the text as typed. Attributes only -- the characters, and so the
    /// prompt, are untouched.
    @MainActor
    static func paintMarkdown(_ tv: NSTextView, on: Bool, caret: NSRange) {
        guard let storage = tv.textStorage else { return }
        let ns = storage.string as NSString
        let all = NSRange(location: 0, length: ns.length)
        let runs = on ? PromptMarkdown.runs(in: ns) : []
        let caretParagraph = caret
        storage.beginEditing()
        storage.addAttributes([.font: PromptEditor.font,
                               .foregroundColor: NSColor.labelColor],
                              range: all)
        storage.removeAttribute(.underlineStyle, range: all)
        storage.removeAttribute(.strikethroughStyle, range: all)
        if !runs.isEmpty {
            // Each character's style, from the spans it is in.
            let n = ns.length
            var bold = [Bool](repeating: false, count: n)
            var italic = [Bool](repeating: false, count: n)
            var code = [Bool](repeating: false, count: n)
            var scale = [CGFloat](repeating: 1, count: n)
            func each(_ r: NSRange, _ f: (Int) -> Void) {
                for i in r.location..<min(NSMaxRange(r), n) { f(i) }
            }
            for run in runs {
                switch run.kind {
                case .bold: each(run.content) { bold[$0] = true }
                case .italic: each(run.content) { italic[$0] = true }
                case .boldItalic:
                    each(run.content) { bold[$0] = true; italic[$0] = true }
                case .code: each(run.content) { code[$0] = true }
                case .heading(let level):
                    let k: CGFloat = [1.45, 1.25, 1.1][min(level, 3) - 1]
                    each(run.whole) { scale[$0] = k; bold[$0] = true }
                case .underline:
                    storage.addAttribute(
                        .underlineStyle,
                        value: NSUnderlineStyle.single.rawValue,
                        range: run.content)
                case .strike:
                    storage.addAttribute(
                        .strikethroughStyle,
                        value: NSUnderlineStyle.single.rawValue,
                        range: run.content)
                }
            }
            // Fonts by runs of one style.
            var start = 0
            while start < n {
                var end = start + 1
                while end < n, bold[end] == bold[start],
                      italic[end] == italic[start],
                      code[end] == code[start],
                      scale[end] == scale[start] {
                    end += 1
                }
                if bold[start] || italic[start] || code[start]
                    || scale[start] != 1 {
                    storage.addAttribute(
                        .font,
                        value: PromptEditor.styledFont(
                            bold: bold[start], italic: italic[start],
                            scale: scale[start], code: code[start]),
                        range: NSRange(location: start,
                                       length: end - start))
                }
                start = end
            }
            // The markers: shown, dimmed, where the caret is; hidden
            // everywhere else.
            for run in runs {
                for m in run.markers where m.length > 0 {
                    if caretParagraph.location != NSNotFound
                        && (NSIntersectionRange(m, caretParagraph).length > 0
                            || NSLocationInRange(m.location,
                                                 caretParagraph)) {
                        storage.addAttribute(.foregroundColor,
                                             value: NSColor.tertiaryLabelColor,
                                             range: m)
                    } else {
                        storage.addAttributes(
                            [.font: PromptEditor.hiddenFont,
                             .foregroundColor: NSColor.clear],
                            range: m)
                    }
                }
            }
        }
        storage.endEditing()
        // Typing starts plain; the next pass styles it.
        var typing = tv.typingAttributes
        typing[.font] = PromptEditor.font
        typing[.foregroundColor] = NSColor.labelColor
        typing.removeValue(forKey: .underlineStyle)
        typing.removeValue(forKey: .strikethroughStyle)
        tv.typingAttributes = typing
    }
}
