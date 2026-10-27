import AppKit
import SwiftUI

/// The Audio panel: the selected layer's SOUND as a channel strip -- its
/// GAIN, its PITCH and a ten-band EQUALIZER, each on a fader -- keyed as
/// its look is (the key bar's `.sound` track, `AppModel.layerSound`,
/// written with the Trim panel's values; linear between keys, as the core
/// reads them). The pitch is the layer's own; whether it also FOLLOWS
/// the speed, as a tape's, is the Trim panel's, beside the speed. The
/// gain plays as it changes; the pitch and the equalizer are the mixer's
/// offline pass (media/sound.h), heard once the change rests and the
/// mix is made again.
struct AudioPanel: View {
    @Bindable var model: AppModel

    var body: some View {
        let here = model.layerSoundHere
        let frozen = model.layerFrozen
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                KeyframeBar(model: model, track: .sound)
                Spacer()
                Button("Flat") { model.flattenLayerEq() }
                    .disabled(model.layerSound.keys.allSatisfy(\.value.isFlat)
                              || frozen)
                    .help("Every band of the equalizer back to 0 dB, in every keyframe")
                Button("Reset") { model.resetLayerSound() }
                    .disabled(model.layerSound.isIdentity || frozen)
                    .help("Gain, pitch and equalizer back as the sound was, in every keyframe")
            }
            HStack(alignment: .top, spacing: 14) {
                FaderColumn(
                    id: "gain", name: String(localized: "Gain"),
                    shown: AppModel.decibelText(here.volume),
                    value: Self.gainPosition(here.volume),
                    range: Self.gainFloor...Self.gainCeiling, rest: 0,
                    step: 0.5, fine: 0.1, width: 58,
                    help: String(localized: "The sound's gain: 0 dB as it is, −6 dB half the level, up to +12 dB; at the bottom, muted. ⌥ for finer steps; double-click for 0 dB."),
                    onChange: { model.setLayerVolume(Self.gain(atPosition: $0)) })
                    .searchMark(model.searchHits.contains(SettingsRow.audioGain))
                FaderColumn(
                    id: "pitch", name: String(localized: "Pitch"),
                    shown: Self.pitchText(here.pitch),
                    note: model.pitchFromSpeed != 0
                        ? String(localized: "\(Self.pitchText(model.pitchFromSpeed)) by speed")
                        : nil,
                    value: here.pitch, range: LayerSound.pitchRange,
                    rest: 0, step: 1, fine: 0.1, width: 58,
                    help: String(localized: "Semitones up (+) or down (−), whole ones; ⌥ for tenths. Whether it also follows the speed is set beside the speed, in Trim. Double-click for none."),
                    onChange: { model.setLayerPitch($0) })
                    .searchMark(model.searchHits.contains(SettingsRow.pitch))
                Divider()
                // As tall as the gain and the pitch: its faders shorter,
                // the curve over them.
                VStack(alignment: .leading, spacing: 4) {
                    EqCurve(gains: here.eq)
                        .frame(height: 40)
                        .overlay(alignment: .topLeading) {
                            Text("Equalizer")
                                .font(.caption2.weight(.semibold))
                                .foregroundStyle(.secondary)
                                .padding(.horizontal, 5)
                                .padding(.vertical, 3)
                                .allowsHitTesting(false)
                        }
                    // Spread over the width, a band a column: the curve
                    // above puts each band's centre over its fader.
                    HStack(alignment: .top, spacing: 0) {
                        ForEach(LayerSound.eqFrequencies.indices, id: \.self) {
                            i in
                            FaderColumn(
                                id: "eq\(i)",
                                name: Self.hertzText(
                                    LayerSound.eqFrequencies[i]),
                                shown: Self.decibels(here.eq[i]),
                                value: here.eq[i],
                                range: LayerSound.eqRange, rest: 0,
                                step: 0.5, fine: 0.1, width: 32,
                                stretches: true,
                                help: String(localized: "The band around \(Self.hertzText(LayerSound.eqFrequencies[i]))Hz, an octave wide: up to 12 dB louder or quieter. ⌥ for finer steps; double-click for 0 dB."),
                                onChange: { model.setLayerEq(i, $0) })
                        }
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity,
                       alignment: .topLeading)
                .searchMark(model.searchHits.contains(SettingsRow.audioEq))
            }
            // One height for the strip: every fader fills its column.
            .frame(height: Self.strip)
            // Greyed while the playhead is off the selected layer -- its
            // values would change unheard -- or it is locked.
            .disabled(frozen)
        }
        .controlSize(.small)
    }

    static let gainFloor = -60.0
    static let gainCeiling = 12.0
    /// The strip's height: a gain or pitch fader with its value over it
    /// and its name under it.
    static let strip: CGFloat = 148

    /// The gain fader's place for a gain: decibels, mute at its bottom.
    static func gainPosition(_ volume: Double) -> Double {
        guard volume > 0 else { return gainFloor }
        return min(gainCeiling, max(gainFloor, 20 * log10(volume)))
    }

    static func gain(atPosition db: Double) -> Double {
        db <= gainFloor + 0.5 ? 0 : pow(10, db / 20)
    }

    /// "+2", "0", "−1.5".
    static func pitchText(_ v: Double) -> String {
        let t = CropField.display((v * 10).rounded() / 10)
        return (v >= 0.05 ? "+" : "") + t.replacingOccurrences(of: "-",
                                                               with: "−")
    }

    /// A band's gain: "+3.5", "0", "−12".
    static func decibels(_ v: Double) -> String {
        let r = (v * 10).rounded() / 10
        if r == 0 { return "0" }
        let t = r == r.rounded() ? String(Int(r)) : String(format: "%.1f", r)
        return (r > 0 ? "+" : "") + t.replacingOccurrences(of: "-",
                                                           with: "−")
    }

    /// A band's centre: "31", "63", "500", "1k", "16k".
    static func hertzText(_ hz: Double) -> String {
        hz >= 1000 ? "\(Int((hz / 1000).rounded()))k"
                   : "\(Int(hz.rounded()))"
    }
}

/// A fader with its value over it and its name under it, filling the
/// height its row gives it.
struct FaderColumn: View {
    let id: String
    let name: String
    let shown: String
    var note: String? = nil
    let value: Double
    let range: ClosedRange<Double>
    let rest: Double
    let step: Double
    let fine: Double
    let width: CGFloat
    /// Its column as wide as the row gives it, the fader in its middle.
    var stretches = false
    let help: String
    let onChange: (Double) -> Void
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        VStack(spacing: 3) {
            Text(verbatim: shown)
                .font(.caption2.monospacedDigit())
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .minimumScaleFactor(0.7)
            Fader(id: id, value: value, range: range, rest: rest,
                  step: step, fine: fine, enabled: enabled,
                  onChange: onChange)
                .frame(width: width)
                .frame(minHeight: 40, maxHeight: .infinity)
                .help(help)
                .accessibilityLabel(Text(verbatim: name))
            Text(verbatim: name)
                .font(.caption2.weight(.medium))
                .foregroundStyle(.secondary)
                .lineLimit(1)
                // Under it, out of the row's height (the strip stays one
                // height whether a note shows or not).
                .overlay(alignment: .top) {
                    if let note {
                        Text(verbatim: note)
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                            .lineLimit(1)
                            .fixedSize()
                            .offset(y: 14)
                    }
                }
        }
        .frame(minWidth: width, maxWidth: stretches ? .infinity : width,
               maxHeight: .infinity)
        .opacity(enabled ? 1 : 0.55)
    }
}

/// The equalizer's response as drawn here: each band a peaking filter an
/// octave wide (the biquad of Robert Bristow-Johnson's cookbook, what an
/// AVAudioUnitEQ parametric band is), their gains added -- across, an
/// octave a fader's column, each band's centre over its fader (22 Hz to
/// 22.6 kHz); up and down, ±15 dB.
private struct EqCurve: View {
    let gains: [Double]

    var body: some View {
        Canvas { ctx, size in
            let mid = size.height / 2
            let y = { (db: Double) -> CGFloat in
                mid - CGFloat(db / 15) * (size.height / 2 - 2)
            }
            let f0 = LayerSound.eqFrequencies[0]
            let bands = Double(LayerSound.eqFrequencies.count)
            let x = { (hz: Double) -> CGFloat in
                CGFloat((log2(hz / f0) + 0.5) / bands) * size.width
            }
            ctx.fill(Path(roundedRect: CGRect(origin: .zero, size: size),
                          cornerRadius: 5),
                     with: .color(Color.primary.opacity(0.05)))
            var lines = Path()
            lines.move(to: CGPoint(x: 0, y: mid))
            lines.addLine(to: CGPoint(x: size.width, y: mid))
            for f in LayerSound.eqFrequencies {
                lines.move(to: CGPoint(x: x(f), y: 2))
                lines.addLine(to: CGPoint(x: x(f), y: size.height - 2))
            }
            ctx.stroke(lines, with: .color(Color.secondary.opacity(0.25)),
                       lineWidth: 0.5)
            let n = 160
            var curve = Path()
            for i in 0...n {
                let hz = f0 * pow(2, Double(i) / Double(n) * bands - 0.5)
                let p = CGPoint(x: x(hz),
                                y: y(min(15, max(-15, Self.response(gains,
                                                                    hz)))))
                if i == 0 { curve.move(to: p) } else { curve.addLine(to: p) }
            }
            ctx.stroke(curve, with: .color(.accentColor), lineWidth: 1.5)
        }
        .help("The equalizer's response, low to high: louder above the line, quieter below")
        .accessibilityHidden(true)
    }

    /// The bands' gains added at `hz`, decibels, at 48 kHz.
    static func response(_ gains: [Double], _ hz: Double) -> Double {
        let fs = 48000.0
        var total = 0.0
        for (i, g) in gains.enumerated() where g != 0 {
            let f0 = LayerSound.eqFrequencies[i]
            let a = pow(10, g / 40)
            let w0 = 2 * Double.pi * f0 / fs
            let alpha = sin(w0) * sinh(log(2) / 2 * 1 * w0 / sin(w0))
            let b = (1 + alpha * a, -2 * cos(w0), 1 - alpha * a)
            let d = (1 + alpha / a, -2 * cos(w0), 1 - alpha / a)
            let w = 2 * Double.pi * hz / fs
            func mag(_ c: (Double, Double, Double)) -> Double {
                let re = c.0 + c.1 * cos(w) + c.2 * cos(2 * w)
                let im = -(c.1 * sin(w) + c.2 * sin(2 * w))
                return hypot(re, im)
            }
            total += 20 * log10(mag(b) / mag(d))
        }
        return total
    }
}

// MARK: - A fader

/// A fader -- the Audio panel's, the Adjust panel's alpha -- an AppKit
/// view (its own mouse handling: a press, a drag, a double-click, the
/// scroll wheel).
struct Fader: NSViewRepresentable {
    let id: String
    let value: Double
    let range: ClosedRange<Double>
    let rest: Double
    let step: Double
    let fine: Double
    let enabled: Bool
    let onChange: (Double) -> Void

    func makeNSView(context: Context) -> FaderNSView {
        let v = FaderNSView(id: id)
        update(v)
        return v
    }

    func updateNSView(_ v: FaderNSView, context: Context) { update(v) }

    private func update(_ v: FaderNSView) {
        v.range = range
        v.rest = rest
        v.step = step
        v.fine = fine
        v.onChange = onChange
        v.show(value, enabled: enabled)
    }
}

/// A vertical fader: its value up and down a track, marked at its rest
/// and filled from there in the accent. A press on its knob drags it; a
/// press elsewhere on the track puts it there and drags on. Steps of
/// `step` (⌥ held: `fine`), a double-click its rest, the scroll wheel a
/// step at a time.
final class FaderNSView: NSView {
    /// Points: the track's ends this far in, and a press this near the
    /// knob grabs it.
    private static let inset: CGFloat = 7
    private static let grab: CGFloat = 8

    let id: String
    var range: ClosedRange<Double> = 0...1
    var rest = 0.0
    var step = 0.0
    var fine = 0.0
    var onChange: ((Double) -> Void)?
    private(set) var value = 0.0
    private var enabled = true
    /// A drag: where it was pressed, the value then.
    private var held: (press: CGPoint, from: Double)?

    /// The faders shown, by id ("gain", "pitch", "eq0" ..., "alpha"): the
    /// snapshot hooks post their events to them.
    private static let shown = NSMapTable<NSString, FaderNSView>
        .strongToWeakObjects()
    static func view(_ id: String) -> FaderNSView? {
        shown.object(forKey: id as NSString)
    }

    init(id: String) {
        self.id = id
        super.init(frame: .zero)
        setAccessibilityElement(true)
        setAccessibilityRole(.slider)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil { Self.shown.setObject(self, forKey: id as NSString) }
    }

    /// The model's value -- not while dragged: the drag's own is shown.
    func show(_ v: Double, enabled e: Bool) {
        let changed = (held == nil && v != value) || e != enabled
        if held == nil { value = v }
        enabled = e
        setAccessibilityValue(value)
        setAccessibilityMinValue(range.lowerBound)
        setAccessibilityMaxValue(range.upperBound)
        if changed { needsDisplay = true }
    }

    // MARK: Where things are

    private var span: CGFloat { max(1, bounds.height - 2 * Self.inset) }

    private func y(_ v: Double) -> CGFloat {
        let lo = range.lowerBound, hi = range.upperBound
        let f = hi > lo ? (min(hi, max(lo, v)) - lo) / (hi - lo) : 0
        return Self.inset + CGFloat(1 - f) * span
    }

    private func value(atY y: CGFloat) -> Double {
        let f = Double(1 - (y - Self.inset) / span)
        return range.lowerBound
            + min(1, max(0, f)) * (range.upperBound - range.lowerBound)
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        g.setAlpha(enabled ? 1 : 0.5)
        let x = bounds.midX
        let track = CGRect(x: x - 2, y: Self.inset, width: 4, height: span)
        g.addPath(CGPath(roundedRect: track, cornerWidth: 2,
                         cornerHeight: 2, transform: nil))
        g.setFillColor(NSColor.quaternaryLabelColor.cgColor)
        g.fillPath()
        // Its rest, across the track.
        let r = y(rest).rounded() + 0.5
        g.setStrokeColor(NSColor.secondaryLabelColor.cgColor)
        g.setLineWidth(1)
        g.strokeLineSegments(between: [CGPoint(x: x - 8, y: r),
                                       CGPoint(x: x + 8, y: r)])
        // From the rest to the value.
        let v = y(value)
        let fill = CGRect(x: x - 2, y: min(r, v), width: 4,
                          height: abs(v - r))
        g.setFillColor(NSColor.controlAccentColor.cgColor)
        g.fill(fill)
        // The knob.
        let knob = CGRect(x: x - min(11, bounds.width / 2 - 1), y: v - 5,
                          width: min(22, bounds.width - 2), height: 10)
        g.setShadow(offset: CGSize(width: 0, height: 0.5), blur: 1.5,
                    color: NSColor.black.withAlphaComponent(0.35).cgColor)
        g.addPath(CGPath(roundedRect: knob, cornerWidth: 3, cornerHeight: 3,
                         transform: nil))
        g.setFillColor((held != nil ? NSColor.controlAccentColor
                                    : NSColor.white).cgColor)
        g.fillPath()
        g.setShadow(offset: .zero, blur: 0, color: nil)
        g.addPath(CGPath(roundedRect: knob.insetBy(dx: 0.5, dy: 0.5),
                         cornerWidth: 3, cornerHeight: 3, transform: nil))
        g.setStrokeColor(NSColor.black.withAlphaComponent(0.25).cgColor)
        g.strokePath()
    }

    // MARK: The pointer

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: enabled ? .resizeUpDown : .arrow)
    }

    private func snapped(_ v: Double, fineStep: Bool) -> Double {
        let s = fineStep ? fine : step
        let w = s > 0 ? (v / s).rounded() * s : v
        return min(range.upperBound, max(range.lowerBound, w))
    }

    override func mouseDown(with e: NSEvent) {
        guard enabled else { return }
        if MarkupPointer.clicks(e) >= 2 {
            held = nil
            set(rest)
            return
        }
        let p = convert(e.locationInWindow, from: nil)
        if abs(p.y - y(value)) > Self.grab {
            set(snapped(value(atY: p.y),
                        fineStep: e.modifierFlags.contains(.option)))
        }
        held = (p, value)
        needsDisplay = true
    }

    override func mouseDragged(with e: NSEvent) {
        guard let h = held else { return }
        let p = convert(e.locationInWindow, from: nil)
        let fineStep = e.modifierFlags.contains(.option)
        let per = (range.upperBound - range.lowerBound) / Double(span)
        // ⌥: a tenth as far for the same travel.
        let moved = Double(h.press.y - p.y) * per * (fineStep ? 0.1 : 1)
        set(snapped(h.from + moved, fineStep: fineStep))
    }

    override func mouseUp(with e: NSEvent) {
        guard held != nil else { return }
        held = nil
        needsDisplay = true
    }

    override func scrollWheel(with e: NSEvent) {
        guard enabled, e.scrollingDeltaY != 0 else { return }
        let fineStep = e.modifierFlags.contains(.option)
        let s = fineStep ? fine : step
        set(snapped(value + (e.scrollingDeltaY > 0 ? s : -s),
                    fineStep: fineStep))
    }

    override func accessibilityPerformIncrement() -> Bool {
        set(snapped(value + step, fineStep: false))
        return true
    }

    override func accessibilityPerformDecrement() -> Bool {
        set(snapped(value - step, fineStep: false))
        return true
    }

    private func set(_ v: Double) {
        let c = min(range.upperBound, max(range.lowerBound, v))
        guard c != value else { return }
        value = c
        setAccessibilityValue(c)
        onChange?(c)
        needsDisplay = true
    }
}
