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
    /// Shut to its header until opened (a program in a tool call: the
    /// chat stays readable, the code a click away).
    var collapsed = false

    @State private var open = false
    @State private var copied = false
    /// Opened, where it starts collapsed.
    @State private var shown = false

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
        let showsCode = !collapsed || shown
        VStack(alignment: .leading, spacing: 0) {
            header(lines)
            if showsCode {
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
        }
        .background(Color.primary.opacity(0.045),
                    in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous)
            .strokeBorder(Color.primary.opacity(0.09), lineWidth: 0.5))
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func header(_ lines: Int) -> some View {
        HStack(spacing: 8) {
            if collapsed {
                Button {
                    withAnimation(AppModel.motion) { shown.toggle() }
                } label: {
                    Image(systemName: "chevron.right")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .rotationEffect(.degrees(shown ? 90 : 0))
                        .frame(width: 12)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help(shown ? String(localized: "Hide the code")
                            : String(localized: "Show the code"))
            }
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
        if let l = CodeSyntax.normal(language) {
            // The editor's tokens (CodeSyntax): every language it knows.
            for (range, kind) in CodeSyntax.runs(code, language: l) {
                switch kind {
                case .comment?: add(code[range], .comment)
                case .string?: add(code[range], .string)
                case .keyword?: add(code[range], .keyword)
                case .number?: add(code[range], .number)
                case nil: add(code[range], .plain)
                }
            }
        } else if tinted.contains(language.lowercased()) {
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


/// TOOL CALLS as a model WRITES them, told from its words (DESIGN §9b):
/// what the chat shows while a turn is written -- the decoded text, call
/// and all -- and what a turn keeps when its parser read none. Each
/// family writes its own form, so each is read:
///
/// - Qwen3.5 / 3.8 (Flash-Next, Bonsai): `<tool_call>` holding
///   `<function=NAME>` and a `<parameter=KEY>` each;
/// - Hermes JSON: `<tool_call>{"name": ..., "arguments": {...}}`;
/// - DeepSeek V4.1's DSML: `<｜DSML｜ calls>`, an `invoke name="..."`
///   each, `parameter name="..."` inside;
/// - LFM's `<|tool_call_start|>[name(key="value", ...)]<|tool_call_end|>`;
/// - a bare `<function=NAME>` with no `<tool_call>` round it.
///
/// One still being written (no close yet) is read as far as it goes,
/// `closed` false; the start of a tag at the very end is held back.
enum ToolCallText {
    struct Call: Equatable {
        var name: String
        var params: [(key: String, value: String)]
        var closed: Bool

        static func == (a: Call, b: Call) -> Bool {
            a.name == b.name && a.closed == b.closed
                && a.params.map(\.key) == b.params.map(\.key)
                && a.params.map(\.value) == b.params.map(\.value)
        }

        /// A parameter that holds code, and its language: a program's
        /// `code`, a page's `html`, ... or any value of several lines.
        static func language(_ key: String, _ value: String) -> String? {
            switch key.lowercased() {
            case "code", "program", "source", "script", "js",
                 "javascript": return "javascript"
            case "html": return "html"
            case "css": return "css"
            case "svg": return "svg"
            case "glsl", "shader": return "glsl"
            case "wgsl": return "wgsl"
            case "json": return "json"
            default: return value.contains("\n") ? "" : nil
            }
        }
    }

    enum Segment: Equatable {
        case text(String)
        case call(Call)
    }

    private struct Form: Sendable {
        let open: String
        let close: String
        let read: @Sendable (String, Bool) -> [Call]
    }

    private static let dsml = "\u{FF5C}DSML\u{FF5C}"
    private static let forms: [Form] = [
        Form(open: "<tool_call>", close: "</tool_call>") { body, closed in
            let t = body.trimmingCharacters(in: .whitespacesAndNewlines)
            return t.hasPrefix("{") ? json(t, closed) : xml(body, closed)
        },
        Form(open: "<" + dsml + " calls>", close: "</" + dsml + " calls>",
             read: { dsmlCalls($0, $1) }),
        Form(open: "<" + dsml + "calls>", close: "</" + dsml + "calls>",
             read: { dsmlCalls($0, $1) }),
        Form(open: "<" + dsml + "function_calls>",
             close: "</" + dsml + "function_calls>",
             read: { dsmlCalls($0, $1) }),
        Form(open: "<|tool_call_start|>", close: "<|tool_call_end|>",
             read: { pythonic($0, $1) }),
        Form(open: "<function=", close: "</function>") { body, closed in
            xml("<function=" + body, closed)
        },
    ]

    static func split(_ s: String) -> [Segment] {
        guard s.contains("<") else { return [.text(s)] }
        var out: [Segment] = []
        var rest = Substring(s)
        while true {
            // The first opener of any form.
            var best: (Form, Range<Substring.Index>)?
            for f in forms {
                if let r = rest.range(of: f.open),
                   best == nil || r.lowerBound < best!.1.lowerBound {
                    best = (f, r)
                }
            }
            guard let (form, r) = best else { break }
            let before = String(rest[..<r.lowerBound])
            if !before.trimmingCharacters(in: .whitespacesAndNewlines)
                .isEmpty {
                out.append(.text(before))
            }
            let after = rest[r.upperBound...]
            let closed: Bool
            let body: Substring
            if let c = after.range(of: form.close) {
                body = after[..<c.lowerBound]
                rest = after[c.upperBound...]
                closed = true
            } else {
                body = after
                rest = ""
                closed = false
            }
            let calls = form.read(String(body), closed)
            if calls.isEmpty {
                // Its name not written yet: a call all the same.
                out.append(.call(Call(name: "", params: [], closed: closed)))
            } else {
                out += calls.map { .call($0) }
            }
            if rest.isEmpty { return out }
        }
        // The start of a tag being written: held back until it is one.
        var tail = String(rest)
        if let lt = tail.lastIndex(of: "<") {
            let t = tail[lt...]
            let partial = forms.contains { f in
                f.open.hasPrefix(t) && t.count < f.open.count
            } || "<|tool_call_start|>".hasPrefix(t)
            if partial && !t.isEmpty { tail = String(tail[..<lt]) }
        }
        if !tail.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            || out.isEmpty {
            out.append(.text(tail))
        }
        return out
    }

    /// Qwen's: `<function=NAME>` then `<parameter=KEY>value</parameter>`.
    static func xml(_ body: String, _ closed: Bool) -> [Call] {
        var calls: [Call] = []
        var rest = Substring(body)
        while let f = rest.range(of: "<function=") {
            let tail = rest[f.upperBound...]
            let gt = tail.firstIndex(of: ">")
            // Named once its `>` is written, not a few letters of it.
            let name = gt.map {
                String(tail[..<$0])
                    .trimmingCharacters(in: .whitespacesAndNewlines)
            } ?? ""
            var inside = gt.map { tail[tail.index(after: $0)...] } ?? ""
            var whole = gt != nil && closed
            if let end = inside.range(of: "</function>") {
                rest = inside[end.upperBound...]
                inside = inside[..<end.lowerBound]
                whole = true
            } else {
                rest = ""
            }
            var params: [(key: String, value: String)] = []
            var ps = inside
            while let p = ps.range(of: "<parameter=") {
                let pt = ps[p.upperBound...]
                guard let pgt = pt.firstIndex(of: ">") else { break }
                let key = String(pt[..<pgt])
                var v = pt[pt.index(after: pgt)...]
                if let e = v.range(of: "</parameter>") {
                    ps = v[e.upperBound...]
                    v = v[..<e.lowerBound]
                } else if let n = v.range(of: "<parameter=") {
                    ps = v[n.lowerBound...]
                    v = v[..<n.lowerBound]
                } else {
                    ps = ""
                }
                params.append((key, trimmedValue(v)))
            }
            calls.append(Call(name: name, params: params, closed: whole))
        }
        return calls
    }

    /// Hermes: `{"name": ..., "arguments": {...}}` -- read whole, else
    /// (being written) its name and its string values so far.
    static func json(_ body: String, _ closed: Bool) -> [Call] {
        if let d = body.data(using: .utf8),
           let o = try? JSONSerialization.jsonObject(with: d)
               as? [String: Any] {
            let name = o["name"] as? String ?? ""
            var args = o["arguments"] ?? o["parameters"]
            if let s = args as? String, let d = s.data(using: .utf8) {
                args = try? JSONSerialization.jsonObject(with: d)
            }
            var params: [(key: String, value: String)] = []
            for (k, v) in (args as? [String: Any] ?? [:])
                .sorted(by: { $0.key < $1.key }) {
                params.append((k, describe(v)))
            }
            return [Call(name: name, params: params, closed: closed)]
        }
        let name = firstString(after: "\"name\"", in: body)?.0 ?? ""
        var params: [(key: String, value: String)] = []
        if let a = body.range(of: "\"arguments\"") {
            var rest = body[a.upperBound...]
            while let (key, k) = firstString(after: "", in: rest) {
                rest = rest[k...]
                let colon = rest.drop { $0 == " " || $0 == "\n" }
                guard colon.first == ":" else { continue }
                let v = colon.dropFirst().drop { $0 == " " || $0 == "\n" }
                guard v.first == "\"" else { rest = v; continue }
                let (value, end) = unescape(v.dropFirst())
                params.append((key, value))
                rest = end
            }
        }
        return [Call(name: name, params: params, closed: closed)]
    }

    /// DeepSeek's: an `invoke name="X"` each, `parameter name="K"`
    /// inside (the space after the marker optional).
    static func dsmlCalls(_ body: String, _ closed: Bool) -> [Call] {
        let b = body.replacingOccurrences(of: dsml + " ", with: dsml)
        var calls: [Call] = []
        let invokes = b.components(separatedBy: "<" + dsml + "invoke")
        for inv in invokes.dropFirst() {
            let name = attribute("name", in: inv) ?? ""
            var params: [(key: String, value: String)] = []
            let ps = inv.components(separatedBy: "<" + dsml + "parameter")
            for p in ps.dropFirst() {
                guard let gt = p.firstIndex(of: ">") else { continue }
                let key = attribute("name", in: String(p[..<gt])) ?? ""
                var v = p[p.index(after: gt)...]
                if let e = v.range(of: "</" + dsml + "parameter") {
                    v = v[..<e.lowerBound]
                }
                params.append((key, trimmedValue(v)))
            }
            let whole = closed
                || inv.contains("</" + dsml + "invoke>")
            calls.append(Call(name: name, params: params, closed: whole))
        }
        return calls
    }

    /// LFM's: `[name(key="value", key2=3), other(...)]`.
    static func pythonic(_ body: String, _ closed: Bool) -> [Call] {
        var calls: [Call] = []
        var s = Substring(body.trimmingCharacters(in: .whitespacesAndNewlines))
        if s.first == "[" { s = s.dropFirst() }
        while let paren = s.firstIndex(of: "(") {
            let name = s[..<paren].trimmingCharacters(
                in: CharacterSet(charactersIn: " ,\n[]"))
            var rest = s[s.index(after: paren)...]
            var params: [(key: String, value: String)] = []
            var whole = false
            while true {
                rest = rest.drop { $0 == " " || $0 == "," || $0 == "\n" }
                if rest.first == ")" {
                    rest = rest.dropFirst()
                    whole = true
                    break
                }
                guard let eq = rest.firstIndex(of: "=") else {
                    rest = ""
                    break
                }
                let key = rest[..<eq].trimmingCharacters(in: .whitespaces)
                var v = rest[rest.index(after: eq)...]
                    .drop { $0 == " " }
                if let q = v.first, q == "\"" || q == "'" {
                    let (value, end) = unescape(v.dropFirst(), quote: q)
                    params.append((key, value))
                    rest = end
                } else {
                    let stop = v.firstIndex { $0 == "," || $0 == ")" }
                        ?? v.endIndex
                    params.append((key, String(v[..<stop])))
                    v = v[stop...]
                    rest = v
                }
            }
            calls.append(Call(name: name, params: params,
                              closed: whole && closed))
            s = rest
        }
        return calls
    }

    // MARK: helpers

    private static func trimmedValue(_ v: Substring) -> String {
        var t = v
        if t.first == "\n" { t = t.dropFirst() }
        if t.last == "\n" { t = t.dropLast() }
        return String(t)
    }

    private static func describe(_ v: Any) -> String {
        if let s = v as? String { return s }
        if JSONSerialization.isValidJSONObject(v),
           let d = try? JSONSerialization.data(
               withJSONObject: v, options: [.prettyPrinted, .sortedKeys]),
           let s = String(data: d, encoding: .utf8) {
            return s
        }
        return "\(v)"
    }

    private static func attribute(_ name: String, in s: String) -> String? {
        guard let r = s.range(of: name + "=\"") else { return nil }
        let rest = s[r.upperBound...]
        return String(rest[..<(rest.firstIndex(of: "\"") ?? rest.endIndex)])
    }

    /// The first JSON string after `marker` (and a colon, when there is
    /// a marker): its value and where it ends.
    private static func firstString<S: StringProtocol>(
        after marker: String, in s: S) -> (String, S.Index)?
        where S.SubSequence == Substring {
        var rest = s[...]
        if !marker.isEmpty {
            guard let m = rest.range(of: marker) else { return nil }
            rest = rest[m.upperBound...]
            rest = rest.drop { $0 == " " || $0 == ":" || $0 == "\n" }
        }
        guard let q = rest.firstIndex(of: "\"") else { return nil }
        let (v, end) = unescape(rest[rest.index(after: q)...])
        guard end.startIndex > q else { return nil }
        return (v, end.startIndex)
    }

    /// A quoted string's body up to its closing quote (or the end, being
    /// written), escapes read; what follows it.
    private static func unescape(_ s: Substring, quote: Character = "\"")
        -> (String, Substring) {
        var out = ""
        var i = s.startIndex
        while i < s.endIndex {
            let c = s[i]
            if c == quote { return (out, s[s.index(after: i)...]) }
            if c == "\\" {
                let n = s.index(after: i)
                guard n < s.endIndex else { break }
                switch s[n] {
                case "n": out.append("\n")
                case "t": out.append("\t")
                case "r": out.append("\r")
                case "u":
                    let h = s[s.index(after: n)...].prefix(4)
                    if h.count == 4, let v = UInt32(h, radix: 16),
                       let u = Unicode.Scalar(v) {
                        out.unicodeScalars.append(u)
                        i = s.index(n, offsetBy: 5)
                        continue
                    }
                    out.append("u")
                default: out.append(s[n])
                }
                i = s.index(after: n)
                continue
            }
            out.append(c)
            i = s.index(after: i)
        }
        return (out, s[s.endIndex...])
    }
}

/// A tool call written in the agent's words, as the chat shows it: a box
/// naming the tool, SHUT until opened -- then its parameters, code as a
/// code block, the rest a line each.
struct ToolCallBlockView: View {
    let call: ToolCallText.Call
    @State private var open = false

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button {
                withAnimation(AppModel.motion) { open.toggle() }
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: "chevron.right")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .rotationEffect(.degrees(open ? 90 : 0))
                        .frame(width: 12)
                    Image(systemName: "curlybraces")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Text(verbatim: call.name.isEmpty
                         ? String(localized: "Tool call")
                         : AppModel.agentToolTitle(call.name))
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                    if let lines = codeLines {
                        Text(lines == 1 ? String(localized: "1 line")
                                        : String(localized: "\(lines) lines"))
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                    if !call.closed {
                        ProgressView().controlSize(.mini)
                    }
                    Spacer(minLength: 6)
                }
                .padding(.horizontal, 10)
                .padding(.vertical, 6)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .help(open ? String(localized: "Hide the call")
                       : String(localized: "Show the call"))
            if open {
                Divider().opacity(0.6)
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(call.params.indices, id: \.self) { i in
                        let p = call.params[i]
                        if let lang = ToolCallText.Call.language(p.key,
                                                                 p.value) {
                            Text(verbatim: p.key)
                                .font(.caption2.weight(.semibold))
                                .foregroundStyle(.tertiary)
                            CodeBlockView(code: p.value, language: lang,
                                          closed: call.closed)
                        } else {
                            Text(verbatim: "\(p.key): \(p.value)")
                                .font(CodeHighlight.font)
                                .foregroundStyle(.secondary)
                                .lineLimit(4)
                                .truncationMode(.tail)
                                .textSelection(.enabled)
                        }
                    }
                }
                .padding(10)
            }
        }
        .background(Color.primary.opacity(0.045),
                    in: RoundedRectangle(cornerRadius: 8, style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 8, style: .continuous)
            .strokeBorder(Color.primary.opacity(0.09), lineWidth: 0.5))
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    /// The lines of the code it carries, when it carries code.
    private var codeLines: Int? {
        let code = call.params.filter {
            ToolCallText.Call.language($0.key, $0.value) != nil
        }
        guard !code.isEmpty else { return nil }
        return code.reduce(0) { $0 + CodeBlockCache.lineCount($1.value) }
    }
}
