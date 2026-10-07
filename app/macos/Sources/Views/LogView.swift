import AppKit
import SwiftUI

/// The Log view's rows: what the engine (vpipe) and Valtz report, read
/// from the core's log (Controller::log_since) while the view is on
/// screen, the newest 16 384 kept -- a log that runs on does not grow
/// without end, nor slow the window.
@MainActor @Observable
final class LogState {
    static let capacity = 16_384

    private(set) var rows: [LogRow] = []
    /// The text's size, in points (11 by default), kept between launches
    /// -- except by a scripted snapshot run.
    var fontSize: Double = LogState.storedFontSize {
        didSet {
            if Self.keeps {
                UserDefaults.standard.set(fontSize, forKey: Self.fontKey)
            }
        }
    }
    @ObservationIgnored private var next: UInt64 = 0
    @ObservationIgnored private var pulling = false

    static let fontRange: ClosedRange<Double> = 8...24
    private static let fontKey = "log.fontSize"
    private static var keeps: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
    }
    private static var storedFontSize: Double {
        let v = keeps ? UserDefaults.standard.double(forKey: fontKey) : 0
        return v > 0 ? min(fontRange.upperBound, max(fontRange.lowerBound, v))
                     : 11
    }

    /// The rows the core has after the newest one here.
    func pull(_ core: CoreService?) async {
        guard let core, !pulling else { return }
        pulling = true
        defer { pulling = false }
        let after = next
        let page: (rows: [LogRow], next: UInt64)? = await Task.detached(
            priority: .utility) {
            struct Page: Decodable { var rows: [LogRow]; var next: UInt64 }
            guard let p = DTO.decode(Page.self, core.logSinceJSON(after))
            else { return nil }
            return (p.rows, p.next)
        }.value
        guard let page, !page.rows.isEmpty else { return }
        next = page.next
        rows.append(contentsOf: page.rows)
        if rows.count > Self.capacity {
            rows.removeFirst(rows.count - Self.capacity)
        }
    }

    /// Every row gone, here and in the core.
    func clear(_ core: CoreService?) {
        core?.clearLog()
        rows = []
    }

    func zoom(by delta: Double) {
        fontSize = min(Self.fontRange.upperBound,
                       max(Self.fontRange.lowerBound, fontSize + delta))
    }
}

/// The Log screen: a bar -- how many rows, smaller and larger text,
/// clear -- over the rows, selectable and copyable, following the newest
/// while it is scrolled to the end.
struct LogView: View {
    @Bindable var model: AppModel

    private var log: LogState { model.logState }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text("Log").font(.headline)
                Text(verbatim: log.rows.count.formatted())
                    .monospacedDigit()
                    .foregroundStyle(.secondary)
                    .help("Rows kept: the newest 16,384")
                Spacer()
                Button {
                    log.zoom(by: -1)
                } label: { Image(systemName: "textformat.size.smaller") }
                    .help("Smaller text")
                    .disabled(log.fontSize <= LogState.fontRange.lowerBound)
                Button {
                    log.zoom(by: 1)
                } label: { Image(systemName: "textformat.size.larger") }
                    .help("Larger text")
                    .disabled(log.fontSize >= LogState.fontRange.upperBound)
                Button("Clear") { log.clear(model.core) }
                    .help("Clear the log")
            }
            .buttonStyle(.borderless)
            .controlSize(.small)
            .padding(.horizontal, 14)
            .frame(height: 34)
            .background(.bar)
            .overlay(alignment: .bottom) { Divider() }
            LogTextView(rows: log.rows, fontSize: log.fontSize)
        }
        // Read only while it is on screen.
        .task {
            while !Task.isCancelled {
                await log.pull(model.core)
                try? await Task.sleep(for: .milliseconds(300))
            }
        }
    }
}

/// The rows in a text view: new ones appended, ones that left the ring
/// taken off the top -- never the whole text redrawn for a new line.
private struct LogTextView: NSViewRepresentable {
    let rows: [LogRow]
    let fontSize: Double

    final class Coordinator {
        /// What the text holds: each row's seq and length, in order, from
        /// `head` on.
        var shown: [(seq: UInt64, length: Int)] = []
        var head = 0
        var fontSize: Double = 0
        let time: DateFormatter = {
            let f = DateFormatter()
            f.dateFormat = "HH:mm:ss.SSS"
            return f
        }()
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSScrollView {
        let scroll = NSTextView.scrollableTextView()
        if let text = scroll.documentView as? NSTextView {
            text.isEditable = false
            text.isSelectable = true
            text.isRichText = true
            text.drawsBackground = true
            text.backgroundColor = .textBackgroundColor
            text.textContainerInset = NSSize(width: 8, height: 6)
            text.layoutManager?.allowsNonContiguousLayout = true
        }
        scroll.hasVerticalScroller = true
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        guard let text = scroll.documentView as? NSTextView,
              let storage = text.textStorage else { return }
        let c = context.coordinator
        let atEnd = scroll.documentVisibleRect.maxY
            >= text.bounds.maxY - 24
        storage.beginEditing()
        if rows.isEmpty || fontSize != c.fontSize {
            storage.setAttributedString(NSAttributedString())
            c.shown = []
            c.head = 0
            c.fontSize = fontSize
        }
        // Rows that left the ring: off the top.
        if let first = rows.first?.seq {
            var cut = 0
            while c.head < c.shown.count && c.shown[c.head].seq < first {
                cut += c.shown[c.head].length
                c.head += 1
            }
            if cut > 0 {
                storage.deleteCharacters(in: NSRange(location: 0,
                                                     length: cut))
            }
            if c.head > 4096 {
                c.shown.removeFirst(c.head)
                c.head = 0
            }
        }
        // New ones: at the end.
        let last = c.shown.last?.seq ?? 0
        let fresh = rows.drop { $0.seq <= last }
        if !fresh.isEmpty {
            let out = NSMutableAttributedString()
            for r in fresh {
                let line = attributed(r, c)
                c.shown.append((r.seq, line.length))
                out.append(line)
            }
            storage.append(out)
        }
        storage.endEditing()
        if atEnd && !fresh.isEmpty {
            text.scrollToEndOfDocument(nil)
        }
    }

    /// "12:04:31.207  vpipe  INFO   text", the level and an error's or a
    /// warning's text in colour.
    private func attributed(_ r: LogRow, _ c: Coordinator) -> NSAttributedString {
        let font = NSFont.monospacedSystemFont(ofSize: fontSize,
                                               weight: .regular)
        let dim: [NSAttributedString.Key: Any] = [
            .font: font, .foregroundColor: NSColor.secondaryLabelColor,
        ]
        let tint: NSColor = switch r.level {
        case "error": .systemRed
        case "warn": .systemOrange
        case "debug": .tertiaryLabelColor
        default: .labelColor
        }
        let out = NSMutableAttributedString()
        let when = Date(timeIntervalSince1970: Double(r.time) / 1000)
        out.append(NSAttributedString(
            string: c.time.string(from: when) + "  "
                + r.source.padding(toLength: 5, withPad: " ",
                                   startingAt: 0) + "  ",
            attributes: dim))
        out.append(NSAttributedString(
            string: r.level.uppercased().padding(toLength: 5,
                                                 withPad: " ",
                                                 startingAt: 0) + "  ",
            attributes: [.font: font, .foregroundColor:
                            r.level == "info" ? .secondaryLabelColor : tint]))
        out.append(NSAttributedString(
            string: r.text + "\n",
            attributes: [.font: font, .foregroundColor:
                            r.level == "error" || r.level == "warn"
                                ? tint : NSColor.labelColor]))
        return out
    }
}
