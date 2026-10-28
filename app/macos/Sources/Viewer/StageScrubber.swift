import AppKit
import SwiftUI

/// The stage's SCRUBBER, a slim row over its transport (StageTransport):
/// where the player is, the time on its left and the length on its right
/// in the stage's own units (a clip's timecode, a sound's milliseconds).
/// A press goes there, a drag scrubs -- the player paused under it and
/// playing on as it was when let go. Hidden with the transport while the
/// timeline, whose ruler scrubs, is open.
struct StageScrubber: View {
    @Bindable var model: AppModel

    var body: some View {
        ScrubberRow(frame: model.videoFrame,
                    length: model.stageClipLength ?? 0,
                    units: model.stageFrameRate,
                    seek: { model.seekVideo(to: $0) },
                    letGo: {
                        model.noteBar("scrubber: let go at \($0)")
                        model.scrubbed(to: $0, resume: $1)
                    },
                    rate: { model.videoRate },
                    step: { model.stepVideo($0) })
            // A voice-over's take under way: not dragged while it plays.
            .disabled(model.voiceOverRecording)
    }
}

/// The scrubber's row, as the stage and the Atlas's player show it: the
/// time where it is, the track, the length -- in `units` (a clip's
/// timecode, a sound's milliseconds).
struct ScrubberRow: View {
    let frame: Int
    let length: Int
    let units: FrameRate
    let seek: (Int) -> Void
    /// Let go at a frame, playing on at the rate the press found.
    let letGo: (Int, Float) -> Void
    let rate: () -> Float
    let step: (Int) -> Void

    var body: some View {
        let at = min(max(0, frame), max(0, length - 1))
        HStack(spacing: 8) {
            Text(verbatim: units.timecode(at))
            ScrubberBar(frame: at, length: length,
                        value: units.timecode(at),
                        seek: seek, letGo: letGo, rate: rate, step: step)
            Text(verbatim: units.timecode(length))
                .foregroundStyle(.secondary)
        }
        .font(.caption.monospacedDigit())
        .frame(height: 18)
        .disabled(length < 2)
    }
}

/// The scrubber's track, an AppKit view: its own mouse handling, so the
/// snapshot hooks' posted drags reach it (a SwiftUI gesture never sees
/// them).
private struct ScrubberBar: NSViewRepresentable {
    let frame: Int
    let length: Int
    /// Where it is, for accessibility.
    let value: String
    let seek: (Int) -> Void
    /// Let go at a frame, playing on at the rate the press found.
    let letGo: (Int, Float) -> Void
    let rate: () -> Float
    let step: (Int) -> Void

    @MainActor
    final class Coordinator {
        /// The rate the player had when the press came: played on at it
        /// when the press ends.
        var resume: Float = 0
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> ScrubberNSView {
        ScrubberNSView(frame: .zero)
    }

    func updateNSView(_ v: ScrubberNSView, context: Context) {
        let c = context.coordinator
        let last = max(1, length - 1)
        v.enabled = context.environment.isEnabled
        v.setAccessibilityValue(value)
        v.show(CGFloat(frame) / CGFloat(last))
        v.onStep = step
        v.onScrub = { phase, f in
            let n = Int((f * CGFloat(last)).rounded())
            switch phase {
            case .began:
                c.resume = rate()
                seek(n)
            case .moved:
                seek(n)
            case .ended:
                letGo(n, c.resume)
            }
        }
    }
}

@MainActor
final class ScrubberNSView: NSView {
    enum Phase { case began, moved, ended }

    /// 0...1 along the track (a press's, while it lasts).
    private var fraction: CGFloat = 0
    private var pressed = false
    var enabled = true {
        didSet { if enabled != oldValue { needsDisplay = true } }
    }
    var onScrub: ((Phase, CGFloat) -> Void)?
    var onStep: ((Int) -> Void)?

    /// The view on show, for the snapshot hooks.
    static weak var shown: ScrubberNSView?

    private let knob: CGFloat = 9
    private let track: CGFloat = 3

    override init(frame: NSRect) {
        super.init(frame: frame)
        setAccessibilityElement(true)
        setAccessibilityRole(.slider)
        setAccessibilityLabel(String(localized: "Position"))
        toolTip = String(localized: "Where the player is: click or drag to go there")
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override var isFlipped: Bool { true }
    override var acceptsFirstResponder: Bool { false }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { Self.shown = self }
    }

    /// Where the player is, unless a press holds the knob.
    func show(_ f: CGFloat) {
        let f = min(1, max(0, f.isFinite ? f : 0))
        guard !pressed, f != fraction else { return }
        fraction = f
        needsDisplay = true
    }

    /// The knob's centre runs from one end of the track to the other,
    /// inset by its radius so it is never cut.
    private var span: (x0: CGFloat, x1: CGFloat) {
        (knob / 2, max(knob / 2, bounds.width - knob / 2))
    }

    private func fraction(at p: NSPoint) -> CGFloat {
        let (x0, x1) = span
        return x1 > x0 ? min(1, max(0, (p.x - x0) / (x1 - x0))) : 0
    }

    /// Where the knob is, in points from the left (the snapshot hooks
    /// aim at it).
    var knobX: CGFloat {
        let (x0, x1) = span
        return x0 + (x1 - x0) * fraction
    }

    override func draw(_ dirtyRect: NSRect) {
        let mid = bounds.midY
        let (x0, x1) = span
        let x = knobX
        // The label colour, as the transport's glyphs: the track faint,
        // what has played stronger, the knob whole. Off, all faint.
        let alpha: CGFloat = enabled ? 1 : 0.4
        NSColor.labelColor.withAlphaComponent(0.16 * alpha).setFill()
        NSBezierPath(roundedRect: NSRect(x: x0, y: mid - track / 2,
                                         width: x1 - x0, height: track),
                     xRadius: track / 2, yRadius: track / 2).fill()
        NSColor.labelColor.withAlphaComponent(0.55 * alpha).setFill()
        NSBezierPath(roundedRect: NSRect(x: x0, y: mid - track / 2,
                                         width: x - x0, height: track),
                     xRadius: track / 2, yRadius: track / 2).fill()
        guard enabled else { return }
        let r = (pressed ? knob + 2 : knob) / 2
        NSColor.labelColor.setFill()
        NSBezierPath(ovalIn: NSRect(x: x - r, y: mid - r,
                                    width: 2 * r, height: 2 * r)).fill()
    }

    override func mouseDown(with event: NSEvent) {
        guard enabled else { return }
        pressed = true
        fraction = fraction(at: convert(event.locationInWindow, from: nil))
        needsDisplay = true
        onScrub?(.began, fraction)
    }

    override func mouseDragged(with event: NSEvent) {
        guard pressed else { return }
        let f = fraction(at: convert(event.locationInWindow, from: nil))
        guard f != fraction else { return }
        fraction = f
        needsDisplay = true
        onScrub?(.moved, f)
    }

    override func mouseUp(with event: NSEvent) {
        guard pressed else { return }
        fraction = fraction(at: convert(event.locationInWindow, from: nil))
        pressed = false
        needsDisplay = true
        onScrub?(.ended, fraction)
    }

    override func accessibilityPerformIncrement() -> Bool {
        onStep?(1)
        return true
    }

    override func accessibilityPerformDecrement() -> Bool {
        onStep?(-1)
        return true
    }
}
