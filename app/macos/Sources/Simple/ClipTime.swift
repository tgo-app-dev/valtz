import Foundation
import SwiftUI

/// A layer in TIME as the core resolves it (media/timing.h LayerTiming),
/// from the Trim panel's values as they are now: where it starts on the
/// composition's timeline and how long it runs there, and its source's
/// time at each moment of it -- `in` plus the integral of its speed, keyed
/// at its own frames of the composition's rate, linear between keys.
/// The Trim panel's clip times and the stage's mark past a clip's end
/// read it, between the panel's change and the core's next plan.
struct LayerClock: Equatable {
    /// The composition's frames a second.
    var fps = 24.0
    /// Timeline seconds.
    var start = 0.0
    var length = Double.infinity
    /// Source seconds at its start, and where its span ends.
    var inPoint = 0.0
    var outPoint = Double.infinity
    /// A clip, a sound, a timeline -- not a picture.
    var timed = false
    var speed = Keyframes<LayerSpeed>()

    var end: Double { start + length }

    private func clamped(_ r: Double) -> Double {
        min(LayerSpeed.range.upperBound, max(LayerSpeed.range.lowerBound, r))
    }

    /// Its speed `u` seconds after its start.
    func speed(atLocal u: Double) -> Double {
        clamped(speed.value(at: u * fps)?.rate ?? 1)
    }

    /// Source seconds `u` seconds after its start.
    func source(atLocal u: Double) -> Double {
        guard u > 0 else { return inPoint }
        let keys = speed.keys
        if keys.isEmpty { return inPoint + u }
        if keys.count == 1 { return inPoint + clamped(keys[0].value.rate) * u }
        // Between key times the speed ramps: a trapezoid each; before the
        // first and after the last, it holds.
        var s = inPoint
        var t = 0.0
        for k in keys {
            let kt = Double(k.frame) / fps
            if kt <= t { continue }
            let seg = min(kt, u) - t
            let v0 = speed(atLocal: t), v1 = speed(atLocal: kt)
            s += v0 * seg + 0.5 * (v1 - v0) / (kt - t) * seg * seg
            t += seg
            if t >= u { return s }
        }
        return s + speed(atLocal: t) * (u - t)
    }

    /// The local second at which its source reaches `target`.
    func local(atSource target: Double) -> Double {
        guard target > inPoint else { return 0 }
        if speed.keys.count <= 1 {
            let r = speed.keys.first.map { clamped($0.value.rate) } ?? 1
            return (target - inPoint) / r
        }
        // Monotonic (the speed is above 0): bisected.
        var lo = 0.0
        var hi = (target - inPoint) / LayerSpeed.range.lowerBound
        for _ in 0..<64 where hi - lo > 1e-7 {
            let mid = (lo + hi) / 2
            if source(atLocal: mid) < target { lo = mid } else { hi = mid }
        }
        return hi
    }
}

/// The selected layer's TIME on the stage (DESIGN §6a): what the Trim
/// panel shows and changes -- its marks and its start, and now its SPEED
/// and SOUND -- and where the player is in the clip itself.
extension AppModel {
    /// The frame on screen lies outside the selected layer's clip: before
    /// it starts, or after it ends.
    enum ClipEdge: String { case before, after }

    /// How long `a` runs, in seconds: a clip's, a sound's, a timeline's
    /// own length; nil for a picture.
    func runSeconds(of a: AssetDTO) -> Double? {
        if a.isComposition {
            guard a.isTimeline, let n = a.timeline ?? a.length, n > 0 else {
                return nil
            }
            return Double(n) / (a.compositionRate ?? .fallback).fps
        }
        guard a.kind == "video" || a.kind == "audio" else { return nil }
        return a.info?.seconds
    }

    /// What the selected layer shows (a flat clip on the stage: itself).
    var layerSource: AssetDTO? {
        guard let c = currentClip else { return nil }
        guard c.isComposition else { return c }
        return c.layers?.first { $0.id == activeLayer }
            .flatMap { source(of: $0) }
    }

    /// The selected layer in time, as the panel holds it.
    var layerClock: LayerClock? {
        guard let c = currentClip else { return nil }
        var k = LayerClock(fps: stageFrameRate.fps)
        guard c.isComposition else {
            // A flat clip: itself, from its first frame, as it is.
            guard let secs = runSeconds(of: c) else { return nil }
            k.timed = true
            k.outPoint = secs
            k.length = secs
            return k
        }
        let seconds = layerSource.flatMap { runSeconds(of: $0) } ?? 0
        let markRate = trimRate.fps
        k.start = Double(max(0, trimOffset)) / k.fps
        k.speed = layerSpeed
        k.timed = seconds > 0
        if let i = trim.markIn, i > 0 { k.inPoint = Double(i) / markRate }
        if k.timed {
            // A mark-out includes its frame: the span ends after it.
            let out = trim.markOut.map { Double($0 + 1) / markRate }
                ?? seconds
            k.outPoint = min(out, seconds)
            k.inPoint = min(k.inPoint, max(0, k.outPoint))
        }
        let d = c.time(of: activeLayer).duration
        if d > 0 {
            k.length = Double(d) / k.fps
        } else if k.timed {
            k.length = max(0, k.local(atSource: k.outPoint))
        }
        return k
    }

    /// The frame on screen is not in the selected layer's clip -- the
    /// timeline goes on past it (its black frames are useful: a layer can
    /// be started right after a clip ends), so the stage marks it as a
    /// film's torn end, and the panel's times say so.
    var clipEdge: ClipEdge? {
        guard clipOnStage, let k = layerClock, k.timed else { return nil }
        let t = Double(videoFrame) / k.fps
        if t < k.start - 1e-6 { return .before }
        if t >= k.end - 1e-6 { return .after }
        return nil
    }

    /// The frame of the selected layer's source showing at the playhead,
    /// at its speed: the CLIP's own time, which the panel shows around
    /// the transport.
    var sourceFrameAtPlayhead: Int {
        guard let c = currentClip, c.isComposition, let k = layerClock else {
            return videoFrame
        }
        let t = Double(videoFrame) / k.fps
        return trimRate.frame(at: k.source(atLocal: t - k.start))
    }

    /// The timeline frame where source frame `n` of the selected layer
    /// shows.
    func timelineFrame(ofSource n: Int) -> Int {
        guard let c = currentClip, c.isComposition, let k = layerClock else {
            return n
        }
        let local = k.local(atSource: Double(n) / trimRate.fps)
        return trimOffset + stageFrameRate.frame(at: local)
    }

    /// The selected layer runs in time (a clip, a sound, a timeline): its
    /// speed and sound mean something.
    var layerIsTimed: Bool { layerClock?.timed == true }

    /// What the selected layer shows has a sound: a sound, a clip with
    /// one, a timeline (its mix).
    var layerHasSound: Bool {
        guard let a = layerSource else { return false }
        if a.kind == "audio" { return true }
        if a.isComposition { return a.isTimeline }
        return a.info?.audio?.has == true
    }

    // MARK: Speed

    /// The selected layer's speed at the frame on screen.
    var layerSpeedHere: Double { layerSpeed.value(at: keyFrame)?.rate ?? 1 }

    /// Its speed there (keyed as its look is: one key is the whole clip's
    /// value, more change the one here -- or make one). A faster clip
    /// takes less of the timeline.
    func setLayerSpeed(_ rate: Double) {
        let r = min(LayerSpeed.range.upperBound,
                    max(LayerSpeed.range.lowerBound, rate))
        var k = layerSpeed.isEmpty ? Keyframes(start: LayerSpeed()) : layerSpeed
        k.edit(at: keyFrame) { $0.rate = r }
        layerSpeed = k
    }

    /// The clip's own frame rate when the timeline counts others: each of
    /// the timeline's frames shows the clip's frame at that moment (the
    /// nearest before it), so frames repeat or are skipped. The same at a
    /// speed other than 1.
    var layerFrameRate: FrameRate? {
        guard let a = layerSource, a.kind == "video" else { return nil }
        return a.isComposition ? a.compositionRate : a.info?.frameRate
    }

    /// The clip's frames are taken at other times than its own: another
    /// rate, or a speed.
    var layerResampled: Bool {
        guard let r = layerFrameRate else { return false }
        return r != stageFrameRate || layerSpeed.keys.contains {
            $0.value.rate != 1
        }
    }

    /// "23.976", "24", "29.97".
    static func fpsText(_ r: FrameRate) -> String {
        var t = String(format: "%.3f", r.fps)
        while t.contains("."), t.hasSuffix("0") { t.removeLast() }
        if t.hasSuffix(".") { t.removeLast() }
        return t
    }

    // MARK: Sound

    var layerPitchHere: Double { layerSound.value(at: keyFrame)?.pitch ?? 0 }
    var layerVolumeHere: Double {
        layerSound.value(at: keyFrame)?.volume ?? 1
    }

    /// The semitones its speed adds while its pitch follows it, as a
    /// tape's: twice as fast, an octave up.
    var pitchFromSpeed: Double {
        pitchFollowsSpeed ? 12 * log2(layerSpeedHere) : 0
    }

    private func editSound(_ change: (inout LayerSound) -> Void) {
        var k = layerSound.isEmpty ? Keyframes(start: LayerSound()) : layerSound
        k.edit(at: keyFrame, change)
        layerSound = k
    }

    /// Its pitch there, in semitones (held to two octaves either way).
    func setLayerPitch(_ semitones: Double) {
        let p = min(LayerSound.pitchRange.upperBound,
                    max(LayerSound.pitchRange.lowerBound, semitones))
        editSound { $0.pitch = p }
    }

    /// Its gain there: 0 mute, 1 as it is, up to 4 (+12 dB).
    func setLayerVolume(_ volume: Double) {
        let v = min(LayerSound.volumeRange.upperBound,
                    max(LayerSound.volumeRange.lowerBound, volume))
        editSound { $0.volume = v }
    }

    /// A gain in decibels ("-6.0 dB"; mute "-∞ dB").
    static func decibelText(_ volume: Double) -> String {
        guard volume > 0 else { return "−∞ dB" }
        let db = 20 * log10(volume)
        let t = String(format: "%.1f", abs(db) < 0.05 ? 0 : db)
        return (db >= 0.05 ? "+" : "") + t.replacingOccurrences(of: "-",
                                                               with: "−")
            + " dB"
    }
}

// MARK: - The Trim panel's rows

/// A small field the way the Crop panel's are: taken on Return or when it
/// is left; it shows its value again as it is, and follows it unless
/// something is being typed. Its x sets the value back.
struct TrimValueField: View {
    let name: LocalizedStringKey
    let shown: String
    var width: CGFloat = 64
    var isReset = true
    let commit: (String) -> Bool
    let reset: () -> Void
    @State private var text = ""
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 2) {
            TextField(name, text: $text)
                .labelsHidden()
                .textFieldStyle(.plain)
                .multilineTextAlignment(.trailing)
                .font(.callout.monospacedDigit())
                .focused($focused)
                .onSubmit(take)
                .frame(width: width)
            Button(action: reset) {
                Image(systemName: "xmark.circle.fill").font(.caption)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.tertiary)
            .help("Reset")
            .opacity(isReset ? 0 : 1)
            .disabled(isReset)
        }
        .padding(.leading, 6)
        .padding(.trailing, 3)
        .padding(.vertical, 2)
        .background(RoundedRectangle(cornerRadius: 5)
            .fill(Color(nsColor: .textBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 5)
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

/// The selected clip's SPEED, centred: "2×", "50%", typed. Where its
/// frames are taken at other times than its own -- a speed, or a timeline
/// at another rate -- it says so: the timeline shows the clip's frame
/// nearest before each of its own.
struct SpeedRow: View {
    @Bindable var model: AppModel

    var body: some View {
        let here = model.layerSpeedHere
        HStack(spacing: 8) {
            Text("Speed")
                .foregroundStyle(.secondary)
            TrimValueField(name: "Speed", shown: Self.display(here),
                           isReset: here == 1,
                           commit: { t in
                               guard let v = Self.parse(t) else { return false }
                               model.setLayerSpeed(v)
                               return true
                           },
                           reset: { model.setLayerSpeed(1) })
                .help("How fast the clip plays: 2× (or 200%) twice as fast, 0.5× half -- the clip then takes less, or more, of the timeline")
            if model.layerResampled {
                resampled
            }
        }
        .frame(maxWidth: .infinity)
        .searchMark(model.searchHits.contains(SettingsRow.speed))
    }

    @ViewBuilder
    private var resampled: some View {
        let clip = model.layerFrameRate.map(AppModel.fpsText) ?? ""
        let line = AppModel.fpsText(model.stageFrameRate)
        HStack(spacing: 4) {
            Image(systemName: "film.stack")
            if model.layerFrameRate != model.stageFrameRate {
                Text("\(clip) fps clip, \(line) fps timeline")
                Text(verbatim: "·")
            }
            Text("nearest frames")
        }
        .font(.callout)
        .foregroundStyle(.tertiary)
        .help("Each frame of the timeline shows the clip's frame at that moment: frames repeat or are skipped. (Rendering new frames in between is to come.)")
    }

    /// "1×", "0.5×", "1.25×".
    static func display(_ v: Double) -> String {
        CropField.display(v) + "×"
    }

    /// "2", "2x", "×2", "200%", "50 %"; nil for anything else.
    static func parse(_ text: String) -> Double? {
        var t = text.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ",", with: ".")
            .lowercased()
        let percent = t.hasSuffix("%")
        t = t.trimmingCharacters(in: CharacterSet(charactersIn: "x×% "))
        guard let v = Double(t), v.isFinite, v > 0 else { return nil }
        return percent ? v / 100 : v
    }
}

/// The selected clip's PITCH, centred: a semitone down or up, typed, and
/// whether it is HELD while the speed changes (stretched, its pitch kept)
/// or FOLLOWS the speed as a tape's does.
struct PitchRow: View {
    @Bindable var model: AppModel

    var body: some View {
        let here = model.layerPitchHere
        HStack(spacing: 6) {
            Text("Pitch")
                .foregroundStyle(.secondary)
            Button {
                model.setLayerPitch(here - 1)
            } label: {
                Image(systemName: "minus")
                    .frame(width: 12, height: 12)
            }
            .help("A semitone lower")
            .disabled(here <= LayerSound.pitchRange.lowerBound)
            TrimValueField(name: "Pitch", shown: Self.display(here),
                           width: 44, isReset: here == 0,
                           commit: { t in
                               guard let v = Self.parse(t) else { return false }
                               model.setLayerPitch(v)
                               return true
                           },
                           reset: { model.setLayerPitch(0) })
                .help("Semitones up (+) or down (−)")
            Button {
                model.setLayerPitch(here + 1)
            } label: {
                Image(systemName: "plus")
                    .frame(width: 12, height: 12)
            }
            .help("A semitone higher")
            .disabled(here >= LayerSound.pitchRange.upperBound)
            Text("semitones")
                .foregroundStyle(.tertiary)
                .padding(.trailing, 6)
            Picker("Pitch and speed", selection: $model.pitchFollowsSpeed) {
                Text("Hold").tag(false)
                Text("Follow Speed").tag(true)
            }
            .labelsHidden()
            .pickerStyle(.segmented)
            .fixedSize()
            .help("Hold: the pitch stays as the speed changes. Follow Speed: it rises and falls with the speed, as a tape's -- twice as fast, an octave up")
            if model.pitchFromSpeed != 0 {
                Text(verbatim: Self.display(model.pitchFromSpeed))
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.tertiary)
                    .help("What the speed adds")
            }
        }
        .frame(maxWidth: .infinity)
        .searchMark(model.searchHits.contains(SettingsRow.pitch))
    }

    /// "+2", "0", "−1.5".
    static func display(_ v: Double) -> String {
        let t = CropField.display((v * 10).rounded() / 10)
        return (v >= 0.05 ? "+" : "") + t.replacingOccurrences(of: "-",
                                                               with: "−")
    }

    /// "+3", "-1.5", "−2", "2 st"; nil for anything else.
    static func parse(_ text: String) -> Double? {
        var t = text.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ",", with: ".")
            .replacingOccurrences(of: "−", with: "-")
            .lowercased()
        t = t.trimmingCharacters(in: CharacterSet(charactersIn: "st +"))
        guard let v = Double(t), v.isFinite else { return nil }
        return v
    }
}

/// The selected clip's VOLUME, centred: muted, a slider in decibels
/// (-60 to +12; its far left mutes), and the gain typed -- "-6", "-6 dB"
/// or "50%".
struct VolumeRow: View {
    @Bindable var model: AppModel
    /// What a mute goes back to.
    @State private var unmuted = 1.0
    private static let floor = -60.0
    private static let ceiling = 12.0

    var body: some View {
        let v = model.layerVolumeHere
        HStack(spacing: 6) {
            Text("Volume")
                .foregroundStyle(.secondary)
            Button {
                if v > 0 {
                    unmuted = v
                    model.setLayerVolume(0)
                } else {
                    model.setLayerVolume(unmuted)
                }
            } label: {
                Image(systemName: v > 0 ? "speaker.wave.2" : "speaker.slash")
                    .frame(width: 18)
            }
            .buttonStyle(.borderless)
            .help(v > 0 ? "Mute" : "Unmute")
            Slider(value: Binding(get: { Self.position(v) },
                                  set: { model.setLayerVolume(Self.gain($0)) }),
                   in: Self.floor...Self.ceiling)
                .frame(width: 160)
                .accessibilityLabel(Text("Volume"))
            TrimValueField(name: "Volume", shown: AppModel.decibelText(v),
                           width: 62, isReset: v == 1,
                           commit: { t in
                               guard let g = Self.parse(t) else { return false }
                               model.setLayerVolume(g)
                               return true
                           },
                           reset: { model.setLayerVolume(1) })
                .help("Its gain: 0 dB as it is, −6 dB half the level, up to +12 dB")
        }
        .frame(maxWidth: .infinity)
        .searchMark(model.searchHits.contains(SettingsRow.volume))
    }

    /// The slider's place for a gain: decibels, mute at its far left.
    static func position(_ volume: Double) -> Double {
        guard volume > 0 else { return floor }
        return min(ceiling, max(floor, 20 * log10(volume)))
    }

    static func gain(_ db: Double) -> Double {
        db <= floor + 0.5 ? 0 : pow(10, db / 20)
    }

    /// "-6", "-6 dB", "+3dB", "50%", "-inf"; nil for anything else.
    static func parse(_ text: String) -> Double? {
        var t = text.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ",", with: ".")
            .replacingOccurrences(of: "−", with: "-")
            .lowercased()
        if t.contains("∞") || t.contains("inf") { return 0 }
        if t.hasSuffix("%") {
            t = t.trimmingCharacters(in: CharacterSet(charactersIn: "% "))
            guard let p = Double(t), p.isFinite, p >= 0 else { return nil }
            return p / 100
        }
        t = t.replacingOccurrences(of: "db", with: "")
            .trimmingCharacters(in: CharacterSet(charactersIn: " +"))
        guard let db = Double(t), db.isFinite else { return nil }
        return pow(10, db / 20)
    }
}

// MARK: - Past a clip's end, on the stage

/// A strip whose long side is a row of teeth -- a film's torn end: the
/// teeth on its left (the clip ended there), or on its right (it starts
/// there).
struct ZigzagStrip: Shape {
    var teethOnLeft = true
    var tooth: CGFloat = 12

    func path(in r: CGRect) -> Path {
        var p = Path()
        let depth = r.width * 0.55
        let n = max(1, Int((r.height / tooth).rounded()))
        let step = r.height / CGFloat(n)
        // Drawn with the teeth on the left, then turned over if need be.
        p.move(to: CGPoint(x: r.width, y: 0))
        p.addLine(to: CGPoint(x: r.width, y: r.height))
        p.addLine(to: CGPoint(x: depth, y: r.height))
        for i in stride(from: n - 1, through: 0, by: -1) {
            let y = CGFloat(i) * step
            p.addLine(to: CGPoint(x: 0, y: y + step / 2))
            p.addLine(to: CGPoint(x: depth, y: y))
        }
        p.closeSubpath()
        let t = teethOnLeft ? .identity
            : CGAffineTransform(a: -1, b: 0, c: 0, d: 1, tx: r.width, ty: 0)
        return p.applying(t.concatenating(
            CGAffineTransform(translationX: r.minX, y: r.minY)))
    }
}

/// Over the stage while the frame on screen is not in the selected clip
/// (DaVinci Resolve's mark): the film's torn end down the picture's side
/// -- its right after the clip's end, its left before its start -- and
/// what lies there named.
struct ClipEdgeMark: View {
    let edge: AppModel.ClipEdge
    private static let width: CGFloat = 14

    var body: some View {
        let after = edge == .after
        HStack(spacing: 8) {
            if !after { strip }
            Text(after ? "After the clip's end" : "Before the clip's start")
                .font(.system(size: 11, weight: .medium))
                .foregroundStyle(.white)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(Capsule().fill(Color.black.opacity(0.55)))
            if after { strip }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity,
               alignment: after ? .trailing : .leading)
        .allowsHitTesting(false)
        .transition(.opacity)
    }

    private var strip: some View {
        let shape = ZigzagStrip(teethOnLeft: edge == .after)
        return shape
            .fill(Color(cgColor: Guides.light))
            .overlay(shape.stroke(Color(cgColor: Guides.dark), lineWidth: 1))
            .frame(width: Self.width)
            .frame(maxHeight: .infinity)
    }
}
