import AppKit

/// CODE in the Prompt Editor (DESIGN §10c): a text asset that is a
/// program (DESIGN §4t) -- or a text named as a code file is ("main.js",
/// "shader.glsl") -- opened in a tab is edited as code. The text view's
/// type is monospaced, its lines numbered in a gutter at its left, its
/// tokens tinted by language, the bracket beside the caret and its match
/// marked; Return keeps the line's indent, Tab puts in spaces. The words
/// are untouched: only their look changes (attributes), as Markdown's.
enum CodeSyntax {
    enum Kind: Sendable { case comment, string, keyword, number }

    /// The language a text asset is written in -- a program's JavaScript
    /// (HTML when it is a page), else by its name's extension -- nil for
    /// one that is not code.
    @MainActor
    static func language(of a: AssetDTO) -> String? {
        guard a.kind == "text" else { return nil }
        if a.isProgram {
            let head = (a.text ?? "").drop { $0.isWhitespace }
            return head.hasPrefix("<") ? "html" : "javascript"
        }
        let ext = (a.name as NSString).pathExtension
        return ext.isEmpty ? nil : normal(ext)
    }

    /// A language's name, or one of its files' extensions, as the
    /// tokenizer knows it; nil for one it does not.
    nonisolated static func normal(_ l: String) -> String? {
        switch l.lowercased() {
        case "js", "javascript", "mjs", "cjs", "jsx": "javascript"
        case "ts", "typescript", "tsx": "typescript"
        case "json": "json"
        case "html", "htm", "xhtml", "svg", "xml": "html"
        case "css": "css"
        case "glsl", "vert", "frag", "vs", "fs": "glsl"
        case "wgsl": "wgsl"
        case "c", "h", "cc", "cpp", "hpp", "cxx", "m", "mm", "objc",
             "metal": "c"
        case "swift": "swift"
        case "py", "python": "python"
        case "sh", "bash", "zsh", "shell": "shell"
        default: nil
        }
    }

    /// The language as a person reads it.
    nonisolated static func title(_ language: String) -> String {
        switch language {
        case "javascript": "JavaScript"
        case "typescript": "TypeScript"
        case "json": "JSON"
        case "html": "HTML"
        case "css": "CSS"
        case "glsl": "GLSL"
        case "wgsl": "WGSL"
        case "c": "C"
        case "swift": "Swift"
        case "python": "Python"
        case "shell": "Shell"
        default: language
        }
    }

    /// What a language's tokens are made of.
    private struct Spec {
        var lineComments: [String] = ["//"]
        var block: (String, String)? = ("/*", "*/")
        var quotes: Set<UInt16> = [0x22, 0x27]  // " '
        var multiline: Set<UInt16> = []          // a quote that spans lines
        var keywords: Set<String> = []
    }

    private static let jsWords: Set<String> = [
        "await", "async", "break", "case", "catch", "class", "const",
        "continue", "debugger", "default", "delete", "do", "else", "export",
        "extends", "false", "finally", "for", "from", "function", "get",
        "if", "import", "in", "instanceof", "let", "new", "null", "of",
        "return", "set", "static", "super", "switch", "this", "throw",
        "true", "try", "typeof", "undefined", "var", "void", "while",
        "with", "yield",
    ]

    private static func spec(_ language: String) -> Spec? {
        switch language {
        case "javascript":
            return Spec(quotes: [0x22, 0x27, 0x60], multiline: [0x60],
                        keywords: jsWords)
        case "typescript":
            return Spec(quotes: [0x22, 0x27, 0x60], multiline: [0x60],
                        keywords: jsWords.union([
                            "abstract", "any", "as", "boolean", "declare",
                            "enum", "implements", "interface", "keyof",
                            "namespace", "never", "number", "private",
                            "protected", "public", "readonly", "string",
                            "type", "unknown",
                        ]))
        case "json":
            return Spec(lineComments: [], block: nil, quotes: [0x22],
                        keywords: ["true", "false", "null"])
        case "css":
            return Spec(lineComments: [], keywords: [
                "important", "inherit", "initial", "none", "auto",
            ])
        case "glsl", "wgsl", "c":
            return Spec(keywords: [
                // C and Metal
                "auto", "break", "case", "char", "const", "constexpr",
                "continue", "default", "do", "double", "else", "enum",
                "extern", "float", "for", "if", "inline", "int", "long",
                "return", "short", "signed", "sizeof", "static", "struct",
                "switch", "typedef", "union", "unsigned", "void",
                "volatile", "while", "true", "false", "nullptr", "NULL",
                "bool", "class", "namespace", "using", "template",
                "kernel", "device", "constant", "thread",
                // GLSL
                "uniform", "varying", "attribute", "in", "out", "inout",
                "precision", "highp", "mediump", "lowp", "discard",
                "vec2", "vec3", "vec4", "ivec2", "ivec3", "ivec4", "mat2",
                "mat3", "mat4", "sampler2D", "samplerCube", "uint",
                "layout",
                // WGSL
                "fn", "let", "var", "loop", "f32", "i32", "u32", "vec2f",
                "vec3f", "vec4f", "mat4x4f", "array", "override",
            ])
        case "swift":
            return Spec(quotes: [0x22], keywords: [
                "actor", "as", "async", "await", "break", "case", "catch",
                "class", "continue", "default", "defer", "do", "else",
                "enum", "extension", "false", "for", "func", "guard", "if",
                "import", "in", "init", "inout", "is", "let", "nil",
                "private", "protocol", "public", "repeat", "return",
                "self", "Self", "static", "struct", "super", "switch",
                "throw", "throws", "true", "try", "var", "where", "while",
            ])
        case "python":
            return Spec(lineComments: ["#"], block: nil, keywords: [
                "and", "as", "assert", "async", "await", "break", "class",
                "continue", "def", "del", "elif", "else", "except",
                "False", "finally", "for", "from", "global", "if",
                "import", "in", "is", "lambda", "None", "nonlocal", "not",
                "or", "pass", "raise", "return", "self", "True", "try",
                "while", "with", "yield",
            ])
        case "shell":
            return Spec(lineComments: ["#"], block: nil, keywords: [
                "case", "do", "done", "elif", "else", "esac", "export",
                "fi", "for", "function", "if", "in", "local", "return",
                "then", "until", "while",
            ])
        default:
            return nil
        }
    }

    /// The tokens worth tinting, in order: ranges of `u` (UTF-16, as an
    /// NSString counts) with their kind. A light pass, not a parser.
    nonisolated static func tokens(_ u: [UInt16], language: String)
        -> [(NSRange, Kind)] {
        if language == "html" { return html(u, 0, u.count) }
        guard let s = spec(language) else { return [] }
        return tokens(u, 0, u.count, s)
    }

    private static func isIdent(_ c: UInt16) -> Bool {
        (c >= 0x30 && c <= 0x39) || (c >= 0x41 && c <= 0x5A)
            || (c >= 0x61 && c <= 0x7A) || c == 0x5F || c == 0x24
            || c >= 0x80
    }

    private static func isDigit(_ c: UInt16) -> Bool {
        c >= 0x30 && c <= 0x39
    }

    private static func starts(_ u: [UInt16], _ i: Int, _ end: Int,
                               _ w: [UInt16]) -> Bool {
        guard !w.isEmpty, i + w.count <= end else { return false }
        for k in 0..<w.count where u[i + k] != w[k] { return false }
        return true
    }

    private static func tokens(_ u: [UInt16], _ from: Int, _ end: Int,
                               _ s: Spec) -> [(NSRange, Kind)] {
        var out: [(NSRange, Kind)] = []
        let lines = s.lineComments.map { Array($0.utf16) }
        let open = s.block.map { Array($0.0.utf16) }
        let close = s.block.map { Array($0.1.utf16) }
        var i = from
        func emit(_ a: Int, _ b: Int, _ k: Kind) {
            if b > a { out.append((NSRange(location: a, length: b - a), k)) }
        }
        while i < end {
            let c = u[i]
            if lines.contains(where: { starts(u, i, end, $0) }) {
                var j = i
                while j < end, u[j] != 0x0A { j += 1 }
                emit(i, j, .comment)
                i = j
            } else if let open, let close, starts(u, i, end, open) {
                var j = i + open.count
                while j < end, !starts(u, j, end, close) { j += 1 }
                j = min(end, j + close.count)
                emit(i, j, .comment)
                i = j
            } else if s.quotes.contains(c) {
                var j = i + 1
                let spans = s.multiline.contains(c)
                while j < end, u[j] != c, spans || u[j] != 0x0A {
                    if u[j] == 0x5C { j += 1 }  // backslash
                    j += 1
                }
                j = min(end, j < end && u[j] == c ? j + 1 : j)
                emit(i, j, .string)
                i = j
            } else if isDigit(c), i == from || !isIdent(u[i - 1]) {
                var j = i + 1
                while j < end, isIdent(u[j]) || u[j] == 0x2E { j += 1 }
                emit(i, j, .number)
                i = j
            } else if isIdent(c) {
                var j = i + 1
                while j < end, isIdent(u[j]) { j += 1 }
                if !s.keywords.isEmpty, j - i <= 16,
                   s.keywords.contains(String(decoding: u[i..<j],
                                              as: UTF16.self)) {
                    emit(i, j, .keyword)
                }
                i = j
            } else {
                i += 1
            }
        }
        return out
    }

    /// A page's: comments, tags' names, attribute values; a script's
    /// JavaScript and a style's CSS within them.
    private static func html(_ u: [UInt16], _ from: Int, _ end: Int)
        -> [(NSRange, Kind)] {
        var out: [(NSRange, Kind)] = []
        let comment = Array("<!--".utf16), uncomment = Array("-->".utf16)
        var i = from
        func lower(_ a: Int, _ b: Int) -> String {
            String(decoding: u[a..<b], as: UTF16.self).lowercased()
        }
        while i < end {
            if starts(u, i, end, comment) {
                var j = i + comment.count
                while j < end, !starts(u, j, end, uncomment) { j += 1 }
                j = min(end, j + uncomment.count)
                out.append((NSRange(location: i, length: j - i), .comment))
                i = j
                continue
            }
            guard u[i] == 0x3C else { i += 1; continue }  // <
            var j = i + 1
            if j < end, u[j] == 0x2F || u[j] == 0x21 || u[j] == 0x3F {
                j += 1  // / ! ?
            }
            let name = j
            while j < end, isIdent(u[j]) || u[j] == 0x2D || u[j] == 0x3A {
                j += 1
            }
            guard j > name else { i += 1; continue }
            out.append((NSRange(location: name, length: j - name),
                        .keyword))
            let tag = lower(name, j)
            let closing = u[i + 1] == 0x2F
            // Its attributes up to its ">": the values strings.
            while j < end, u[j] != 0x3E {
                if u[j] == 0x22 || u[j] == 0x27 {
                    let q = u[j]
                    var k = j + 1
                    while k < end, u[k] != q { k += 1 }
                    k = min(end, k + 1)
                    out.append((NSRange(location: j, length: k - j),
                                .string))
                    j = k
                } else {
                    j += 1
                }
            }
            j = min(end, j + 1)
            i = j
            // A script's or a style's body: its own language, to its end.
            if !closing, tag == "script" || tag == "style" {
                let shut = Array("</\(tag)".utf16)
                var k = j
                while k < end {
                    if u[k] == 0x3C, k + shut.count <= end,
                       lower(k, k + shut.count) == "</\(tag)" { break }
                    k += 1
                }
                if let s = spec(tag == "script" ? "javascript" : "css") {
                    out += tokens(u, j, k, s)
                }
                i = k
            }
        }
        return out
    }

    /// The code's runs covering it whole (`nil` kind: plain), as String
    /// ranges -- a chat's code block (CodeHighlight).
    nonisolated static func runs(_ code: String, language: String)
        -> [(Range<String.Index>, Kind?)] {
        let u = Array(code.utf16)
        var out: [(Range<String.Index>, Kind?)] = []
        var at = code.startIndex
        var offset = 0
        func index(_ o: Int) -> String.Index {
            // Forward from the last: one walk over the text in all.
            let i = code.utf16.index(at, offsetBy: o - offset)
            return i
        }
        for (r, k) in tokens(u, language: language) {
            let a = index(r.location)
            if at < a { out.append((at..<a, nil)) }
            at = a
            offset = r.location
            let b = index(r.location + r.length)
            out.append((a..<b, k))
            at = b
            offset = r.location + r.length
        }
        if at < code.endIndex { out.append((at..<code.endIndex, nil)) }
        return out
    }
}

/// The text view as a code editor: its look, its brackets, its keys.
@MainActor
enum CodeEditing {
    static let font = NSFont.monospacedSystemFont(ofSize: 13,
                                                  weight: .regular)
    static let strong = NSFont.monospacedSystemFont(ofSize: 13,
                                                    weight: .semibold)
    /// No room between lines; a tab four spaces wide.
    static let paragraph: NSParagraphStyle = {
        let p = NSMutableParagraphStyle()
        p.lineHeightMultiple = 1.1
        p.paragraphSpacing = 0
        let space = (" " as NSString).size(withAttributes: [.font: font])
        p.tabStops = []
        p.defaultTabInterval = space.width * 4
        return p
    }()
    static var typing: [NSAttributedString.Key: Any] {
        [.font: font, .foregroundColor: NSColor.labelColor,
         .paragraphStyle: paragraph]
    }

    static func color(_ k: CodeSyntax.Kind) -> NSColor {
        switch k {
        case .comment: .secondaryLabelColor
        case .string: .systemRed
        case .keyword: .systemPink
        case .number: .systemBlue
        }
    }

    /// The whole text in the code's look, its tokens tinted; the ranges
    /// of its strings and comments, where a bracket is no bracket.
    static func paint(_ tv: NSTextView, language: String) -> [NSRange] {
        guard let storage = tv.textStorage else { return [] }
        let all = NSRange(location: 0, length: storage.length)
        let tokens = CodeSyntax.tokens(Array(storage.string.utf16),
                                       language: language)
        storage.beginEditing()
        storage.addAttributes(typing, range: all)
        for (r, k) in tokens where NSMaxRange(r) <= storage.length {
            storage.addAttribute(.foregroundColor, value: color(k),
                                 range: r)
            if k == .keyword {
                storage.addAttribute(.font, value: strong, range: r)
            }
        }
        storage.endEditing()
        tv.typingAttributes = typing
        return tokens.filter { $0.1 == .comment || $0.1 == .string }
            .map(\.0)
    }

    /// The bracket beside the caret -- before it, else after it -- and its
    /// match: both, or the bracket alone when it has none (`matched`
    /// false). Brackets in strings and comments (`skip`, in order) are
    /// none.
    static func brackets(_ tv: NSTextView, skip: [NSRange])
        -> (ranges: [NSRange], matched: Bool)? {
        let ns = tv.string as NSString
        let sel = tv.selectedRange()
        guard sel.length == 0 else { return nil }
        func skipped(_ i: Int) -> Bool {
            var lo = 0, hi = skip.count
            while lo < hi {
                let mid = (lo + hi) / 2
                if NSMaxRange(skip[mid]) <= i { lo = mid + 1 } else { hi = mid }
            }
            return lo < skip.count && NSLocationInRange(i, skip[lo])
        }
        let pairs: [UInt16: (UInt16, Int)] = [
            0x28: (0x29, 1), 0x5B: (0x5D, 1), 0x7B: (0x7D, 1),  // ( [ {
            0x29: (0x28, -1), 0x5D: (0x5B, -1), 0x7D: (0x7B, -1),
        ]
        for at in [sel.location - 1, sel.location]
        where at >= 0 && at < ns.length {
            let c = ns.character(at: at)
            guard let (other, step) = pairs[c], !skipped(at) else {
                continue
            }
            var depth = 0
            var i = at + step
            var looked = 0
            while i >= 0, i < ns.length, looked < 200_000 {
                looked += 1
                let d = ns.character(at: i)
                if (d == c || d == other) && !skipped(i) {
                    if d == c {
                        depth += 1
                    } else if depth == 0 {
                        return ([NSRange(location: at, length: 1),
                                 NSRange(location: i, length: 1)], true)
                    } else {
                        depth -= 1
                    }
                }
                i += step
            }
            return ([NSRange(location: at, length: 1)], false)
        }
        return nil
    }

    /// Return: a new line at the indent of the caret's, one step further
    /// after an opening bracket.
    static func newline(_ tv: NSTextView) {
        let ns = tv.string as NSString
        let sel = tv.selectedRange()
        let line = ns.lineRange(for: NSRange(location: sel.location,
                                             length: 0))
        var indent = ""
        var i = line.location
        while i < sel.location, i < ns.length {
            let c = ns.character(at: i)
            guard c == 0x20 || c == 0x09 else { break }
            indent.append(c == 0x09 ? "\t" : " ")
            i += 1
        }
        var j = sel.location - 1
        while j >= line.location, ns.character(at: j) == 0x20 { j -= 1 }
        if j >= line.location,
           [0x7B, 0x5B, 0x28].contains(ns.character(at: j)) {
            indent += "  "
        }
        tv.insertText("\n" + indent, replacementRange: sel)
    }

    /// Tab: spaces to the next stop of two.
    static func tab(_ tv: NSTextView) {
        let ns = tv.string as NSString
        let sel = tv.selectedRange()
        let line = ns.lineRange(for: NSRange(location: sel.location,
                                             length: 0))
        let column = sel.location - line.location
        tv.insertText(String(repeating: " ", count: 2 - column % 2),
                      replacementRange: sel)
    }
}

/// The code's LINE NUMBERS, a gutter at the text's left: each line's
/// number where its first piece is laid (a wrapped line's others none),
/// the caret's line brighter.
final class LineNumberRuler: NSRulerView {
    private weak var text: NSTextView?
    /// Where each line starts (UTF-16), from 0.
    private(set) var starts: [Int] = [0]
    static let numberFont = NSFont.monospacedDigitSystemFont(
        ofSize: 10.5, weight: .regular)

    init(scrollView: NSScrollView, textView: NSTextView) {
        text = textView
        super.init(scrollView: scrollView, orientation: .verticalRuler)
        clientView = textView
        scrollView.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            self, selector: #selector(scrolled),
            name: NSView.boundsDidChangeNotification,
            object: scrollView.contentView)
        refresh()
    }

    required init(coder: NSCoder) { fatalError("not used") }

    @objc private func scrolled(_ n: Notification) { needsDisplay = true }

    override var isFlipped: Bool { true }

    /// The text changed: its lines counted again, the gutter as wide as
    /// its numbers.
    func refresh() {
        guard let tv = text else { return }
        var s = [0]
        for (i, c) in tv.string.utf16.enumerated() where c == 0x0A {
            s.append(i + 1)
        }
        starts = s
        let digits = max(2, String(s.count).count)
        let w = ceil(CGFloat(digits) * ("8" as NSString).size(
            withAttributes: [.font: Self.numberFont]).width + 16)
        if abs(ruleThickness - w) > 0.5 { ruleThickness = w }
        needsDisplay = true
    }

    /// The line `i` (UTF-16) is on, from 0.
    func line(at i: Int) -> Int {
        var lo = 0, hi = starts.count
        while lo < hi {
            let mid = (lo + hi) / 2
            if starts[mid] <= i { lo = mid + 1 } else { hi = mid }
        }
        return max(0, lo - 1)
    }

    override func draw(_ dirtyRect: NSRect) {
        guard let tv = text, let lm = tv.layoutManager,
              let tc = tv.textContainer else { return }
        // The system's hairline, in its own faint alpha.
        NSColor.separatorColor.setFill()
        NSRect(x: bounds.maxX - 1, y: dirtyRect.minY, width: 1,
               height: dirtyRect.height).fill()
        let caret = line(at: tv.selectedRange().location)
        let origin = tv.textContainerOrigin
        func number(_ n: Int, _ frag: NSRect) {
            let top = convert(NSPoint(x: 0, y: frag.minY + origin.y),
                              from: tv).y
            let s = "\(n + 1)" as NSString
            let attrs: [NSAttributedString.Key: Any] = [
                .font: Self.numberFont,
                .foregroundColor: n == caret ? NSColor.labelColor
                                             : NSColor.tertiaryLabelColor,
            ]
            let size = s.size(withAttributes: attrs)
            let lineHeight = lm.defaultLineHeight(for: CodeEditing.font)
            let y = top + (min(frag.height, lineHeight * 1.1)
                           - size.height) / 2
            guard y + size.height >= dirtyRect.minY - 2,
                  y <= dirtyRect.maxY + 2 else { return }
            s.draw(at: NSPoint(x: bounds.maxX - 8 - size.width, y: y),
                   withAttributes: attrs)
        }
        let glyphs = lm.glyphRange(forBoundingRect: tv.visibleRect, in: tc)
        lm.enumerateLineFragments(forGlyphRange: glyphs) {
            rect, _, _, range, _ in
            let at = lm.characterIndexForGlyph(at: range.location)
            let n = self.line(at: at)
            // A wrapped line's first piece alone.
            if self.starts[n] == at { number(n, rect) }
        }
        // The line after a last newline, or of an empty text.
        if lm.extraLineFragmentTextContainer != nil {
            number(starts.count - 1, lm.extraLineFragmentRect)
        }
    }
}
