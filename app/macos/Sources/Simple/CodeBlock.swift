import AppKit
import SwiftUI

/// CODE in the agent's chat (DESIGN §9b, §4t): a program the model wrote
/// -- render_program's `code`, or a block fenced in its words -- shown
/// as a chat shows code, a CODE BLOCK: a box of its own in the system's
/// monospace, its lines kept (scrolled across, never wrapped), its
/// language over it with a Copy button, JavaScript's comments, strings,
/// keywords and numbers tinted. In the steps' arguments it was one line
/// of JSON escapes. A long one shows its first lines, Show All the rest
/// in a box of its own that scrolls -- drawn in pieces of lines, no one
/// text tens of thousands of points tall (one such made the chat white,
/// 2026-11-05).
struct CodeBlockView: View {
    let code: String
    /// Its fence's language ("js", "javascript", ...); "" none said.
    let language: String
    /// Written whole: tinted. Still being written, plain -- tinted once
    /// its fence closes, not again at every piece.
    var closed = true

    @State private var open = false
    @State private var copied = false

    /// Lines in a piece; the first piece is what a long block shows
    /// folded.
    static let pieceLines = 16
    /// Folded past this many lines.
    static let foldPast = 24
    static let lineHeight = NSLayoutManager().defaultLineHeight(
        for: .monospacedSystemFont(ofSize: 12, weight: .regular))
    /// The tallest an opened block is: past it, it scrolls.
    static let openHeight: CGFloat = 440

    var body: some View {
        let pieces = CodeBlockCache.shared.pieces(code, language: language,
                                                  tinted: closed)
        let lines = CodeBlockCache.lineCount(code)
        let folds = lines > Self.foldPast
        VStack(alignment: .leading, spacing: 0) {
            header(lines)
            Divider().opacity(0.6)
            Group {
                if folds && open {
                    // Opened: its own box, scrolled down and across.
                    ScrollView([.horizontal, .vertical]) {
                        stack(pieces)
                    }
                    .frame(height: min(Self.openHeight,
                                       CGFloat(lines) * Self.lineHeight + 20))
                } else {
                    ScrollView(.horizontal) {
                        stack(folds ? Array(pieces.prefix(1)) : pieces)
                    }
                }
            }
            if folds {
                Divider().opacity(0.6)
                Button {
                    withAnimation(AppModel.motion) { open.toggle() }
                } label: {
                    Label(open ? String(localized: "Show Less")
                               : String(localized: "Show All"),
                          systemImage: open ? "chevron.up" : "chevron.down")
                        .font(.caption.weight(.medium))
                        .frame(maxWidth: .infinity)
                        .padding(.vertical, 5)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
            }
        }
        .background(Color.primary.opacity(0.045),
                    in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous)
            .strokeBorder(Color.primary.opacity(0.09), lineWidth: 0.5))
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func header(_ lines: Int) -> some View {
        HStack(spacing: 8) {
            Text(verbatim: Self.languageName(language))
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Text(lines == 1 ? String(localized: "1 line")
                            : String(localized: "\(lines) lines"))
                .font(.caption)
                .foregroundStyle(.tertiary)
            Spacer(minLength: 6)
            Button {
                let pb = NSPasteboard.general
                pb.clearContents()
                pb.setString(code, forType: .string)
                withAnimation(AppModel.motion) { copied = true }
                Task { @MainActor in
                    try? await Task.sleep(for: .seconds(1.5))
                    withAnimation(AppModel.motion) { copied = false }
                }
            } label: {
                Label(copied ? String(localized: "Copied")
                             : String(localized: "Copy"),
                      systemImage: copied ? "checkmark" : "doc.on.doc")
                    .font(.caption)
            }
            .buttonStyle(.borderless)
            .help("Copy the code")
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 5)
    }

    private func stack(_ pieces: [AttributedString]) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(pieces.indices, id: \.self) { k in
                Text(pieces[k])
                    .font(CodeHighlight.font)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: true, vertical: true)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
    }

    /// The language as a person reads it: "JavaScript" for a program.
    static func languageName(_ l: String) -> String {
        switch l.lowercased() {
        case "js", "javascript", "mjs", "jsx": "JavaScript"
        case "ts", "typescript", "tsx": "TypeScript"
        case "html": "HTML"
        case "css": "CSS"
        case "json": "JSON"
        case "glsl": "GLSL"
        case "wgsl": "WGSL"
        case "svg", "xml": l.uppercased()
        case "python", "py": "Python"
        case "": String(localized: "Code")
        default: l
        }
    }
}

/// A text's FENCED code blocks told from its words -- CommonMark's: a
/// line of three or more backticks (or tildes), up to three spaces in,
/// the language after it; closed by a line of the same character, as
/// many at least. One left open runs to the end: being written.
enum CodeFences {
    enum Segment: Equatable {
        case text(String)
        case code(language: String, body: String, closed: Bool)
    }

    static func split(_ s: String) -> [Segment] {
        guard s.contains("```") || s.contains("~~~") else {
            return [.text(s)]
        }
        var out: [Segment] = []
        var text: [Substring] = []
        var code: [Substring] = []
        var fence: Fence?
        func flush() {
            if !text.isEmpty {
                out.append(.text(text.joined(separator: "\n")))
                text = []
            }
        }
        for line in s.split(separator: "\n", omittingEmptySubsequences: false) {
            if let f = fence {
                if f.closes(line) {
                    out.append(.code(language: f.language,
                                     body: code.joined(separator: "\n"),
                                     closed: true))
                    code = []
                    fence = nil
                } else {
                    code.append(line)
                }
            } else if let f = Fence(opening: line) {
                flush()
                fence = f
            } else {
                text.append(line)
            }
        }
        if let f = fence {
            out.append(.code(language: f.language,
                             body: code.joined(separator: "\n"),
                             closed: false))
        }
        flush()
        return out
    }

    struct Fence {
        let mark: Character
        let count: Int
        let language: String

        /// A fence's opening line, else nil.
        init?(opening line: Substring) {
            let (mark, count, rest) = Self.run(line)
            guard count >= 3, let mark else { return nil }
            let info = rest.trimmingCharacters(in: .whitespaces)
            // A backtick fence's info has no backtick (inline code).
            if mark == "`" && info.contains("`") { return nil }
            self.mark = mark
            self.count = count
            language = String(info.split(separator: " ").first ?? "")
                .lowercased()
        }

        func closes(_ line: Substring) -> Bool {
            let (m, n, rest) = Self.run(line)
            return m == mark && n >= count
                && rest.allSatisfy { $0 == " " || $0 == "\t" }
        }

        /// Up to three spaces, then a run of ` or ~: its character, its
        /// length, what follows it.
        static func run(_ line: Substring)
            -> (Character?, Int, Substring) {
            var i = line.startIndex
            var spaces = 0
            while i < line.endIndex, line[i] == " ", spaces < 3 {
                i = line.index(after: i)
                spaces += 1
            }
            guard i < line.endIndex, line[i] == "`" || line[i] == "~" else {
                return (nil, 0, line[i...])
            }
            let c = line[i]
            var n = 0
            while i < line.endIndex, line[i] == c {
                i = line.index(after: i)
                n += 1
            }
            return (c, n, line[i...])
        }
    }
}

/// Code blocks drawn: each text's pieces, tinted, kept while it is the
/// same -- a chat draws again at every piece of a reply written.
@MainActor
final class CodeBlockCache {
    static let shared = CodeBlockCache()

    private var kept: [String: [AttributedString]] = [:]
    private var order: [String] = []

    func pieces(_ code: String, language: String, tinted: Bool)
        -> [AttributedString] {
        let key = "\(tinted ? language : "-")\u{0}\(code)"
        if let p = kept[key] { return p }
        let p = CodeHighlight.pieces(
            code, language: tinted ? language : "",
            lines: CodeBlockView.pieceLines)
        kept[key] = p
        order.append(key)
        if order.count > 48 {
            kept.removeValue(forKey: order.removeFirst())
        }
        return p
    }

    static func lineCount(_ code: String) -> Int {
        code.utf8.reduce(1) { $0 + ($1 == 10 ? 1 : 0) }
    }
}

/// JavaScript's tokens tinted -- comments, strings (template literals
/// whole), keywords, numbers -- as a light pass, not a parser: regular
/// expressions read as code. Other languages plain.
enum CodeHighlight {
    enum Kind { case plain, comment, string, keyword, number }

    static let font = Font.system(size: 12, design: .monospaced)

    static let tinted: Set<String> = [
        "js", "javascript", "mjs", "jsx", "ts", "typescript", "tsx",
        "json", "glsl", "wgsl",
    ]

    static let keywords: Set<String> = [
        "await", "async", "break", "case", "catch", "class", "const",
        "continue", "default", "delete", "do", "else", "export", "extends",
        "false", "finally", "for", "from", "function", "if", "import", "in",
        "instanceof", "let", "new", "null", "of", "return", "static",
        "super", "switch", "this", "throw", "true", "try", "typeof",
        "undefined", "var", "void", "while", "yield",
    ]

    /// The text in pieces of `lines` lines, each its tokens' runs.
    static func pieces(_ code: String, language: String, lines: Int)
        -> [AttributedString] {
        var out: [AttributedString] = []
        var piece = AttributedString()
        var count = 0
        func append(_ s: Substring, _ kind: Kind) {
            guard !s.isEmpty else { return }
            var a = AttributedString(s)
            switch kind {
            case .plain: break
            case .comment:
                a.foregroundColor = Color(nsColor: .secondaryLabelColor)
            case .string: a.foregroundColor = Color(nsColor: .systemRed)
            case .keyword:
                a.foregroundColor = Color(nsColor: .systemPink)
                a.font = font.weight(.semibold)
            case .number: a.foregroundColor = Color(nsColor: .systemBlue)
            }
            piece.append(a)
        }
        func add(_ s: Substring, _ kind: Kind) {
            // Cut at newlines: a piece ends at its last line's end, the
            // newline between two pieces in neither.
            var rest = s
            while let nl = rest.firstIndex(of: "\n") {
                append(rest[..<nl], kind)
                count += 1
                if count == lines {
                    out.append(piece)
                    piece = AttributedString()
                    count = 0
                } else {
                    append("\n", kind)
                }
                rest = rest[rest.index(after: nl)...]
            }
            append(rest, kind)
        }
        if tinted.contains(language.lowercased()) {
            for (range, kind) in tokens(code) {
                add(code[range], kind)
            }
        } else {
            add(code[...], .plain)
        }
        if !piece.characters.isEmpty || out.isEmpty { out.append(piece) }
        return out
    }

    /// The code's runs, in order, covering it whole.
    static func tokens(_ code: String) -> [(Range<String.Index>, Kind)] {
        var out: [(Range<String.Index>, Kind)] = []
        let s = code
        var i = s.startIndex
        var plain = i
        func emit(_ end: String.Index, _ kind: Kind, from: String.Index) {
            if plain < from { out.append((plain..<from, .plain)) }
            out.append((from..<end, kind))
            plain = end
        }
        func ident(_ c: Character) -> Bool {
            c.isLetter || c.isNumber || c == "_" || c == "$"
        }
        while i < s.endIndex {
            let c = s[i]
            let next = s.index(after: i)
            if c == "/", next < s.endIndex, s[next] == "/" {
                let end = s[i...].firstIndex(of: "\n") ?? s.endIndex
                emit(end, .comment, from: i)
                i = end
            } else if c == "/", next < s.endIndex, s[next] == "*" {
                let body = s.index(after: next)
                let end = s[body...].range(of: "*/")?.upperBound
                    ?? s.endIndex
                emit(end, .comment, from: i)
                i = end
            } else if c == "\"" || c == "'" || c == "`" {
                var j = next
                while j < s.endIndex, s[j] != c,
                      c == "`" || s[j] != "\n" {
                    if s[j] == "\\" { j = s.index(after: j) }
                    if j < s.endIndex { j = s.index(after: j) }
                }
                let end = j < s.endIndex && s[j] == c ? s.index(after: j) : j
                emit(end, .string, from: i)
                i = end
            } else if c.isNumber, c.isASCII,
                      i == s.startIndex || !ident(s[s.index(before: i)]) {
                var j = next
                while j < s.endIndex,
                      s[j].isHexDigit || s[j] == "." || s[j] == "x"
                        || s[j] == "_" || s[j] == "e" {
                    j = s.index(after: j)
                }
                emit(j, .number, from: i)
                i = j
            } else if c.isLetter || c == "_" || c == "$" {
                var j = next
                while j < s.endIndex, ident(s[j]) { j = s.index(after: j) }
                // A keyword tinted; an identifier stays in the plain run.
                if keywords.contains(String(s[i..<j])) {
                    emit(j, .keyword, from: i)
                }
                i = j
            } else {
                i = next
            }
        }
        if plain < s.endIndex { out.append((plain..<s.endIndex, .plain)) }
        return out
    }
}
