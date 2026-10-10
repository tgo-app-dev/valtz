import AVFoundation
import CoreImage
import Foundation
import SwiftUI

/// A value a clip's keyframes can carry: interpolated linearly between two
/// keys (core media/keyframes.h).
protocol Keyable: Equatable, Sendable {
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self
    var isIdentity: Bool { get }
}

/// A value that changes over a clip -- its adjustments, its crop: keys at
/// frames, and between two of them the values interpolated linearly;
/// before the first key, the first, after the last, the last. A track of
/// one key is a value for the whole clip, which is how a clip starts (a
/// key at its first frame). Recorded on the clip as its "adjust" / "crop"
/// modifier ({"keys": [{"frame", ...}], "rate_num", "rate_den"}).
struct Keyframes<T: Keyable>: Equatable, Sendable {
    struct Key: Equatable, Sendable {
        var frame: Int
        var value: T
    }

    /// Ascending frames, no two alike.
    private(set) var keys: [Key] = []

    init() {}

    /// One key at the first frame.
    init(start value: T) {
        keys = [Key(frame: 0, value: value)]
    }

    /// From a modifier's keys, each with its "frame" (none: 0).
    init(_ dicts: [[String: Double]], value: ([String: Double]) -> T) {
        for d in dicts {
            set(value(d), at: max(0, Int(d["frame"] ?? 0)))
        }
    }

    /// As the core reads a track; nothing when every key is the identity.
    func json(rate: FrameRate,
              value: (T) -> [String: Double]) -> [String: Any] {
        guard !isIdentity else { return [:] }
        return [
            "keys": keys.map { k -> [String: Double] in
                var d = value(k.value)
                d["frame"] = Double(k.frame)
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
        return T.lerp(ka.value, kb.value,
                      (frame - Double(ka.frame))
                          / Double(kb.frame - ka.frame))
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

    /// Put `value` at `frame`: that key's, or a new key there.
    mutating func set(_ value: T, at frame: Int) {
        if let i = index(at: frame) {
            keys[i].value = value
        } else {
            keys.append(Key(frame: frame, value: value))
            keys.sort { $0.frame < $1.frame }
        }
    }

    mutating func remove(at frame: Int) {
        keys.removeAll { $0.frame == frame }
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
        var o = a
        for k in Key.allCases { o[k] = a[k] + (b[k] - a[k]) * t }
        return o
    }
}

extension CropSpec: Keyable {
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self {
        func mix(_ x: Double, _ y: Double) -> Double { x + (y - x) * t }
        var o = a.contentWidth > 0 ? a : b
        o.scaleX = mix(a.scaleX, b.scaleX)
        o.scaleY = mix(a.scaleY, b.scaleY)
        o.offsetX = mix(a.offsetX, b.offsetX)
        o.offsetY = mix(a.offsetY, b.offsetY)
        o.rotate = mix(a.rotate, b.rotate)
        o.pad = zip(a.pad, b.pad).map { mix($0, $1) }
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
}

/// A composition layer's SOUND (core media::Sound): its gain -- 0 mute, 1
/// as it is, up to 4 (+12 dB) -- and its pitch in semitones.
struct LayerSound: Keyable {
    var volume = 1.0
    var pitch = 0.0
    static let volumeRange = 0.0...4.0
    static let pitchRange = -24.0...24.0
    var isIdentity: Bool { volume == 1 && pitch == 0 }
    static func lerp(_ a: Self, _ b: Self, _ t: Double) -> Self {
        LayerSound(volume: a.volume + (b.volume - a.volume) * t,
                   pitch: a.pitch + (b.pitch - a.pitch) * t)
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
}

/// Which track a key control works on: a clip's adjustments, where its
/// frames lie (offsets and scales), or how they are turned.
/// A clip's tracks -- and the parts of any picture's look a held Bypass
/// leaves out of what the stage shows.
enum KeyTrack: Hashable, Sendable {
    case adjust, place, turn
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
        place = Keyframes(keys) { d in
            var c = CropSpec(d)
            c.rotate = 0
            return c
        }
        turn = Keyframes(m.rotateKeys ?? keys) {
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
            "keys": place.keys.map { k -> [String: Double] in
                let c = k.value
                var d = ["frame": Double(k.frame),
                         "content_w": Double(c.contentWidth),
                         "content_h": Double(c.contentHeight),
                         "scale_x": c.scaleX, "scale_y": c.scaleY,
                         "offset_x": c.offsetX, "offset_y": c.offsetY]
                // A still's crop may name its canvas (a page's look).
                if c.canvasWidth > 0, c.canvasHeight > 0 {
                    d["canvas_w"] = Double(c.canvasWidth)
                    d["canvas_h"] = Double(c.canvasHeight)
                }
                return d
            },
            "rotate_keys": turn.keys.map {
                ["frame": Double($0.frame), "rotate": $0.value.degrees]
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

    /// `image` (a frame, extent at the origin) with the look at `frame`.
    func apply(_ image: CIImage, frame: Int, core: CoreService) -> CIImage {
        var img = image
        if let a = adjust.value(at: frame), !a.isIdentity {
            img = ImageAdjustments.applyChain(core.adjustmentChain(a), to: img)
        }
        if case let c = crop.value(at: frame), !c.isIdentity,
           let p = core.cropPlacement(c, width: Int(image.extent.width),
                                      height: Int(image.extent.height)) {
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
            .disabled(off != nil || (here && keys.count == 1))
            .help(here ? "Remove the keyframe here" : "Add a keyframe here")
            Button {
                model.goToKey(track, forward: true)
            } label: {
                Image(systemName: "chevron.forward").frame(width: 22)
            }
            .disabled(!keys.contains { $0 > f })
            .help("Next keyframe")
            Image(systemName: here ? "diamond.fill" : "diamond")
                .font(.caption)
                .foregroundStyle(here ? Color.accentColor : .secondary)
                .padding(.leading, 6)
            if let off {
                Text(Self.offText(off, paged: model.pagedOnStage
                                      && !model.clipOnStage))
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .help("Move the playhead onto the layer to change it")
            } else {
                Text(keyText(keys, here: here, frame: f))
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
            }
        }
        .buttonStyle(.borderless)
    }

    /// Where the stage is against the layer, when it is not on it.
    static func offText(_ edge: AppModel.ClipEdge,
                        paged: Bool) -> LocalizedStringKey {
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
