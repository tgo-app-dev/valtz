import AVFoundation
import CoreImage
import Foundation
import SwiftUI

/// A value a clip's keyframes can carry: interpolated between two keys
/// (core media/keyframes.h).
protocol Keyable: Equatable, Sendable {
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self
    /// Each number of `a` made by `f` from it and the same number of `p`,
    /// `b` and `n` -- `a`'s other parts kept, as `lerp` keeps them (the
    /// core's zip): a spline taken number by number.
    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self
    var isIdentity: Bool { get }
}

/// How a key's value goes on to the next key's (core media::Interp): not
/// at all until the next key -- held, an abrupt change there -- in a
/// straight line, or along a smooth curve through the keys.
enum KeyInterp: String, CaseIterable, Identifiable, Sendable {
    case hold, linear, spline
    var id: String { rawValue }

    var label: String {
        switch self {
        case .hold: String(localized: "None")
        case .linear: String(localized: "Linear")
        case .spline: String(localized: "Spline")
        }
    }

    var help: String {
        switch self {
        case .hold: String(localized: "None: held until the next keyframe, then changed at once")
        case .linear: String(localized: "Linear: changed at an even pace to the next keyframe")
        case .spline: String(localized: "Spline: eased along a smooth curve through the keyframes")
        }
    }

    /// Its picture: two keys (dots) and the way between them -- a step,
    /// a straight line, an S-curve -- drawn as a template image.
    var image: NSImage {
        let size = NSSize(width: 18, height: 14)
        let img = NSImage(size: size, flipped: false) { _ in
            let a = NSPoint(x: 3, y: 3), b = NSPoint(x: 15, y: 11)
            let path = NSBezierPath()
            path.lineWidth = 1.5
            path.lineCapStyle = .round
            path.lineJoinStyle = .round
            path.move(to: a)
            switch self {
            case .hold:
                path.line(to: NSPoint(x: b.x, y: a.y))
                path.line(to: b)
            case .linear:
                path.line(to: b)
            case .spline:
                path.curve(to: b, controlPoint1: NSPoint(x: 10, y: a.y),
                           controlPoint2: NSPoint(x: 8, y: b.y))
            }
            NSColor.black.setStroke()
            path.stroke()
            for p in [a, b] {
                NSBezierPath(ovalIn: NSRect(x: p.x - 2, y: p.y - 2,
                                            width: 4, height: 4)).fill()
            }
            return true
        }
        img.isTemplate = true
        return img
    }
}

/// A SPLINE's number between two keys -- the core's media::spline, the
/// same arithmetic: a cubic Hermite from `a` to `b` (`h` frames apart,
/// `t` of the way), its slopes Fritsch and Butland's from the keys either
/// side it runs on through (`p` `hp` frames before, `n` `hn` after; nil:
/// level there).
func keySpline(_ p: Double, _ a: Double, _ b: Double, _ n: Double,
               t: Double, h: Double, hp: Double?, hn: Double?) -> Double {
    let h = max(1e-9, h)
    let d = (b - a) / h
    func slope(_ d0: Double, _ d1: Double, _ h0: Double,
               _ h1: Double) -> Double {
        if d0 * d1 <= 0 { return 0 }
        return 3 * (h0 + h1) / ((2 * h1 + h0) / d0 + (h1 + 2 * h0) / d1)
    }
    let m0 = hp.map { slope((a - p) / max(1e-9, $0), d, $0, h) } ?? 0
    let m1 = hn.map { slope(d, (n - b) / max(1e-9, $0), h, $0) } ?? 0
    let t = min(1, max(0, t)), t2 = t * t, t3 = t2 * t
    return (2 * t3 - 3 * t2 + 1) * a + (t3 - 2 * t2 + t) * h * m0
        + (-2 * t3 + 3 * t2) * b + (t3 - t2) * h * m1
}

/// A value that changes over a clip -- its adjustments, its crop: keys at
/// frames, and between two of them the values interpolated as the first
/// one says (held, linear, a spline); before the first key, the first,
/// after the last, the last. A track of one key is a value for the whole
/// clip, which is how a clip starts (a key at its first frame). Recorded
/// on the clip as its "adjust" / "crop" modifier ({"keys": [{"frame",
/// ..., "interp"?}], "rate_num", "rate_den"}).
struct Keyframes<T: Keyable>: Equatable, Sendable {
    struct Key: Equatable, Sendable {
        var frame: Int
        var value: T
        /// On to the next key.
        var interp: KeyInterp = .linear
    }

    /// Ascending frames, no two alike.
    private(set) var keys: [Key] = []

    init() {}

    /// One key at the first frame.
    init(start value: T) {
        keys = [Key(frame: 0, value: value)]
    }

    /// From a modifier's keys, each with its "frame" (none: 0) -- and,
    /// on the look's tracks, its "interp" (`interps`, in the same order).
    init(_ dicts: [[String: Double]], interps: [String?]? = nil,
         value: ([String: Double]) -> T) {
        for (i, d) in dicts.enumerated() {
            let f = max(0, Int(d["frame"] ?? 0))
            set(value(d), at: f)
            // Its own, not the span's it fell in as the keys were read.
            let s = interps.flatMap { i < $0.count ? $0[i] : nil }
            if let k = index(at: f) {
                keys[k].interp = s.flatMap(KeyInterp.init) ?? .linear
            }
        }
    }

    /// As the core reads a track; nothing when every key is the identity.
    func json(rate: FrameRate,
              value: (T) -> [String: Double]) -> [String: Any] {
        guard !isIdentity else { return [:] }
        return [
            "keys": keys.map { k -> [String: Any] in
                var d: [String: Any] = value(k.value)
                d["frame"] = Double(k.frame)
                if k.interp != .linear { d["interp"] = k.interp.rawValue }
                return d
            },
            "rate_num": rate.num, "rate_den": rate.den,
        ]
    }

    var isEmpty: Bool { keys.isEmpty }
    var isIdentity: Bool { keys.allSatisfy { $0.value.isIdentity } }

    func value(at frame: Int) -> T? { value(at: Double(frame)) }

    /// Between frames too: a layer's speed at any moment.
    func value(at frame: Double) -> T? {
        guard let first = keys.first, let last = keys.last else { return nil }
        if frame <= Double(first.frame) { return first.value }
        if frame >= Double(last.frame) { return last.value }
        let b = keys.firstIndex { Double($0.frame) > frame }!
        let ka = keys[b - 1], kb = keys[b]
        // On a key: its own value (a mixer's curves its own points, not
        // laid on the next key's).
        if frame == Double(ka.frame) { return ka.value }
        let h = Double(kb.frame - ka.frame)
        let t = (frame - Double(ka.frame)) / h
        switch ka.interp {
        case .hold: return ka.value
        case .linear: return T.lerp(ka.value, kb.value, t)
        case .spline: break
        }
        // On through a key either side unless that side is held.
        let before = b >= 2 && keys[b - 2].interp != .hold
            ? keys[b - 2] : nil
        let after = b + 1 < keys.count && kb.interp != .hold
            ? keys[b + 1] : nil
        let hp = before.map { Double(ka.frame - $0.frame) }
        let hn = after.map { Double($0.frame - kb.frame) }
        return T.zip(before?.value ?? ka.value, ka.value, kb.value,
                     after?.value ?? kb.value) {
            keySpline($0, $1, $2, $3, t: t, h: h, hp: hp, hn: hn)
        }
    }

    /// How the span `frame` is in goes on: its first key's (before the
    /// first key: linear).
    func interp(at frame: Int) -> KeyInterp {
        keys.last { $0.frame <= frame }?.interp ?? .linear
    }

    /// The key a span starts at: the one at `frame`, else the one before.
    func spanKey(at frame: Int) -> Int? {
        keys.lastIndex { $0.frame <= frame }
    }

    /// How the key at (or before) `frame` goes on to the next.
    mutating func setInterp(_ how: KeyInterp, at frame: Int) {
        if let i = spanKey(at: frame) { keys[i].interp = how }
    }

    /// The key at `frame`, if there is one.
    func index(at frame: Int) -> Int? {
        keys.firstIndex { $0.frame == frame }
    }

    /// The nearest key before (or after) `frame`.
    func previous(before frame: Int) -> Int? {
        keys.last { $0.frame < frame }?.frame
    }

    func next(after frame: Int) -> Int? {
        keys.first { $0.frame > frame }?.frame
    }

    /// Put `value` at `frame`: that key's, or a new key there -- going on
    /// as the span it is made in did (a held span stays held).
    mutating func set(_ value: T, at frame: Int) {
        if let i = index(at: frame) {
            keys[i].value = value
        } else {
            keys.append(Key(frame: frame, value: value,
                            interp: interp(at: frame)))
            keys.sort { $0.frame < $1.frame }
        }
    }

    mutating func remove(at frame: Int) {
        keys.removeAll { $0.frame == frame }
    }

    /// Every key's value made by `f`, the keys where they were.
    func mapValues(_ f: (T) -> T) -> Self {
        var o = self
        for i in o.keys.indices { o.keys[i].value = f(o.keys[i].value) }
        return o
    }

    /// The same, with keys made alike by it merged: a track whose keys all
    /// hold one value is that value, one key at its first frame.
    func collapsingValues(_ f: (T) -> T) -> Self {
        let o = mapValues(f)
        guard let first = o.keys.first,
              o.keys.allSatisfy({ $0.value == first.value }) else { return o }
        return Self(start: first.value)
    }

    /// An edit of the value shown at `frame`. With one key the clip is not
    /// animated yet: the edit changes that key, wherever the clip is. With
    /// more, it changes the key at `frame` -- or makes one there.
    mutating func edit(at frame: Int, _ change: (inout T) -> Void) {
        guard var v = value(at: frame) else { return }
        change(&v)
        if keys.count == 1 {
            keys[0].value = v
        } else {
            set(v, at: frame)
        }
    }
}

extension ImageAdjustments: Keyable {
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self {
        Self.zip(a, a, b, b) { _, x, y, _ in x + (y - x) * t }
    }

    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        var o = a
        for k in Key.allCases { o[k] = f(p[k], a[k], b[k], n[k]) }
        // The color mixer's curves, laid on one another's points: each
        // value and slope there on its own (the core's zip_mix).
        o.mix = ColorMix.zip(p.mix, a.mix, b.mix, n.mix, f)
        // The grade's tints across the wheel's disk (the core's
        // zip_grade).
        o.grade = ColorGrade.zip(p.grade, a.grade, b.grade, n.grade, f)
        return o
    }
}

extension CropSpec: Keyable {
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self {
        Self.zip(a, a, b, b) { _, x, y, _ in x + (y - x) * t }
    }

    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        var o = a.contentWidth > 0 ? a : b
        o.scaleX = f(p.scaleX, a.scaleX, b.scaleX, n.scaleX)
        o.scaleY = f(p.scaleY, a.scaleY, b.scaleY, n.scaleY)
        o.offsetX = f(p.offsetX, a.offsetX, b.offsetX, n.offsetX)
        o.offsetY = f(p.offsetY, a.offsetY, b.offsetY, n.offsetY)
        o.rotate = f(p.rotate, a.rotate, b.rotate, n.rotate)
        func at(_ c: [Double], _ i: Int) -> Double {
            i < c.count ? c[i] : a.pad[i]
        }
        o.pad = a.pad.indices.map { i in
            f(at(p.pad, i), a.pad[i], at(b.pad, i), at(n.pad, i))
        }
        return o
    }
}

/// A composition layer's playback RATE (core media::Speed): its source
/// advances `rate` seconds a second; a ramp between two keys eases.
struct LayerSpeed: Keyable {
    var rate = 1.0
    static let range = 0.05...20.0
    var isIdentity: Bool { rate == 1 }
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self {
        LayerSpeed(rate: a.rate + (b.rate - a.rate) * t)
    }
    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        LayerSpeed(rate: f(p.rate, a.rate, b.rate, n.rate))
    }
}

/// A composition layer's SOUND (core media::Sound): its gain -- 0 mute, 1
/// as it is, up to 4 (+12 dB) -- and its pitch in semitones.
struct LayerSound: Keyable {
    var volume = 1.0
    var pitch = 0.0
    /// The equalizer (the core's media::Sound::eq): a gain in decibels
    /// for each band, an octave wide, centred on `eqFrequencies`.
    var eq = [Double](repeating: 0, count: LayerSound.eqFrequencies.count)
    static let volumeRange = 0.0...4.0
    static let pitchRange = -24.0...24.0
    static let eqRange = -12.0...12.0
    /// The bands' centres, Hz (media::kEqFrequencies).
    static let eqFrequencies: [Double] = [31.25, 62.5, 125, 250, 500, 1000,
                                          2000, 4000, 8000, 16000]
    var isFlat: Bool { eq.allSatisfy { $0 == 0 } }
    var isIdentity: Bool { volume == 1 && pitch == 0 && isFlat }

    init(volume: Double = 1, pitch: Double = 0, eq: [Double]? = nil) {
        self.volume = volume
        self.pitch = pitch
        if let eq, eq.count == Self.eqFrequencies.count { self.eq = eq }
    }

    /// From a key's numbers, as the core keeps them: "volume", "pitch",
    /// "eq_0" ... "eq_9".
    init(_ p: [String: Double]) {
        self.init(volume: p["volume"] ?? 1, pitch: p["pitch"] ?? 0)
        for i in eq.indices {
            if let g = p["eq_\(i)"], g.isFinite {
                eq[i] = min(Self.eqRange.upperBound,
                            max(Self.eqRange.lowerBound, g))
            }
        }
    }

    /// Its numbers as the core reads a key's: a band while it is set.
    var json: [String: Double] {
        var j = ["volume": volume, "pitch": pitch]
        for (i, g) in eq.enumerated() where g != 0 { j["eq_\(i)"] = g }
        return j
    }

    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self {
        Self.zip(a, a, b, b) { _, x, y, _ in x + (y - x) * t }
    }
    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        LayerSound(volume: f(p.volume, a.volume, b.volume, n.volume),
                   pitch: f(p.pitch, a.pitch, b.pitch, n.pitch),
                   eq: a.eq.indices.map { f(p.eq[$0], a.eq[$0], b.eq[$0],
                                            n.eq[$0]) })
    }
}

/// A clip's turn, in degrees (+ clockwise): its crop's rotation, keyed on
/// its own.
struct Turn: Keyable {
    var degrees = 0.0
    var isIdentity: Bool { degrees == 0 }
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self {
        Turn(degrees: a.degrees + (b.degrees - a.degrees) * t)
    }
    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        Turn(degrees: f(p.degrees, a.degrees, b.degrees, n.degrees))
    }
}

/// Which track a key control works on: a clip's adjustments, where its
/// frames lie (offsets and scales), or how they are turned.
/// A clip's tracks -- and the parts of any picture's look a held Bypass
/// leaves out of what the stage shows.
enum KeyTrack: Hashable, Sendable {
    case adjust, place, turn
    /// The selected layer's sound (`AppModel.layerSound`): the Audio
    /// panel's -- its keys linear, as the core reads them.
    case sound
}

/// A clip's crop (core media/keyframes.h KeyedCrop): where its frames lie
/// and how they are turned, two tracks keyed apart, over one background
/// colour for the whole clip.
struct ClipCrop: Equatable, Sendable {
    var place = Keyframes<CropSpec>()
    var turn = Keyframes<Turn>()
    var pad: [Double] = [0, 0, 0, 1]

    /// One key each at the first frame.
    static let start = ClipCrop(place: Keyframes(start: CropSpec()),
                                turn: Keyframes(start: Turn()))

    var isEmpty: Bool { place.isEmpty && turn.isEmpty }
    var isIdentity: Bool { place.isIdentity && turn.isIdentity }

    /// The crop at `frame`.
    func value(at frame: Int) -> CropSpec {
        var c = place.value(at: frame) ?? CropSpec()
        c.rotate = turn.value(at: frame)?.degrees ?? 0
        c.pad = pad
        return c
    }

    /// From a clip's "crop" modifier: its keys, its rotate keys (or each
    /// key's own rotation, as a crop keyed whole was), its background.
    init(place: Keyframes<CropSpec> = Keyframes(),
         turn: Keyframes<Turn> = Keyframes(), pad: [Double] = [0, 0, 0, 1]) {
        self.place = place
        self.turn = turn
        self.pad = pad
    }

    init(_ m: ModifierDTO) {
        let keys = m.keys ?? [m.params]
        place = Keyframes(keys, interps: m.keyInterps) { d in
            var c = CropSpec(d)
            c.rotate = 0
            return c
        }
        // A crop keyed whole: the turn goes on as the placement does.
        turn = Keyframes(m.rotateKeys ?? keys,
                         interps: m.rotateKeys != nil ? m.rotateInterps
                                                      : m.keyInterps) {
            Turn(degrees: $0["rotate"] ?? 0)
        }
        let first = keys.first ?? [:]
        pad = ["pad_r", "pad_g", "pad_b", "pad_a"].enumerated().map { i, k in
            m.params[k] ?? first[k] ?? (i == 3 ? 1 : 0)
        }
    }

    /// As the core reads it; nothing when it changes nothing.
    func json(rate: FrameRate) -> [String: Any] {
        guard !isIdentity else { return [:] }
        var j: [String: Any] = [
            "keys": place.keys.map { k -> [String: Any] in
                let c = k.value
                var d: [String: Any] = [
                    "frame": Double(k.frame),
                    "content_w": Double(c.contentWidth),
                    "content_h": Double(c.contentHeight),
                    "scale_x": c.scaleX, "scale_y": c.scaleY,
                    "offset_x": c.offsetX, "offset_y": c.offsetY]
                // A still's crop may name its canvas (a page's look).
                if c.canvasWidth > 0, c.canvasHeight > 0 {
                    d["canvas_w"] = Double(c.canvasWidth)
                    d["canvas_h"] = Double(c.canvasHeight)
                }
                if k.interp != .linear { d["interp"] = k.interp.rawValue }
                return d
            },
            "rotate_keys": turn.keys.map { k -> [String: Any] in
                var d: [String: Any] = ["frame": Double(k.frame),
                                        "rotate": k.value.degrees]
                if k.interp != .linear { d["interp"] = k.interp.rawValue }
                return d
            },
            "rate_num": rate.num, "rate_den": rate.den,
        ]
        for (i, k) in ["pad_r", "pad_g", "pad_b", "pad_a"].enumerated() {
            j[k] = pad[i]
        }
        return j
    }
}

/// A clip's look -- its adjustment and crop tracks -- as the player draws
/// it: each frame, at its own frame number, with the adjustment chain and
/// the crop placement the core defines (the same an export makes real).
struct ClipLook: Equatable, Sendable {
    var adjust: Keyframes<ImageAdjustments>
    var crop: ClipCrop
    var rate: FrameRate

    var isIdentity: Bool { adjust.isIdentity && crop.isIdentity }

    /// The largest frame a look is placed in, a side: a 16K picture's.
    static let largestFrame: CGFloat = 16_384

    /// `image` (a frame, extent at the origin) with the look at `frame`.
    func apply(_ image: CIImage, frame: Int, core: CoreService) -> CIImage {
        var img = image
        if let a = adjust.value(at: frame), !a.isIdentity {
            img = ImageAdjustments.applyChain(core.adjustmentChain(a), to: img)
        }
        // A time with no picture -- a composition's gap, past its last
        // frame -- comes as Core Image's empty frame: an extent INFINITE
        // (CGRect.infinite's width is finite, and no Int holds it) or
        // null. Its crop has no size to be placed in: it is left as it
        // is. Converted, it trapped on AVFoundation's thread (Valtz 95 on
        // the M5 Ultra, twice: a clip with a look on the stage while an
        // agent worked).
        let ext = image.extent
        guard ext.width >= 1, ext.height >= 1,
              ext.width <= Self.largestFrame,
              ext.height <= Self.largestFrame else { return img }
        if case let c = crop.value(at: frame), !c.isIdentity,
           let p = core.cropPlacement(c, width: Int(ext.width),
                                      height: Int(ext.height)) {
            let canvas = CGRect(origin: .zero, size: p.canvas)
            img = img.transformed(by: p.transform)
                .composited(over: CIImage(color: CIColor(cgColor: p.pad))
                    .cropped(to: canvas))
                .cropped(to: canvas)
        }
        return img
    }
}

/// The keyframe controls for a clip: { previous key, set or unset a key
/// here, next key }, and where the clip is against its keys.
struct KeyframeBar: View {
    @Bindable var model: AppModel
    let track: KeyTrack

    var body: some View {
        // Keys count from the layer's own start (a still's: its pages).
        let f = model.keyFrame
        let keys = model.keyFrames(track)
        // The playhead off the layer: no key there -- the way back is a
        // key's arrow, the transport or the timeline.
        let off = model.layerOffStage
        // Locked: its keys are not changed either.
        let locked = model.selectedLayerLocked
        let here = off == nil && keys.contains(f)
        HStack(spacing: 4) {
            Button {
                model.goToKey(track, forward: false)
            } label: {
                Image(systemName: "chevron.backward").frame(width: 22)
            }
            .disabled(!keys.contains { $0 < f })
            .help("Previous keyframe")
            Button {
                model.toggleKey(track)
            } label: {
                Image(systemName: here ? "minus.diamond" : "plus.diamond")
                    .frame(width: 22)
            }
            .disabled(off != nil || locked || (here && keys.count == 1))
            .help(here ? "Remove the keyframe here" : "Add a keyframe here")
            Button {
                model.goToKey(track, forward: true)
            } label: {
                Image(systemName: "chevron.forward").frame(width: 22)
            }
            .disabled(!keys.contains { $0 > f })
            .help("Next keyframe")
            // A sound's keys go on in a straight line (the core's).
            if track != .sound { interpMenu }
            Image(systemName: here ? "diamond.fill" : "diamond")
                .font(.caption)
                .foregroundStyle(here ? Color.accentColor : .secondary)
                .padding(.leading, 6)
            if let off {
                Text(Self.offText(off, paged: model.pagedOnStage
                                      && !model.clipOnStage))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .help(off == .unselected
                          ? "Select a layer -- on the timeline or in Layers -- to change it"
                          : "Move the playhead onto the layer to change it")
            } else if locked {
                Label("Layer locked", systemImage: "lock.fill")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .help("Unlock the layer to change it")
            } else {
                Text(keyText(keys, here: here, frame: f))
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
        .buttonStyle(.borderless)
    }

    /// How the keyframe here -- or the one before -- goes on to the next:
    /// None (held), Linear, Spline; its picture the button.
    private var interpMenu: some View {
        let at = model.keyInterp(track)
        let now = at?.interp ?? .linear
        return Menu {
            ForEach(KeyInterp.allCases) { how in
                Toggle(isOn: Binding(
                    get: { now == how },
                    set: { if $0 { model.setKeyInterp(track, how) } })) {
                    Label {
                        Text(verbatim: how.label)
                    } icon: {
                        Image(nsImage: how.image)
                    }
                }
            }
        } label: {
            Image(nsImage: now.image)
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        .disabled(at == nil || model.selectedLayerLocked)
        .help(at == nil
              ? String(localized: "How a keyframe goes on to the next: move the playhead to a keyframe")
              : String(localized: "To the next keyframe -- \(now.help)"))
        .accessibilityLabel(Text("Interpolation"))
    }

    /// Where the stage is against the layer, when it is not on it.
    static func offText(_ edge: AppModel.ClipEdge,
                        paged: Bool) -> LocalizedStringKey {
        if edge == .unselected { return "No layer selected" }
        if paged { return "Not on this page" }
        return edge == .before ? "Before the layer starts"
                               : "After the layer ends"
    }

    /// "Keyframe 2 of 3 · frame 48", or "3 keyframes · frame 30" -- on a
    /// still with pages, "· page 4" (the page shown, from 1).
    private func keyText(_ keys: [Int], here: Bool, frame: Int) -> String {
        if model.pagedOnStage && !model.clipOnStage {
            let page = String(model.stagePage + 1)
            if here, let i = keys.firstIndex(of: frame) {
                let n = String(i + 1), of = String(keys.count)
                return String(localized: "Keyframe \(n) of \(of) · page \(page)")
            }
            let of = String(keys.count)
            return keys.count == 1
                ? String(localized: "1 keyframe · page \(page)")
                : String(localized: "\(of) keyframes · page \(page)")
        }
        let at = String(frame)
        if here, let i = keys.firstIndex(of: frame) {
            let n = String(i + 1), of = String(keys.count)
            return String(localized: "Keyframe \(n) of \(of) · frame \(at)")
        }
        let of = String(keys.count)
        return keys.count == 1
            ? String(localized: "1 keyframe · frame \(at)")
            : String(localized: "\(of) keyframes · frame \(at)")
    }
}
