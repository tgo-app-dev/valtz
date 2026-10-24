import AppKit
import SwiftUI

/// HOLD ⌘? (DESIGN §10a): every button on screen that has a key shows it,
/// in a small floating label beside it, while ⌘? is held -- let go (the
/// key, or ⌘), they go. Help › Keyboard Shortcuts shows them until the
/// next key or click.
///
/// A button says its key with `.shortcutHint("⇧⌘E")`: an empty AppKit
/// view behind it (`ShortcutProbeView`) that knows where it lies in its
/// window, whichever hosting view holds it -- the title bar's toolbar
/// items too, out of reach of the content's SwiftUI. The labels are drawn
/// in a window of their own over each window showing any
/// (click-through, borderless, a child window), so nothing clips them: a
/// card's rounded edge, the title bar. A container laid out but not seen
/// -- a shut drawer -- says so (`shortcutHintsHidden`), and its buttons'
/// labels stay away; a disabled button's is dimmed.
@MainActor
final class ShortcutHints {
    static let shared = ShortcutHints()

    private let probes = NSHashTable<ShortcutProbeView>.weakObjects()
    /// The label windows shown, each over its own window.
    private var layers: [ShortcutHintWindow] = []
    /// The key that showed them, and the modifiers it was held with: one
    /// of those let go hides them. (AppKit hands no key-up over while ⌘
    /// is held: the modifiers are what tell.)
    private var heldKey: UInt16?
    private var heldMods: NSEvent.ModifierFlags = []
    /// Shown from the menu: until the next key or click.
    private var latched = false
    private var resign: Any?

    var shown: Bool { !layers.isEmpty }

    private init() {
        resign = NotificationCenter.default.addObserver(
            forName: NSApplication.didResignActiveNotification,
            object: nil, queue: .main
        ) { _ in
            MainActor.assumeIsolated { ShortcutHints.shared.hide() }
        }
    }

    func register(_ p: ShortcutProbeView) { probes.add(p) }

    /// ⌘? -- by its character, or on a US keyboard's place, ⇧⌘/ -- with
    /// no ⌃ or ⌥.
    static func isHelpKey(_ e: NSEvent) -> Bool {
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
            .subtracting([.capsLock, .numericPad, .function])
        guard mods.contains(.command),
              mods.isDisjoint(with: [.control, .option]) else { return false }
        return e.characters == "?"
            || (mods.contains(.shift) && e.keyCode == 44)
    }

    /// The window's key monitors': ⌘? down shows the labels (taken: the
    /// Help menu's own ⌘? never sees it), its key or ⌘ let go hides them;
    /// shown from the menu, any key or click does. True: the event taken.
    func handle(_ e: NSEvent) -> Bool {
        switch e.type {
        case .keyDown:
            if Self.isHelpKey(e) {
                if !e.isARepeat && !shown {
                    show()
                    heldKey = e.keyCode
                    heldMods = e.modifierFlags.intersection([.command,
                                                             .shift])
                }
                return true
            }
            if latched { hide() }
            return false
        case .keyUp:
            if shown && !latched && e.keyCode == heldKey { hide() }
            return false
        case .flagsChanged:
            if shown && !latched
                && !e.modifierFlags.isSuperset(of: heldMods) {
                hide()
            }
            return false
        case .leftMouseDown, .rightMouseDown, .otherMouseDown:
            if latched { hide() }
            return false
        default:
            return false
        }
    }

    /// From the menu: shown until the next key or click.
    func showLatched() {
        hide()
        show()
        latched = shown
    }

    /// Each window's labels over it, at once -- its probes as they lie now.
    func show() {
        hide()
        var byWindow: [ObjectIdentifier: (NSWindow, [Hint])] = [:]
        for p in probes.allObjects {
            guard p.active, !p.text.isEmpty, let w = p.window,
                  w.isVisible, !(w is ShortcutHintWindow),
                  !p.isHiddenOrHasHiddenAncestor else { continue }
            let r = p.convert(p.bounds, to: nil)
            let frame = CGRect(origin: .zero, size: w.frame.size)
            guard r.width > 0, r.height > 0, frame.intersects(r) else {
                continue
            }
            // Top-left origin, as the labels' layer lays them out.
            let top = CGRect(x: r.minX, y: frame.height - r.maxY,
                             width: r.width, height: r.height)
            let k = ObjectIdentifier(w)
            var entry = byWindow[k] ?? (w, [])
            // One label for a key over one place (a button drawn twice).
            if !entry.1.contains(where: {
                $0.text == p.text && $0.anchor.intersects(top) }) {
                entry.1.append(Hint(text: p.text, anchor: top,
                                    enabled: p.enabled))
            }
            byWindow[k] = entry
        }
        for (w, hints) in byWindow.values where !hints.isEmpty {
            let layer = ShortcutHintWindow(over: w, hints: hints)
            w.addChildWindow(layer, ordered: .above)
            layer.alphaValue = 0
            layer.orderFront(nil)
            NSAnimationContext.runAnimationGroup { c in
                c.duration = 0.12
                layer.animator().alphaValue = 1
            }
            layers.append(layer)
        }
    }

    func hide() {
        latched = false
        heldKey = nil
        heldMods = [.command]
        for l in layers {
            l.parent?.removeChildWindow(l)
            l.orderOut(nil)
        }
        layers = []
    }

    /// Where the button showing `text` lies in its window, top-left
    /// origin -- seen, shown or not (the snapshot hooks click it).
    func place(of text: String) -> CGRect? {
        for p in probes.allObjects {
            guard p.active, p.text == text, let w = p.window, w.isVisible,
                  !p.isHiddenOrHasHiddenAncestor else { continue }
            let r = p.convert(p.bounds, to: nil)
            guard r.width > 0, r.height > 0 else { continue }
            return CGRect(x: r.minX, y: w.frame.height - r.maxY,
                          width: r.width, height: r.height)
        }
        return nil
    }

    /// What is shown, for the snapshot hooks: "⇧⌘E@612,410" each, by
    /// window (its number).
    var summary: String {
        layers.map { l in
            "\(l.parent?.windowNumber ?? 0):" + l.hints.map {
                "\($0.text)@\(Int($0.anchor.midX)),\(Int($0.anchor.midY))"
                    + ($0.enabled ? "" : "-off")
            }.joined(separator: ",")
        }.joined(separator: " ")
    }

    struct Hint: Identifiable {
        let id = UUID()
        let text: String
        /// The button's place in its window, top-left origin.
        let anchor: CGRect
        let enabled: Bool
    }
}

/// The labels' window: borderless, clear, click-through, over its window
/// (a child window, so it moves and orders with it), never key.
final class ShortcutHintWindow: NSWindow {
    let hints: [ShortcutHints.Hint]

    init(over w: NSWindow, hints: [ShortcutHints.Hint]) {
        self.hints = hints
        super.init(contentRect: w.frame, styleMask: [.borderless],
                   backing: .buffered, defer: false)
        isReleasedWhenClosed = false
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        ignoresMouseEvents = true
        collectionBehavior = [.transient, .ignoresCycle]
        let host = NSHostingView(rootView: ShortcutHintLayer(hints: hints))
        host.sizingOptions = []
        host.frame = CGRect(origin: .zero, size: w.frame.size)
        contentView = host
        appearance = w.effectiveAppearance
    }

    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

/// Each label at its button: under it, centred, overlapping its foot a
/// little -- above it where the window ends -- kept inside the window,
/// and labels of buttons side by side pushed apart evenly until none
/// covers another.
private struct ShortcutHintLayer: View {
    let hints: [ShortcutHints.Hint]

    var body: some View {
        HintPlacement(anchors: hints.map(\.anchor)) {
            ForEach(hints) { h in
                Text(verbatim: h.text)
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.white)
                    .padding(.horizontal, 5)
                    .padding(.vertical, 2)
                    .background(RoundedRectangle(cornerRadius: 5,
                                                 style: .continuous)
                        .fill(Color.black.opacity(0.78)))
                    .overlay(RoundedRectangle(cornerRadius: 5,
                                              style: .continuous)
                        .strokeBorder(Color.white.opacity(0.35),
                                      lineWidth: 0.5))
                    .shadow(color: .black.opacity(0.25), radius: 2, y: 1)
                    .opacity(h.enabled ? 1 : 0.45)
                    .fixedSize()
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .allowsHitTesting(false)
    }
}

private struct HintPlacement: Layout {
    let anchors: [CGRect]

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews,
                      cache: inout ()) -> CGSize {
        proposal.replacingUnspecifiedDimensions()
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize,
                       subviews: Subviews, cache: inout ()) {
        let n = min(subviews.count, anchors.count)
        let rects = (0..<n).map {
            anchors[$0].offsetBy(dx: bounds.minX, dy: bounds.minY)
        }
        let rows = Self.rows(rects)
        var frames: [CGRect] = (0..<n).map { i in
            let a = rects[i], row = rows[i]
            let size = subviews[i].sizeThatFits(.unspecified)
            var y = row.maxY - 4
            if y + size.height > bounds.maxY - 4 {
                y = row.minY - size.height + 4
            }
            return CGRect(x: a.midX - size.width / 2, y: y,
                          width: size.width, height: size.height)
        }
        let gap: CGFloat = 2
        for _ in 0..<8 {
            var moved = false
            for i in 0..<n {
                for j in (i + 1)..<max(i + 1, n)
                    where frames[i].insetBy(dx: -gap / 2, dy: 0)
                        .intersects(frames[j].insetBy(dx: -gap / 2,
                                                      dy: 0)) {
                    let (l, r) = frames[i].midX <= frames[j].midX
                        ? (i, j) : (j, i)
                    let o = frames[l].maxX + gap - frames[r].minX
                    frames[l].origin.x -= o / 2
                    frames[r].origin.x += o / 2
                    moved = true
                }
            }
            if !moved { break }
        }
        for i in 0..<n {
            let f = frames[i]
            let x = min(max(f.minX, bounds.minX + 4),
                        bounds.maxX - f.width - 4)
            subviews[i].place(at: CGPoint(x: x, y: f.minY),
                              anchor: .topLeading, proposal: .unspecified)
        }
    }

    /// Each button's ROW, as its label goes under it: the buttons centred
    /// alike and side by side, its neighbours' and theirs, one span from
    /// the highest top to the lowest foot -- their labels on one line.
    /// A menu's frame is not a button's of the same glyph: the prompt
    /// box's paperclip label hung lower than the Prompt Editor button's
    /// beside it.
    private static func rows(_ rects: [CGRect]) -> [CGRect] {
        func sideBySide(_ a: CGRect, _ b: CGRect) -> Bool {
            abs(a.midY - b.midY) <= 3
                && max(a.minX, b.minX) - min(a.maxX, b.maxX) <= 16
        }
        var rows = rects
        for _ in rects.indices {
            var grew = false
            for i in rects.indices {
                for j in rects.indices
                    where j != i && sideBySide(rects[i], rects[j]) {
                    let top = min(rows[i].minY, rows[j].minY)
                    let foot = max(rows[i].maxY, rows[j].maxY)
                    if top < rows[i].minY || foot > rows[i].maxY {
                        rows[i] = CGRect(x: rows[i].minX, y: top,
                                         width: rows[i].width,
                                         height: foot - top)
                        grew = true
                    }
                }
            }
            if !grew { break }
        }
        return rows
    }
}

/// An empty view behind a button: its key, and where the button is.
final class ShortcutProbeView: NSView {
    var text = ""
    /// Seen: not in a container laid out but hidden (a shut drawer).
    var active = true
    var enabled = true

    override func hitTest(_ point: NSPoint) -> NSView? { nil }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { ShortcutHints.shared.register(self) }
    }
}

private struct ShortcutProbe: NSViewRepresentable {
    let text: String
    let active: Bool
    let enabled: Bool

    func makeNSView(context: Context) -> ShortcutProbeView {
        ShortcutProbeView()
    }

    func updateNSView(_ v: ShortcutProbeView, context: Context) {
        v.text = text
        v.active = active
        v.enabled = enabled
    }
}

extension EnvironmentValues {
    /// Laid out but not seen (a shut drawer): no key labels from here.
    @Entry var shortcutHintsHidden = false
}

extension View {
    /// The key that does what this button does, shown beside it while ⌘?
    /// is held ("⇧⌘E", "Space", "V").
    func shortcutHint(_ text: String) -> some View {
        modifier(ShortcutHintModifier(text: text))
    }
}

private struct ShortcutHintModifier: ViewModifier {
    let text: String
    @Environment(\.shortcutHintsHidden) private var hidden
    @Environment(\.isEnabled) private var enabled

    func body(content: Content) -> some View {
        content.background(ShortcutProbe(text: text, active: !hidden,
                                         enabled: enabled))
    }
}
