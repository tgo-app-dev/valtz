import AppKit
import SwiftUI

/// The Audio panel: the selected layer's SOUND as a channel strip -- its
/// GAIN and PITCH on faders (an eighth of the panel), a ten-band
/// EQUALIZER (three eighths), its REVERB (a quarter: Decay, Pre-delay,
/// Mix, Damping, Size) and its SPACE (a quarter: Pan, Width, Distance,
/// Bass Mono, Crossfeed), each of those a value and a jog wheel -- keyed
/// as its look is (the key bar's `.sound` track, `AppModel.layerSound`,
/// written with the Trim panel's values; linear between keys, as the core
/// reads them): the gain, pitch, bands, the reverb's decay, mix and
/// damping, the pan, width and distance; the room's pre-delay and size
/// and the field's bass mono and crossfeed are the layer's, one for its
/// whole span (`AppModel.layerRoom`, `layerField`). The pitch is the
/// layer's own; whether it also FOLLOWS the speed, as a tape's, is the
/// Trim panel's, beside the speed. The gain plays as it changes; the
/// pitch, the equalizer, the pan, the reverb and the rest of the space
/// are the mixer's offline pass, in that order (media/sound.h: the room
/// convolved on the GPU, media/space.h), heard once the change rests and
/// the layer's span is made again.
struct AudioPanel: View {
    @Bindable var model: AppModel

    var body: some View {
        let here = model.layerSoundHere
        let frozen = model.layerFrozen
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                KeyframeBar(model: model, track: .sound)
                Spacer()
                Button("Flat") { model.flattenLayerEq() }
                    .disabled(model.layerSound.keys.allSatisfy(\.value.isFlat)
                              || frozen)
                    .help("Every band of the equalizer back to 0 dB, in every keyframe")
                Button("Dry") { model.dryLayerReverb() }
                    .disabled(!model.layerReverbSet || frozen)
                    .help("No reverb: its mix, decay and damping back as they were in every keyframe, and the room back too")
                Button("Centre") { model.centreLayerSound() }
                    .disabled(!model.layerPlaced || frozen)
                    .help("Where it was recorded: centred, as wide and as near in every keyframe, its lows not made mono, no crossfeed")
                Button("Reset") { model.resetLayerSound() }
                    .disabled((model.layerSound.isIdentity
                               && model.layerRoom == ReverbRoom()
                               && model.layerField == SoundField()) || frozen)
                    .help("Gain, pitch, equalizer, reverb and place back as the sound was, in every keyframe")
            }
            // An eighth, three eighths, a quarter and a quarter of the
            // panel, a line between each.
            GeometryReader { g in
                let gap = Self.gap * 2 + 1
                let inner = max(0, g.size.width - 3 * gap)
                HStack(spacing: 0) {
                    levels(here).frame(width: inner / 8)
                    Divider().padding(.horizontal, Self.gap)
                    equalizer(here).frame(width: inner * 3 / 8)
                    Divider().padding(.horizontal, Self.gap)
                    NumberSection<ReverbParam>(
                        title: String(localized: "Reverb"), model: model)
                        .frame(width: inner / 4)
                        .searchMark(model.searchHits.contains(
                            SettingsRow.audioReverb))
                    Divider().padding(.horizontal, Self.gap)
                    NumberSection<SpaceParam>(
                        title: String(localized: "Spatial"), model: model)
                        .frame(width: inner / 4)
                        .searchMark(model.searchHits.contains(
                            SettingsRow.audioSpace))
                }
            }
            // One height for the strip: every fader fills its column.
            .frame(height: Self.strip)
            // Greyed while the playhead is off the selected layer -- its
            // values would change unheard -- or it is locked.
            .disabled(frozen)
        }
        .controlSize(.small)
    }

    /// The gain and the pitch, side by side.
    private func levels(_ here: LayerSound) -> some View {
        HStack(alignment: .top, spacing: 4) {
            FaderColumn(
                id: "gain", name: String(localized: "Gain"),
                shown: AppModel.decibelText(here.volume),
                value: Self.gainPosition(here.volume),
                range: Self.gainFloor...Self.gainCeiling, rest: 0,
                step: 0.5, fine: 0.1, width: 28, stretches: true,
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
                rest: 0, step: 1, fine: 0.1, width: 28, stretches: true,
                help: String(localized: "Semitones up (+) or down (−), whole ones; ⌥ for tenths. Whether it also follows the speed is set beside the speed, in Trim. Double-click for none."),
                onChange: { model.setLayerPitch($0) })
                .searchMark(model.searchHits.contains(SettingsRow.pitch))
        }
    }

    /// The equalizer: its curve over its faders, a band a column.
    private func equalizer(_ here: LayerSound) -> some View {
        // As tall as the gain and the pitch: its faders shorter, the
        // curve over them.
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
            // Spread over the width, a band a column: the curve above
            // puts each band's centre over its fader.
            HStack(alignment: .top, spacing: 0) {
                ForEach(LayerSound.eqFrequencies.indices, id: \.self) { i in
                    FaderColumn(
                        id: "eq\(i)",
                        name: Self.hertzText(LayerSound.eqFrequencies[i]),
                        shown: Self.decibels(here.eq[i]),
                        value: here.eq[i],
                        range: LayerSound.eqRange, rest: 0,
                        step: 0.5, fine: 0.1, width: 18,
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

    static let gainFloor = -60.0
    static let gainCeiling = 12.0
    /// The strip's height: a gain or pitch fader with its value over it
    /// and its name under it.
    static let strip: CGFloat = 148
    /// Points either side of the lines between the sections.
    static let gap: CGFloat = 6

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

/// One of the Audio panel's numbers set on a row of its own -- the
/// reverb's, the space's: what it reads and sets on the model, how it is
/// shown and typed, how far a jog wheel's point moves it, and whether it
/// is keyed (in the keyframe at the playhead) or the layer's own.
protocol SoundNumber: CaseIterable, Identifiable, Hashable
    where AllCases: RandomAccessCollection {
    var name: String { get }
    var keyed: Bool { get }
    var help: String { get }
    /// Its value at rest.
    var rest: Double { get }
    /// A jog wheel's point.
    var perPoint: Double { get }
    @MainActor func value(_ m: AppModel) -> Double
    @MainActor func set(_ m: AppModel, _ v: Double)
    func display(_ v: Double) -> String
    func parse(_ text: String) -> Double?
    /// Moved by `delta` (a wheel's points times `perPoint`) from `now`.
    func nudged(_ now: Double, by delta: Double) -> Double
}

extension SoundNumber {
    func nudged(_ now: Double, by delta: Double) -> Double { now + delta }
}

/// What is typed: a number with a unit -- "ms", "s", "%", "hz", "m" --
/// each scaled to the unit kept; a bare one in `bare`'s.
private func typedNumber(_ text: String, bare: Double) -> Double? {
    var t = text.trimmingCharacters(in: .whitespaces).lowercased()
        .replacingOccurrences(of: ",", with: ".")
    var scale = bare
    for (unit, k) in [("ms", 0.001), ("hz", 1), ("%", 0.01), ("s", 1),
                      ("m", 1)] where t.hasSuffix(unit) {
        t.removeLast(unit.count)
        scale = k
        break
    }
    guard let v = Double(t.trimmingCharacters(in: .whitespaces)),
          v.isFinite else { return nil }
    return v * scale
}

/// One of the reverb's numbers (the Audio panel's third column): the
/// decay, the mix, the damping keyed; the room's pre-delay and size the
/// layer's own.
enum ReverbParam: String, CaseIterable, Identifiable, SoundNumber {
    case decay, preDelay, mix, damping, size
    var id: String { rawValue }

    var name: String {
        switch self {
        case .decay: String(localized: "Decay")
        case .preDelay: String(localized: "Pre-delay")
        case .mix: String(localized: "Mix")
        case .damping: String(localized: "Damping")
        case .size: String(localized: "Size")
        }
    }

    var keyed: Bool { self == .decay || self == .mix || self == .damping }

    var help: String {
        switch self {
        case .decay: String(localized: "Decay: how long the room rings, in seconds to fall 60 dB (RT60). Keyed. Its wheel: dragged right longer, 100 points doubling it; ⌥ finer, ⇧ faster.")
        case .preDelay: String(localized: "Pre-delay: how long before the room's first echo, the sound heard dry first. One for the whole layer, not keyed. Its wheel: a millisecond a point.")
        case .mix: String(localized: "Mix: dry to wet -- how much of the sound goes into the room, 0% none, 100% the room alone. Keyed: a mix brought down lets the room's tail ring out.")
        case .damping: String(localized: "Damping: how much sooner the room's highs die than its lows -- 0% bright, 100% dark. Keyed.")
        case .size: String(localized: "Size: how dense the room's echoes are -- small, they crowd in at once; large, they come apart and thicken as the sound dies. One for the whole layer, not keyed.")
        }
    }

    /// Its value where the playhead is, the room the layer's.
    func value(_ here: LayerSound, _ room: ReverbRoom) -> Double {
        switch self {
        case .decay: here.reverbDecay
        case .preDelay: room.preDelay
        case .mix: here.reverbMix
        case .damping: here.reverbDamping
        case .size: room.size
        }
    }

    @MainActor func value(_ m: AppModel) -> Double {
        value(m.layerSoundHere, m.layerRoom)
    }

    @MainActor func set(_ m: AppModel, _ v: Double) {
        m.setLayerReverb(self, v)
    }

    var rest: Double {
        switch self {
        case .decay: LayerSound.restDecay
        case .preDelay: ReverbRoom().preDelay
        case .mix: 0
        case .damping: LayerSound.restDamping
        case .size: ReverbRoom().size
        }
    }

    /// "1.5 s", "0.85 s"; "20 ms"; "25%".
    func display(_ v: Double) -> String {
        switch self {
        case .decay:
            String(format: v < 1 ? "%.2f s" : "%.1f s", v)
        case .preDelay:
            "\(Int((v * 1000).rounded())) ms"
        case .mix, .damping, .size:
            "\(Int((v * 100).rounded()))%"
        }
    }

    /// A decay in seconds ("ms" too), a pre-delay in milliseconds ("s"
    /// too), the rest in percent.
    func parse(_ text: String) -> Double? {
        switch self {
        case .decay: typedNumber(text, bare: 1)
        case .preDelay: typedNumber(text, bare: 0.001)
        case .mix, .damping, .size: typedNumber(text, bare: 0.01)
        }
    }

    /// An octave a hundred points for the decay, a millisecond for the
    /// pre-delay, half a percent for the rest.
    var perPoint: Double {
        switch self {
        case .decay: 0.01
        case .preDelay: 0.001
        case .mix, .damping, .size: 0.005
        }
    }

    func nudged(_ now: Double, by delta: Double) -> Double {
        self == .decay ? now * pow(2, delta) : now + delta
    }
}

/// One of the SPACE's numbers (the Audio panel's last column, core
/// media/space.h): the pan -- before the room -- the width and the
/// distance keyed; the bass mono and the crossfeed the layer's own.
enum SpaceParam: String, CaseIterable, Identifiable, SoundNumber {
    case pan, width, distance, bassMono, crossfeed
    var id: String { rawValue }

    var name: String {
        switch self {
        case .pan: String(localized: "Pan")
        case .width: String(localized: "Width")
        case .distance: String(localized: "Distance")
        case .bassMono: String(localized: "Bass Mono")
        case .crossfeed: String(localized: "Crossfeed")
        }
    }

    var keyed: Bool { self == .pan || self == .width || self == .distance }

    var help: String {
        switch self {
        case .pan: String(localized: "Pan: where it sits from left to right, before the reverb -- L 100 hard left, C the centre, R 100 hard right; as loud wherever it is. Keyed.")
        case .width: String(localized: "Width: how wide its stereo is, the reverb's with it -- 0% one channel, 100% as it is, 200% twice as wide. Keyed.")
        case .distance: String(localized: "Distance: how far away it sounds, in metres -- 1 m as recorded, each doubling 6 dB quieter and its highs duller, as the air dulls them. Keyed. Its wheel: 100 points doubling it.")
        case .bassMono: String(localized: "Bass Mono: its lows below this frequency made one, in the middle, what is above keeping its side -- Off, or 20 to 300 Hz. One for the whole layer, not keyed.")
        case .crossfeed: String(localized: "Crossfeed: each side's lows heard a little in the other ear, as a head lets them through, for listening on headphones; what both sides share is left as it was. One for the whole layer, not keyed.")
        }
    }

    @MainActor func value(_ m: AppModel) -> Double {
        let here = m.layerSoundHere
        switch self {
        case .pan: return here.pan
        case .width: return here.width
        case .distance: return here.distance
        case .bassMono: return m.layerField.bassMono
        case .crossfeed: return m.layerField.crossfeed
        }
    }

    @MainActor func set(_ m: AppModel, _ v: Double) {
        m.setLayerSpace(self, v)
    }

    var rest: Double {
        switch self {
        case .pan, .bassMono, .crossfeed: 0
        case .width, .distance: 1
        }
    }

    /// "C", "L 30", "R 100"; "100%"; "1.0 m", "12 m"; "Off", "120 Hz";
    /// "25%".
    func display(_ v: Double) -> String {
        switch self {
        case .pan:
            let p = Int((v * 100).rounded())
            return p == 0 ? "C" : p < 0 ? "L \(-p)" : "R \(p)"
        case .width, .crossfeed:
            return "\(Int((v * 100).rounded()))%"
        case .distance:
            return String(format: v < 10 ? "%.1f m" : "%.0f m", v)
        case .bassMono:
            return v < SoundField.bassMonoLowest
                ? String(localized: "Off") : "\(Int(v.rounded())) Hz"
        }
    }

    /// A pan as shown ("L 30", "R 50", "C") or a number from -100 to 100;
    /// a width or crossfeed in percent; a distance in metres; a bass
    /// mono's corner in Hz ("off" or 0 off).
    func parse(_ text: String) -> Double? {
        let t = text.trimmingCharacters(in: .whitespaces).lowercased()
        switch self {
        case .pan:
            if t == "c" || t == "centre" || t == "center" { return 0 }
            if let side = t.first, side == "l" || side == "r",
               let v = typedNumber(String(t.dropFirst()), bare: 0.01) {
                return side == "l" ? -abs(v) : abs(v)
            }
            return typedNumber(t, bare: 0.01)
        case .width, .crossfeed: return typedNumber(t, bare: 0.01)
        case .distance: return typedNumber(t, bare: 1)
        case .bassMono:
            if t == "off" || t == String(localized: "Off").lowercased() {
                return 0
            }
            return typedNumber(t, bare: 1)
        }
    }

    /// A percent a point for the pan and the width, an octave a hundred
    /// points for the distance, a hertz for the bass mono, half a percent
    /// for the crossfeed.
    var perPoint: Double {
        switch self {
        case .pan, .width: 0.01
        case .distance: 0.01
        case .bassMono: 1
        case .crossfeed: 0.005
        }
    }

    func nudged(_ now: Double, by delta: Double) -> Double {
        switch self {
        case .distance: return now * pow(2, delta)
        case .bassMono:
            // Off: turned up from its lowest corner; turned down past it,
            // off.
            if now < SoundField.bassMonoLowest {
                return delta > 0 ? SoundField.bassMonoLowest : 0
            }
            return now + delta
        default: return now + delta
        }
    }
}

/// A column of the Audio panel's numbers, a row each -- the name (a
/// diamond after it where it is keyed; a double-click puts it at rest),
/// the value typed, and its jog wheel.
private struct NumberSection<P: SoundNumber>: View {
    let title: String
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(verbatim: title)
                .font(.caption2.weight(.semibold))
                .foregroundStyle(.secondary)
                .frame(height: 16)
            ForEach(Array(P.allCases)) { p in
                row(p)
            }
            Spacer(minLength: 0)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity,
               alignment: .topLeading)
    }

    private func row(_ p: P) -> some View {
        let v = p.value(model)
        return HStack(spacing: 3) {
            HStack(spacing: 2) {
                Text(verbatim: p.name)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .minimumScaleFactor(0.7)
                if p.keyed {
                    Image(systemName: "diamond.fill")
                        .font(.system(size: 5))
                        .foregroundStyle(.tertiary)
                        .accessibilityHidden(true)
                }
            }
            .contentShape(Rectangle())
            .onTapGesture(count: 2) { p.set(model, p.rest) }
            .help(p.help)
            Spacer(minLength: 1)
            ReverbValueField(name: p.name, shown: p.display(v),
                             commit: { t in
                                 guard let x = p.parse(t) else {
                                     return false
                                 }
                                 p.set(model, x)
                                 return true
                             })
                .help(p.help)
            JogWheel(perPoint: p.perPoint,
                     help: LocalizedStringKey(p.help)) {
                model.nudgeSound(p, by: $0)
            }
        }
        .frame(height: 22)
        .accessibilityElement(children: .contain)
        .accessibilityLabel(Text(verbatim: p.name))
    }
}

/// One of the panel's numbers typed: taken on Return or when it is left,
/// shown again as it is, following it unless something is being typed.
private struct ReverbValueField: View {
    let name: String
    let shown: String
    let commit: (String) -> Bool
    @State private var text = ""
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField(name, text: $text)
            .labelsHidden()
            .textFieldStyle(.plain)
            .multilineTextAlignment(.trailing)
            .font(.caption.monospacedDigit())
            .focused($focused)
            .onSubmit(take)
            .frame(width: 40)
            .padding(.horizontal, 3)
            .padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 4)
                .fill(Color(nsColor: .textBackgroundColor)))
            .overlay(RoundedRectangle(cornerRadius: 4)
                .strokeBorder(Color(nsColor: .separatorColor)))
            .onAppear { show(shown) }
            .onChange(of: shown) { _, s in
                if text == lastShown { show(s) }
            }
            .onChange(of: focused) { _, f in
                if !f { take() }
            }
    }

    private func show(_ s: String) {
        text = s
        lastShown = s
    }

    private func take() {
        if text != lastShown && !text.isEmpty && !commit(text) {
            NSSound.beep()
        }
        show(shown)
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
                .minimumScaleFactor(0.7)
                // A point clear of the next column's: narrow ones shrink
                // rather than touch ("250500") -- in a row of one height,
                // so a shrunk one moves neither its fader nor its name.
                .padding(.horizontal, 1)
                .frame(height: 13)
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
