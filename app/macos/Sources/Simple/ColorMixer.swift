import AppKit
import CoreImage
import SwiftUI

// THE COLOR MIXER (DESIGN §10a Color): the tray's Color panel -- three
// PLANES, the colour wheel's hues down each and the amount across: the hue
// shift, the saturation and the luminance as CURVES over the wheel, each
// through control points a click adds, a drag moves and a double-click
// (or a drag off the plane) removes; at rest, a line down the middle. And
// its TARGET tool: a press on the stage reads the hue there and puts the
// points for it on the curves -- a drag across moves its saturation, up
// and down its luminance, up and down with ⌘ its hue. What the curves do
// is the core's (media/adjust.h ColorMix): their points ride on the
// layer's look beside the Adjust panel's values -- keyed with them on a
// clip, one track -- and are drawn by a Core Image kernel the core
// defines and this compiles (ColorMixKernel), as the engine compiles the
// same source.

// MARK: - The values

/// One of the seven hues a person names (core media::kMixHueBands): the
/// marks down a plane's hue axis.
enum MixBand: Int, CaseIterable, Identifiable, Sendable {
    case red, orange, yellow, green, aqua, blue, magenta
    var id: Int { rawValue }

    var label: String {
        switch self {
        case .red: String(localized: "Red")
        case .orange: String(localized: "Orange")
        case .yellow: String(localized: "Yellow")
        case .green: String(localized: "Green")
        case .aqua: String(localized: "Aqua")
        case .blue: String(localized: "Blue")
        case .magenta: String(localized: "Magenta")
        }
    }

    /// Its centre on the colour wheel, degrees (the core's).
    var degrees: Double { ColorMixKernel.degrees[rawValue] }

    /// The hue named nearest `hue`.
    static func nearest(_ hue: Double) -> MixBand {
        allCases.min {
            MixCurve.distance($0.degrees, hue) < MixCurve.distance($1.degrees, hue)
        } ?? .red
    }
}

/// The three curves, in the order their planes are shown.
enum MixChannel: String, CaseIterable, Identifiable, Sendable {
    case hue, saturation, luminance
    var id: String { rawValue }

    /// Its points' keys start so: "mix_hue_0", "mix_sat_0", "mix_lum_0".
    var key: String {
        switch self {
        case .hue: "hue"
        case .saturation: "sat"
        case .luminance: "lum"
        }
    }

    var label: String {
        switch self {
        case .hue: String(localized: "Hue")
        case .saturation: String(localized: "Saturation")
        case .luminance: String(localized: "Luminance")
        }
    }
}

/// A control point (core media::MixPoint).
struct MixPoint: Equatable, Sendable {
    /// Degrees, 0..<360.
    var hue: Double
    /// -1...1.
    var value: Double
    /// Given, a curve laid on another's points between two keys keeps the
    /// shape it had; nil -- every point a person places -- the curve's own.
    var slope: Double? = nil
}

/// A curve over the colour wheel (core media::MixCurve): through its
/// points, round the wheel from the last to the first, a cubic never past
/// the points either side; no point 0 everywhere, one its value
/// everywhere. The same maths as the core's, so what the plane draws and
/// a clip plays between its keys are what the engine draws.
struct MixCurve: Equatable, Sendable {
    /// By hue, ascending, none alike.
    var points: [MixPoint] = []

    /// 0 at every hue.
    var isIdentity: Bool {
        points.allSatisfy { $0.value == 0 && ($0.slope ?? 0) == 0 }
    }

    func value(at hue: Double) -> Double { Self.eval(knots, hue).value }
    func slope(at hue: Double) -> Double { Self.eval(knots, hue).slope }

    /// As a person leaves it: on the wheel, by hue, every slope its own.
    func placed() -> MixCurve {
        var c = self
        for i in c.points.indices { c.points[i].slope = nil }
        c.tidy()
        return c
    }

    /// The points on the wheel, by hue, none alike (the later of two
    /// kept), each value and slope in range -- the core's tidy.
    mutating func tidy() {
        let kept = points.enumerated().compactMap { i, p -> (Int, MixPoint)? in
            guard p.hue.isFinite, p.value.isFinite else { return nil }
            var q = p
            q.hue = Self.wrap(p.hue)
            q.value = min(1, max(-1, p.value))
            if let s = p.slope {
                q.slope = s.isFinite ? min(Self.maxSlope, max(-Self.maxSlope, s))
                    : nil
            }
            return (i, q)
        }
        .sorted { $0.1.hue != $1.1.hue ? $0.1.hue < $1.1.hue : $0.0 < $1.0 }
        var out: [MixPoint] = []
        for (_, p) in kept {
            if let last = out.last, p.hue - last.hue < 1e-9 {
                out[out.count - 1] = p
            } else {
                out.append(p)
            }
        }
        points = out
    }

    // The core's kMaxMixSlope.
    private static let maxSlope = 8.0

    static func wrap(_ h: Double) -> Double {
        var h = h.truncatingRemainder(dividingBy: 360)
        if h < 0 { h += 360 }
        return h >= 360 ? 0 : h
    }

    /// Degrees on round the wheel from one hue to the next: all the way
    /// round from a hue to itself.
    static func gap(_ from: Double, _ to: Double) -> Double {
        let g = to - from
        return g > 0 ? g : g + 360
    }

    /// Degrees between two hues, the short way.
    static func distance(_ a: Double, _ b: Double) -> Double {
        let d = abs(wrap(a) - wrap(b))
        return min(d, 360 - d)
    }

    struct Knot {
        var hue, value, slope: Double
    }

    /// Each point as the curve runs through it: its slope the one given,
    /// or the weighted harmonic mean of the slopes to the points either
    /// side, never steeper than three times the shallower, level where
    /// they turn (Fritsch and Butland's, as the core's).
    var knots: [Knot] {
        let p = points.map { MixPoint(hue: Self.wrap($0.hue), value: $0.value,
                                      slope: $0.slope) }
            .enumerated()
            .sorted { $0.1.hue != $1.1.hue ? $0.1.hue < $1.1.hue : $0.0 < $1.0 }
            .map(\.1)
        let n = p.count
        return p.indices.map { i in
            let prev = p[(i + n - 1) % n], next = p[(i + 1) % n]
            var m = 0.0
            if let s = p[i].slope {
                m = s
            } else {
                let h0 = Self.gap(prev.hue, p[i].hue)
                let h1 = Self.gap(p[i].hue, next.hue)
                let d0 = (p[i].value - prev.value) / h0
                let d1 = (next.value - p[i].value) / h1
                if d0 * d1 > 0 {
                    m = 3 * (h0 + h1) / ((2 * h1 + h0) / d0 + (h1 + 2 * h0) / d1)
                }
            }
            return Knot(hue: p[i].hue, value: p[i].value, slope: m)
        }
    }

    /// The curve at hue `x`: the cubic Hermite of the span it lies in, and
    /// its slope there.
    static func eval(_ k: [Knot], _ x: Double) -> (value: Double, slope: Double) {
        guard !k.isEmpty else { return (0, 0) }
        let x = wrap(x)
        let n = k.count
        var i = n - 1
        for j in 0..<n {
            if k[j].hue > x { break }
            i = j
        }
        let a = k[i], b = k[(i + 1) % n]
        let from = x < a.hue ? a.hue - 360 : a.hue
        let h = gap(a.hue, b.hue)
        let t = min(1, max(0, (x - from) / h))
        let t2 = t * t, t3 = t2 * t
        let slope = (6 * t2 - 6 * t) * (a.value - b.value) / h
            + (3 * t2 - 4 * t + 1) * a.slope + (3 * t2 - 2 * t) * b.slope
        let value = (2 * t3 - 3 * t2 + 1) * a.value
            + (t3 - 2 * t2 + t) * h * a.slope
            + (-2 * t3 + 3 * t2) * b.value + (t3 - t2) * h * b.slope
        return (value, slope)
    }

    /// Four curves made one (a key's lerp or spline, core media::zip_mix):
    /// laid on the points of all four, each value and slope there made by
    /// `f` from the four's.
    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        if p == a && a == b && b == n { return a }
        let all = [p, a, b, n]
        var unique: [Double] = []
        for h in all.flatMap({ $0.points.map { wrap($0.hue) } }).sorted()
        where unique.last.map({ h - $0 >= 1e-9 }) ?? true {
            unique.append(h)
        }
        let k = all.map(\.knots)
        var out = MixCurve()
        for h in unique {
            let e = k.map { eval($0, h) }
            out.points.append(MixPoint(
                hue: h, value: f(e[0].value, e[1].value, e[2].value, e[3].value),
                slope: f(e[0].slope, e[1].slope, e[2].slope, e[3].slope)))
        }
        out.tidy()
        return out
    }

    /// The target tool's: the point at `hue` -- the one within `snap`
    /// degrees, else one put there on the curve -- moved `by` from where it
    /// was, and points `reach` degrees either side, where none lies
    /// nearer, holding the rest of the curve where it is.
    func targeted(_ hue: Double, by: Double, snap: Double,
                  reach: Double) -> MixCurve {
        var pts = placed().points
        let hue = Self.wrap(hue)
        let near = pts.indices.min {
            Self.distance(pts[$0].hue, hue) < Self.distance(pts[$1].hue, hue)
        }
        let centre = near.flatMap {
            Self.distance(pts[$0].hue, hue) <= snap ? $0 : nil
        }
        let at = centre.map { pts[$0].hue } ?? hue
        let start = centre.map { pts[$0].value } ?? value(at: hue)
        let within = { (g: Double) in g > 1e-6 && g <= reach }
        var held: [MixPoint] = []
        if !pts.contains(where: { within(Self.gap(at, $0.hue)) }) {
            let h = Self.wrap(at + reach)
            held.append(MixPoint(hue: h, value: value(at: h)))
        }
        if !pts.contains(where: { within(Self.gap($0.hue, at)) }) {
            let h = Self.wrap(at - reach)
            held.append(MixPoint(hue: h, value: value(at: h)))
        }
        let v = min(1, max(-1, start + by))
        if let i = centre {
            pts[i].value = v
        } else {
            pts.append(MixPoint(hue: at, value: v))
        }
        var c = MixCurve(points: pts + held)
        c.tidy()
        return c
    }

    /// Every hue alike `by` -- a grey's, which has no hue: one point at
    /// red's where there is none.
    func shifted(by: Double) -> MixCurve {
        var c = placed()
        if c.points.isEmpty { c.points = [MixPoint(hue: 0, value: 0)] }
        for i in c.points.indices {
            c.points[i].value = min(1, max(-1, c.points[i].value + by))
        }
        return c
    }

    /// The hue a colour had before this curve -- the mixer's hue shift --
    /// turned it to `shown` (its saturation `saturation`: the turn fades
    /// out toward grey, as the kernel's): the target tool reads it, so a
    /// press on a colour the mixer moved finds its own points again. The
    /// nearest to `shown` of those that turn to it, to half a degree.
    func sourceHue(of shown: Double, saturation: Double) -> Double {
        guard !isIdentity else { return shown }
        let t = min(1, max(0, saturation / 0.15))
        let fade = t * t * (3 - 2 * t)
        let k = knots
        let turn = ColorMixKernel.hueDegrees * fade
        var best = shown, err = Double.infinity
        for i in 0..<720 {
            let h = Double(i) / 2
            let out = h + min(1, max(-1, Self.eval(k, h).value)) * turn
            let e = Self.distance(out, shown) + 1e-3 * Self.distance(h, shown)
            if e < err {
                err = e
                best = h
            }
        }
        return best
    }
}

/// The mixer's curves (core media::ColorMix).
struct ColorMix: Equatable, Sendable {
    var hue = MixCurve()
    var saturation = MixCurve()
    var luminance = MixCurve()

    /// Nothing moves (points may be there, every one at 0).
    var isIdentity: Bool {
        hue.isIdentity && saturation.isIdentity && luminance.isIdentity
    }
    /// No point on any curve: nothing for Reset.
    var isEmpty: Bool { self == ColorMix() }

    subscript(channel: MixChannel) -> MixCurve {
        get {
            switch channel {
            case .hue: hue
            case .saturation: saturation
            case .luminance: luminance
            }
        }
        set {
            switch channel {
            case .hue: hue = newValue
            case .saturation: saturation = newValue
            case .luminance: luminance = newValue
            }
        }
    }

    init() {}

    /// From a modifier's numbers: "mix_hue_0_at" a point's hue,
    /// "mix_hue_0" its value, "mix_hue_0_slope" a slope given -- and
    /// "hue@30", a point at that hue, as the core reads them.
    init(_ p: [String: Double]) {
        for c in MixChannel.allCases {
            var parts: [Int: (at: Double?, value: Double?, slope: Double?)] = [:]
            var loose: [MixPoint] = []
            let prefix = "mix_\(c.key)_"
            for (key, v) in p {
                if key.hasPrefix(c.key + "@"),
                   let h = Double(key.dropFirst(c.key.count + 1)) {
                    loose.append(MixPoint(hue: h, value: v))
                    continue
                }
                guard key.hasPrefix(prefix) else { continue }
                let rest = key.dropFirst(prefix.count)
                let digits = rest.prefix { $0.isNumber }
                guard let n = Int(digits) else { continue }
                var part = parts[n] ?? (nil, nil, nil)
                switch rest.dropFirst(digits.count) {
                case "": part.value = v
                case "_at": part.at = v
                case "_slope": part.slope = v
                default: continue
                }
                parts[n] = part
            }
            var curve = MixCurve()
            for n in parts.keys.sorted() {
                guard let part = parts[n], let at = part.at else { continue }
                curve.points.append(MixPoint(hue: at, value: part.value ?? 0,
                                             slope: part.slope))
            }
            curve.points += loose
            curve.tidy()
            self[c] = curve
        }
    }

    /// Every point, as the core reads them.
    var json: [String: Double] {
        var j: [String: Double] = [:]
        for c in MixChannel.allCases {
            for (i, p) in self[c].points.enumerated() {
                let k = "mix_\(c.key)_\(i)"
                j[k + "_at"] = p.hue
                j[k] = p.value
                if let s = p.slope { j[k + "_slope"] = s }
            }
        }
        return j
    }

    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        var o = ColorMix()
        for c in MixChannel.allCases {
            o[c] = MixCurve.zip(p[c], a[c], b[c], n[c], f)
        }
        return o
    }
}

// MARK: - The kernel

/// The mixer's Core Image kernel: the core's Metal source compiled once
/// (CIKernel's kernels(withMetalString:) -- no offline Metal compiler),
/// handed its curves as a table. Read from any thread: a clip's frames
/// are drawn on AVFoundation's.
enum ColorMixKernel {
    private final class Box: @unchecked Sendable {
        let lock = NSLock()
        var kernel: CIKernel?
        var filter = "ValtzColorMix"
        var degrees: [Double] = [0, 30, 60, 120, 180, 240, 300]
        var hueDegrees = 30.0
        var luminanceStops = 1.5
        var samples = 360
    }
    private static let box = Box()

    /// The core's description (CoreService.colorMix), taken once.
    static func install(_ info: CoreReply) {
        guard info.ok, let source = info["kernel"] as? String,
              let name = info["kernel_name"] as? String else { return }
        let kernel = (try? CIKernel.kernels(withMetalString: source))?
            .first { $0.name == name }
        let degrees = (info["bands"] as? [[String: Any]])?
            .compactMap { $0["degrees"] as? Double }
        box.lock.withLock {
            box.kernel = kernel
            if let f = info["filter"] as? String { box.filter = f }
            if let d = degrees, d.count == MixBand.allCases.count {
                box.degrees = d
            }
            if let h = info["hue_degrees"] as? Double { box.hueDegrees = h }
            if let l = info["luminance_stops"] as? Double {
                box.luminanceStops = l
            }
            if let s = info["samples"] as? Int, s > 0 { box.samples = s }
        }
    }

    /// Its step's name in a chain (media::kColorMixFilter).
    static var filter: String { box.lock.withLock { box.filter } }
    static var degrees: [Double] { box.lock.withLock { box.degrees } }
    static var hueDegrees: Double { box.lock.withLock { box.hueDegrees } }
    static var luminanceStops: Double {
        box.lock.withLock { box.luminanceStops }
    }

    /// `image` with the step's curves -- each sampled a degree at a time,
    /// made a table of (value + 1) / 2 in its red, green and blue, not
    /// colour managed, as the kernel reads it; as it was where the kernel
    /// is not here.
    nonisolated static func apply(_ image: CIImage,
                                  _ params: [String: [Double]]) -> CIImage {
        let (kernel, n) = box.lock.withLock { (box.kernel, box.samples + 1) }
        guard let kernel else { return image }
        var px = [Float](repeating: 0.5, count: 4 * n)
        for i in 0..<n { px[4 * i + 3] = 1 }
        for (c, name) in ["inputHue", "inputSaturation", "inputLuminance"]
            .enumerated() {
            for (i, v) in (params[name] ?? []).prefix(n).enumerated() {
                px[4 * i + c] = Float((v + 1) / 2)
            }
        }
        let data = px.withUnsafeBufferPointer { Data(buffer: $0) }
        let table = CIImage(bitmapData: data, bytesPerRow: 16 * n,
                            size: CGSize(width: n, height: 1),
                            format: .RGBAf, colorSpace: nil)
        let whole = table.extent
        return kernel.apply(extent: image.extent,
                            roiCallback: { i, r in i == 0 ? r : whole },
                            arguments: [image, table]) ?? image
    }
}

// MARK: - The model

/// The target tool's drag: where it began, the hue pressed on -- as the
/// picture had it before the mixer turned it; nil for a grey or a pixel
/// of no alpha -- the curves it began from, how far the pointer has gone
/// (points: across, up, and up with ⌘ held) and the curves it has begun
/// to move. On a clip the colour may come once its frame is drawn: until
/// then the drag is counted and waits (`weighed`).
struct ColorTargetDrag {
    var start: CGPoint
    var pixelsPerPoint: Double
    var from: ColorMix
    var last: CGPoint
    var hue: Double?
    var weighed = false
    /// Read from the colour drawn without the mixer.
    var unmixed = false
    var across = 0.0
    var up = 0.0
    var turn = 0.0
    var moving: Set<MixChannel> = []
}

extension AppModel {
    /// A drag this far (points) takes a point from 0 to +1.
    static let colorTargetPoints = 150.0
    /// Degrees: a point this near the hue pressed on is the one moved.
    static let colorTargetSnap = 4.0
    /// Degrees: where no point lies nearer, one this far either side holds
    /// the rest of the curve where it was.
    static let colorTargetReach = 30.0
    /// Points: a curve begins to move once the pointer has gone this far
    /// its way (a drag across leaves the luminance alone).
    static let colorTargetStart = 3.0

    /// The target tool takes the stage's pointer: on, its panel open, the
    /// selected layer there to change.
    var colorTargetReady: Bool {
        colorTargeting && openPanel == .color
            && canEditLayerHere && !isGenerating && stage.mode == .a
            && markupAsset != nil && !agentHolds
    }

    /// The target tool is on: the stage stays on the picture or clip it
    /// works on. A step elsewhere -- another asset, layer, page, task,
    /// the compare, a Start -- is refused, said in a notice that says how
    /// to go on.
    func refusedForColorTarget() -> Bool {
        guard colorTargeting, openPanel == .color else { return false }
        flash(String(localized: "The Color panel's target tool is on: turn it off first (Target, at the left of the Color panel) to look at anything else."), level: .error)
        return true
    }

    /// One curve, from its plane: on a still, its points; on a clip, the
    /// curve at the frame on screen (Keyframes.edit) -- as Adjust's.
    func setMix(_ c: MixChannel, _ curve: MixCurve) {
        guard adjustments.mix[c] != curve else { return }
        guard keyedStage else {
            adjustments.mix[c] = curve
            return
        }
        guard layerOffStage == nil else { return }
        clipAdjustKeys.edit(at: keyFrame) { $0.mix[c] = curve }
        clipTracksChanged()
    }

    /// The whole mixer at once: a target tool's step.
    func setMix(_ m: ColorMix) {
        guard keyedStage else {
            adjustments.mix = m
            return
        }
        guard layerOffStage == nil else { return }
        clipAdjustKeys.edit(at: keyFrame) { $0.mix = m }
        clipTracksChanged()
    }

    /// Reset: the mixer at rest -- `channel`'s curve, or all three; on a
    /// clip, in every key -- the Adjust panel's values kept.
    func resetColorMix(_ channel: MixChannel? = nil) {
        let rest = { (a: ImageAdjustments) -> ImageAdjustments in
            var o = a
            if let channel {
                o.mix[channel] = MixCurve()
            } else {
                o.mix = ColorMix()
            }
            return o
        }
        if keyedStage {
            clipAdjustKeys = clipAdjustKeys.collapsingValues(rest)
            clipTracksChanged()
            return
        }
        adjustments = rest(adjustments)
    }

    /// The mixer has points (Reset has work).
    var mixChanged: Bool {
        keyedStage
            ? clipAdjustKeys.keys.contains { !$0.value.mix.isEmpty }
            : !adjustments.mix.isEmpty
    }

    /// The target tool, on the stage's pointer (markup's canvas pixels, y
    /// down): pressed, the colour there read -- what the stage shows; a
    /// pixel of no alpha is a grey -- and its hue's points moved by the
    /// drag: across its saturation, up and down its luminance, up and down
    /// with ⌘ held its hue (⌘ taken at each step: let go, the drag goes
    /// on with the other two).
    func colorTargetPointer(_ e: MarkupPointer) {
        guard colorTargetReady else { return }
        switch e.phase {
        case .down:
            // On a clip: the frame pressed on, the player held there.
            if markupOnClip && videoRate != 0 { playVideo(rate: 0) }
            colorTargetDrag = ColorTargetDrag(
                start: e.point, pixelsPerPoint: max(0.0001, e.pixelsPerPoint),
                from: adjustments.mix, last: e.point)
            let p = e.point
            // Nothing mixed here: the colour shown is the picture's own.
            if adjustments.mix.isIdentity, let c = sampledColor(at: p) {
                weighColorTarget(c, unmixed: true)
                break
            }
            // The colour without the mixer drawn first (a clip's frame,
            // or the picture once more, small): the drag waits for it.
            // Where it cannot be drawn so, the colour shown, its hue read
            // back through the hue curve.
            Task { @MainActor [weak self] in
                guard let self else { return }
                var c = await self.unmixedColor(at: p)
                let unmixed = c != nil
                if c == nil {
                    c = self.markupOnClip ? await self.clipSample(at: p)
                        : self.sampledColor(at: p)
                }
                guard let c, self.colorTargetDrag?.start == p else { return }
                self.weighColorTarget(c, unmixed: unmixed)
                self.moveColorTarget()
            }
        case .drag, .up:
            if var d = colorTargetDrag {
                let dx = (e.point.x - d.last.x) / d.pixelsPerPoint
                let up = (d.last.y - e.point.y) / d.pixelsPerPoint
                if e.command {
                    d.turn += up
                } else {
                    d.across += dx
                    d.up += up
                }
                d.last = e.point
                colorTargetDrag = d
            }
            moveColorTarget()
            if e.phase == .up {
                colorTargetDrag = nil
                colorTargetHue = nil
            }
        case .hover, .exit:
            break
        }
    }

    /// The colour pressed on, read as its hue before the mixer (nil: a
    /// grey) -- the colour drawn without it (`unmixed`), else the one shown
    /// read back through the hue curve -- marked on the planes while the
    /// drag lasts.
    private func weighColorTarget(_ c: RGBA, unmixed: Bool) {
        guard let d = colorTargetDrag else { return }
        var hue: Double?
        if c.a > 0, let r = core?.colorMixHue(r: c.r, g: c.g, b: c.b) {
            hue = unmixed ? r.hue
                : d.from.hue.sourceHue(of: r.hue, saturation: r.saturation)
        }
        colorTargetDrag?.hue = hue
        colorTargetDrag?.weighed = true
        colorTargetDrag?.unmixed = unmixed
        colorTargetHue = hue
    }

    /// The drag so far, made the curves: each moved once the pointer has
    /// gone its way, from the curves as the press found them.
    private func moveColorTarget() {
        guard var d = colorTargetDrag, d.weighed else { return }
        let by: [MixChannel: Double] = [
            .hue: d.turn, .saturation: d.across, .luminance: d.up,
        ]
        for (c, v) in by where abs(v) >= Self.colorTargetStart {
            d.moving.insert(c)
        }
        // A grey has no hue to turn.
        if d.hue == nil { d.moving.remove(.hue) }
        colorTargetDrag = d
        var m = d.from
        for c in d.moving {
            let v = (by[c] ?? 0) / Self.colorTargetPoints
            m[c] = d.hue.map {
                d.from[c].targeted($0, by: v, snap: Self.colorTargetSnap,
                                   reach: Self.colorTargetReach)
            } ?? d.from[c].shifted(by: v)
        }
        guard m != adjustments.mix else { return }
        setMix(m)
    }
}

// MARK: - The panel

/// The Color panel: the keyframes (a clip) or a word on what it does,
/// Bypass, Reset; the target tool and how it goes; then the three planes.
struct ColorPanel: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                if model.keyedStage {
                    // The look's keys: the Adjust panel's too.
                    KeyframeBar(model: model, track: .adjust)
                } else {
                    Text("Each hue's own hue, saturation and luminance, as curves round the colour wheel.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                MixBypassButton(model: model)
                Button("Reset") { model.resetColorMix() }
                    .disabled(!model.mixChanged
                              || (model.keyedStage
                                  && model.layerOffStage != nil)
                              || model.selectedLayerLocked)
            }
            HStack(spacing: 10) {
                // The target tool first, where it is seen.
                ColorTargetButton(model: model)
                Text(model.colorTargeting
                     ? "On the stage, drag from a colour: across for its saturation, up or down for its luminance, ⌘ and up or down for its hue."
                     : "Click a disk to add a point at that hue, then drag it: out or in for its value, round for its hue (⌥ finer). Double-click a point, or drag it off the disk, to remove it.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(2)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 0)
            }
            HStack(alignment: .top, spacing: 12) {
                ForEach(MixChannel.allCases) { c in
                    MixDiskColumn(model: model, channel: c)
                }
            }
            // Greyed while the playhead is off the selected layer -- its
            // values would change unseen -- or it is locked.
            .disabled(!model.canEditLayerHere)
        }
        .controlSize(.small)
    }

    /// A curve's value as the panel says it: a hue shift in degrees, the
    /// rest -100...+100.
    static func display(_ c: MixChannel, _ v: Double) -> String {
        if c == .hue {
            let d = Int((v * ColorMixKernel.hueDegrees).rounded())
            return d > 0 ? "+\(d)°" : "\(d)°"
        }
        let n = Int((v * 100).rounded())
        return n > 0 ? "+\(n)" : "\(n)"
    }
}

/// What a plane's header reads: the hue under the pointer -- or of the
/// point dragged -- and the curve's value there.
struct MixReading: Equatable {
    var hue: Double
    var value: Double
}

/// A disk with its name, what is under the pointer, and what inward and
/// outward do.
private struct MixDiskColumn: View {
    @Bindable var model: AppModel
    let channel: MixChannel
    @State private var reading: MixReading?
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        VStack(spacing: 4) {
            HStack(spacing: 4) {
                Text(verbatim: channel.label)
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                Spacer(minLength: 4)
                if let r = reading {
                    Text(verbatim: "\(MixBand.nearest(r.hue).label) \(Int(r.hue.rounded()) % 360)° · \(ColorPanel.display(channel, r.value))")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }
            .frame(width: Self.side)
            MixDisk(channel: channel, curve: model.adjustments.mix[channel],
                    marked: model.colorTargetHue, enabled: enabled,
                    onChange: { model.setMix(channel, $0) },
                    onReset: { model.resetColorMix(channel) },
                    onReading: { reading = $0 })
                .frame(width: Self.side, height: Self.side)
                .accessibilityLabel(Text(verbatim: channel.label))
            HStack {
                Text(verbatim: String(localized: "In: \(ends.0)"))
                Spacer()
                Text(verbatim: String(localized: "Out: \(ends.1)"))
            }
            .font(.caption2)
            .foregroundStyle(.tertiary)
            .frame(width: Self.side)
        }
        .frame(maxWidth: .infinity)
        .opacity(enabled ? 1 : 0.55)
    }

    static let side: CGFloat = 168

    /// What inward (-1 at its inner reach) and outward (+1 at the rim) do.
    private var ends: (String, String) {
        switch channel {
        case .hue:
            let d = Int(ColorMixKernel.hueDegrees.rounded())
            return ("−\(d)°", "+\(d)°")
        case .saturation:
            return (String(localized: "Less"), String(localized: "More"))
        case .luminance:
            return (String(localized: "Darker"), String(localized: "Lighter"))
        }
    }
}

/// Bypass for the mixer: held, the stage without it.
private struct MixBypassButton: View {
    @Bindable var model: AppModel
    @State private var held = false

    var body: some View {
        Text("Bypass")
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .foregroundStyle(held ? Color.white : Color.primary)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(held ? AnyShapeStyle(Color.accentColor)
                           : AnyShapeStyle(.quaternary)))
            .opacity(model.canBypassMix ? 1 : 0.45)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !held, model.canBypassMix else { return }
                    held = true
                    model.setMixBypass(true)
                }
                .onEnded { _ in
                    held = false
                    model.setMixBypass(false)
                })
            .onDisappear {
                if held { model.setMixBypass(false) }
            }
            .help("Hold to see the picture without its colour mixer")
            .accessibilityAddTraits(.isButton)
    }
}

/// The target tool's switch.
private struct ColorTargetButton: View {
    @Bindable var model: AppModel

    var body: some View {
        let on = model.colorTargeting
        // Its own look, not a plain toggle's: the accent's fill while on
        // -- the stage is held to its picture then -- an outline of it
        // while off.
        Button {
            model.colorTargeting.toggle()
        } label: {
            Label("Target", systemImage: "scope")
                .font(.callout.weight(.semibold))
                .padding(.horizontal, 9)
                .padding(.vertical, 3)
                .foregroundStyle(on ? Color.white : Color.accentColor)
                .background(RoundedRectangle(cornerRadius: 6,
                                             style: .continuous)
                    .fill(on ? AnyShapeStyle(Color.accentColor)
                             : AnyShapeStyle(Color.accentColor.opacity(0.12))))
                .overlay(RoundedRectangle(cornerRadius: 6,
                                          style: .continuous)
                    .strokeBorder(Color.accentColor.opacity(on ? 0 : 0.6)))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .fixedSize()
        .accessibilityAddTraits(on ? .isSelected : [])
        .help("Target: press on a colour on the stage and drag — across for its saturation, up or down for its luminance, with ⌘ up or down for its hue")
        .accessibilityLabel(Text("Target Tool"))
    }
}

// MARK: - A disk

/// One of the Color panel's disks, an AppKit view (its own mouse
/// handling: a click, a drag, a double-click, a right-click's menu).
struct MixDisk: NSViewRepresentable {
    let channel: MixChannel
    let curve: MixCurve
    /// The target tool's hue, a line out from the centre while it drags.
    let marked: Double?
    let enabled: Bool
    let onChange: (MixCurve) -> Void
    let onReset: () -> Void
    let onReading: (MixReading?) -> Void

    func makeNSView(context: Context) -> MixDiskNSView {
        let v = MixDiskNSView(channel: channel)
        update(v)
        return v
    }

    func updateNSView(_ v: MixDiskNSView, context: Context) { update(v) }

    private func update(_ v: MixDiskNSView) {
        v.onChange = onChange
        v.onReset = onReset
        v.onReading = onReading
        v.show(curve, marked: marked, enabled: enabled)
    }
}

/// A disk: the colour wheel's hues ROUND it -- red at the right, then
/// yellow, green, aqua, blue, magenta counter-clockwise, as the Grade
/// panel's wheels -- and the curve's value OUT from its centre: 0 on the
/// circle at `base` of its radius, +1 at its rim, -1 as far inside
/// (`base` less as much); its ground what the curve does there. The hue
/// shift's is a RING of that reach (a turned hue has no centre); the
/// saturation's and the luminance's run on to their centres -- as grey,
/// as dark as -1 makes them. The curve is a closed line round it, at
/// rest the circle; no seam where the wheel comes round.
final class MixDiskNSView: NSView {
    /// Of the radius: where 0 is, and how far +1 and -1 go either way.
    static let base = 0.75
    static let reach = 0.25
    /// Points: the disk this far in from the view's edges, a point's
    /// reach for a press, and how far past the rim a point dragged goes
    /// off when let go.
    private static let inset: CGFloat = 3
    private static let grab: CGFloat = 7
    private static let off: CGFloat = 24

    let channel: MixChannel
    var onChange: ((MixCurve) -> Void)?
    var onReset: (() -> Void)?
    var onReading: ((MixReading?) -> Void)?
    private(set) var curve = MixCurve()
    private var marked: Double?
    private var enabled = true

    /// A point held: its place in `curve` (kept unsorted while dragged),
    /// where it was as it was pressed, the pointer's last angle, how far
    /// the pointer has gone round and out (with ⌥ a tenth as far), how
    /// far round it may go (to a degree of its neighbours), and whether
    /// it is off the disk.
    private struct Held {
        var index: Int
        var from: MixPoint
        var angle: Double
        var radius: CGFloat
        var turned = 0.0
        var out = 0.0
        var lower: Double
        var upper: Double
        var off = false
    }
    private var held: Held?

    /// The disks shown, by channel: the snapshot hooks post their events
    /// to them.
    private static let shown = NSMapTable<NSString, MixDiskNSView>
        .strongToWeakObjects()
    static func view(_ c: MixChannel) -> MixDiskNSView? {
        shown.object(forKey: c.rawValue as NSString)
    }

    init(channel: MixChannel) {
        self.channel = channel
        super.init(frame: .zero)
        toolTip = String(localized: "Click to add a point at that hue and drag it -- out or in for its value, round for its hue, ⌥ for finer steps; double-click a point, or drag it off the disk, to remove it")
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil {
            Self.shown.setObject(self, forKey: channel.rawValue as NSString)
        }
    }

    /// The model's curve -- not while a point is held: the drag's own is
    /// shown until it is let go.
    func show(_ c: MixCurve, marked m: Double?, enabled e: Bool) {
        let changed = (held == nil && c != curve) || m != marked || e != enabled
        if held == nil { curve = c }
        marked = m
        enabled = e
        setAccessibilityValue(String(localized: "\(curve.points.count) points"))
        if changed { needsDisplay = true }
    }

    // MARK: Where things are

    private var centre: CGPoint { CGPoint(x: bounds.midX, y: bounds.midY) }
    private var radius: CGFloat {
        max(1, min(bounds.width, bounds.height) / 2 - Self.inset)
    }

    /// A value's distance out.
    private func r(_ v: Double) -> CGFloat {
        radius * CGFloat(Self.base + Self.reach * min(1, max(-1, v)))
    }

    /// The point at a hue and a value.
    private func place(_ hue: Double, _ v: Double) -> CGPoint {
        let a = hue * .pi / 180, d = r(v)
        return CGPoint(x: centre.x + d * CGFloat(cos(a)),
                       y: centre.y - d * CGFloat(sin(a)))
    }

    /// The hue at a point (its angle).
    private func hue(at p: CGPoint) -> Double {
        MixCurve.wrap(atan2(Double(centre.y - p.y), Double(p.x - centre.x))
                      * 180 / .pi)
    }

    private func distance(_ p: CGPoint) -> CGFloat {
        hypot(p.x - centre.x, p.y - centre.y)
    }

    /// The point nearest `p` within reach.
    private func point(near p: CGPoint) -> Int? {
        curve.points.indices.map { i -> (Int, CGFloat) in
            let q = place(curve.points[i].hue, curve.points[i].value)
            return (i, hypot(q.x - p.x, q.y - p.y))
        }
        .filter { $0.1 <= Self.grab }
        .min { $0.1 < $1.1 }?.0
    }

    /// The curve as drawn: without a point held off the disk.
    private var drawn: MixCurve {
        guard let h = held, h.off else { return curve }
        var c = curve
        c.points.remove(at: h.index)
        return c
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let c = centre, rad = radius
        let disk = CGRect(x: c.x - rad, y: c.y - rad, width: 2 * rad,
                          height: 2 * rad)
        let ring = channel == .hue
        let inner = r(-1)
        g.saveGState()
        g.addEllipse(in: disk)
        if ring {
            g.addEllipse(in: disk.insetBy(dx: rad - inner, dy: rad - inner))
            g.clip(using: .evenOdd)
        } else {
            g.clip()
        }
        if let art = MixDiskArt.image(channel) {
            // Its first row at the top: the view is flipped.
            g.saveGState()
            g.translateBy(x: 0, y: disk.maxY + disk.minY)
            g.scaleBy(x: 1, y: -1)
            g.interpolationQuality = .high
            g.draw(art, in: disk)
            g.restoreGState()
        }
        // A shade over it, so the curve reads on every hue.
        g.setFillColor(NSColor.black.withAlphaComponent(0.18).cgColor)
        g.fill(disk)
        g.restoreGState()

        // The hues named: ticks in from the rim.
        g.setStrokeColor(NSColor.white.withAlphaComponent(0.7).cgColor)
        g.setLineWidth(1)
        for b in MixBand.allCases {
            let a = b.degrees * .pi / 180
            let u = CGPoint(x: CGFloat(cos(a)), y: -CGFloat(sin(a)))
            g.strokeLineSegments(between: [
                CGPoint(x: c.x + u.x * (rad - 5), y: c.y + u.y * (rad - 5)),
                CGPoint(x: c.x + u.x * rad, y: c.y + u.y * rad),
            ])
        }
        // Nothing changed: the circle at its base.
        let b0 = r(0)
        g.setStrokeColor(NSColor.white.withAlphaComponent(0.4).cgColor)
        g.setLineDash(phase: 0, lengths: [2, 3])
        g.strokeEllipse(in: CGRect(x: c.x - b0, y: c.y - b0, width: 2 * b0,
                                   height: 2 * b0))
        // The target tool's hue, out across the reach.
        if let m = marked {
            g.setStrokeColor(NSColor.white.withAlphaComponent(0.9).cgColor)
            g.setLineDash(phase: 0, lengths: [4, 3])
            g.strokeLineSegments(between: [place(m, -1), place(m, 1)])
        }
        g.setLineDash(phase: 0, lengths: [])

        // The curve, round the disk a degree at a time.
        let k = drawn.knots
        let path = CGMutablePath()
        for d in 0...360 {
            let h = Double(d)
            let p = place(h, MixCurve.eval(k, h).value)
            if d == 0 { path.move(to: p) } else { path.addLine(to: p) }
        }
        path.closeSubpath()
        g.setLineJoin(.round)
        g.addPath(path)
        g.setStrokeColor(NSColor.black.withAlphaComponent(0.5).cgColor)
        g.setLineWidth(3.2)
        g.strokePath()
        g.addPath(path)
        g.setStrokeColor(NSColor.white.cgColor)
        g.setLineWidth(1.6)
        g.strokePath()

        // Its points; the one held in the accent, faded off the disk.
        for (i, p) in curve.points.enumerated() {
            let isHeld = held?.index == i
            let at = place(p.hue, p.value)
            let dot = CGRect(x: at.x - 4.5, y: at.y - 4.5, width: 9, height: 9)
            g.setAlpha(isHeld && held?.off == true ? 0.35 : 1)
            g.setFillColor(isHeld ? NSColor.controlAccentColor.cgColor
                                  : NSColor.white.cgColor)
            g.fillEllipse(in: dot)
            g.setStrokeColor(NSColor.black.withAlphaComponent(0.55).cgColor)
            g.setLineWidth(1)
            g.strokeEllipse(in: dot)
            g.setAlpha(1)
        }
        g.setStrokeColor(NSColor.separatorColor.cgColor)
        g.setLineWidth(1)
        g.strokeEllipse(in: disk.insetBy(dx: 0.5, dy: 0.5))
        if ring {
            g.strokeEllipse(in: disk.insetBy(dx: rad - inner + 0.5,
                                             dy: rad - inner + 0.5))
        }
    }

    // MARK: The pointer

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        for t in trackingAreas { removeTrackingArea(t) }
        addTrackingArea(NSTrackingArea(
            rect: .zero,
            options: [.mouseMoved, .mouseEnteredAndExited, .activeAlways,
                      .inVisibleRect],
            owner: self))
    }

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: enabled ? .crosshair : .arrow)
    }

    override func mouseMoved(with e: NSEvent) {
        guard held == nil else { return }
        let p = convert(e.locationInWindow, from: nil)
        guard distance(p) <= radius else {
            onReading?(nil)
            return
        }
        let h = hue(at: p)
        onReading?(MixReading(hue: h, value: curve.value(at: h)))
    }

    override func mouseExited(with e: NSEvent) {
        if held == nil { onReading?(nil) }
    }

    override func mouseDown(with e: NSEvent) {
        guard enabled else { return }
        let p = convert(e.locationInWindow, from: nil)
        let hit = point(near: p)
        // On the disk, or a point just past its rim.
        guard hit != nil || distance(p) <= radius else { return }
        if MarkupPointer.clicks(e) >= 2 {
            // A double-click on a point takes it off.
            if let i = hit { remove(i) }
            return
        }
        var c = curve.placed()
        let index: Int
        if let i = hit {
            index = i
        } else {
            // On the curve at that hue: nothing moves until it is
            // dragged.
            let h = hue(at: p)
            c.points.append(MixPoint(hue: h, value: curve.value(at: h)))
            c.tidy()
            index = c.points.firstIndex {
                abs($0.hue - MixCurve.wrap(h)) < 1e-9
            } ?? 0
        }
        curve = c
        // How far round it may go: to a degree of the points either side.
        let h0 = c.points[index].hue
        let others = c.points.indices.filter { $0 != index }
            .map { c.points[$0].hue }
        var lower = -Double.infinity, upper = Double.infinity
        if !others.isEmpty {
            let back = others.map { MixCurve.gap($0, h0) }.min() ?? 360
            let on = others.map { MixCurve.gap(h0, $0) }.min() ?? 360
            lower = -max(0, back - 1)
            upper = max(0, on - 1)
        }
        held = Held(index: index, from: c.points[index], angle: hue(at: p),
                    radius: distance(p), lower: lower, upper: upper)
        // A click puts a point there.
        if hit == nil { onChange?(c) }
        onReading?(MixReading(hue: h0, value: c.points[index].value))
        needsDisplay = true
    }

    override func mouseDragged(with e: NSEvent) {
        guard var h = held else { return }
        let p = convert(e.locationInWindow, from: nil)
        let fine = e.modifierFlags.contains(.option) ? 0.1 : 1.0
        // Round: the step since the last, the short way.
        let a = hue(at: p)
        var step = a - h.angle
        if step > 180 { step -= 360 }
        if step < -180 { step += 360 }
        h.turned += step * fine
        h.angle = a
        // Out: the step since the last, in values.
        let d = distance(p)
        h.out += Double((d - h.radius) / (radius * CGFloat(Self.reach)))
            * fine
        h.radius = d
        h.off = d > radius + Self.off
        held = h
        let hueNow = MixCurve.wrap(h.from.hue
                                   + min(h.upper, max(h.lower, h.turned)))
        let v = min(1, max(-1, h.from.value + h.out))
        curve.points[h.index] = MixPoint(hue: hueNow, value: v)
        var out = drawn
        out.tidy()
        onChange?(out)
        onReading?(h.off ? nil : MixReading(hue: hueNow, value: v))
        needsDisplay = true
    }

    override func mouseUp(with e: NSEvent) {
        guard let h = held else { return }
        held = nil
        var c = curve
        if h.off { c.points.remove(at: h.index) }
        c.tidy()
        curve = c
        onChange?(c)
        needsDisplay = true
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        guard enabled else { return nil }
        let m = NSMenu()
        if let i = point(near: convert(event.locationInWindow, from: nil)) {
            let item = NSMenuItem(title: String(localized: "Remove Point"),
                                  action: #selector(removeChosen(_:)),
                                  keyEquivalent: "")
            item.target = self
            item.tag = i
            m.addItem(item)
        }
        let reset = NSMenuItem(
            title: String(localized: "Reset \(channel.label)"),
            action: curve.points.isEmpty ? nil : #selector(resetChosen),
            keyEquivalent: "")
        reset.target = self
        m.addItem(reset)
        return m
    }

    @objc private func removeChosen(_ item: NSMenuItem) { remove(item.tag) }

    @objc private func resetChosen() { onReset?() }

    private func remove(_ i: Int) {
        guard curve.points.indices.contains(i) else { return }
        var c = curve.placed()
        c.points.remove(at: i)
        curve = c
        onChange?(c)
        onReading?(nil)
        needsDisplay = true
    }
}

/// A disk's ground: round it each hue, out from its base circle what the
/// curve's value there does to that hue -- the hue's a ring, the others
/// on to the centre at -1 -- made once a channel, drawn scaled.
enum MixDiskArt {
    @MainActor private static var made: [MixChannel: CGImage] = [:]

    @MainActor
    static func image(_ c: MixChannel) -> CGImage? {
        if let i = made[c] { return i }
        let n = 192
        var px = [UInt8](repeating: 0, count: n * n * 4)
        let turn = ColorMixKernel.hueDegrees
        let stops = ColorMixKernel.luminanceStops
        let base = MixDiskNSView.base, reach = MixDiskNSView.reach
        for j in 0..<n {
            for i in 0..<n {
                let x = (Double(i) + 0.5) / Double(n) * 2 - 1
                // Its first row at the top: y up from the centre.
                let y = 1 - (Double(j) + 0.5) / Double(n) * 2
                let rr = hypot(x, y)
                // Softened over its last pixel, and the ring's inside.
                var alpha = min(1, max(0, (1 - rr) * Double(n) / 2 + 0.5))
                if c == .hue {
                    alpha *= min(1, max(0, (rr - (base - reach))
                                            * Double(n) / 2 + 0.5))
                }
                guard alpha > 0 else { continue }
                let hue = MixCurve.wrap(atan2(y, x) * 180 / .pi)
                let u = min(1, max(-1, (rr - base) / reach))
                let rgb: (Double, Double, Double)
                switch c {
                case .hue:
                    rgb = hsv(hue + u * turn, 0.7, 0.9)
                case .saturation:
                    let b = hsv(hue, 0.7, 0.9)
                    let l = (linear(b.0), linear(b.1), linear(b.2))
                    let lum = 0.2126 * l.0 + 0.7152 * l.1 + 0.0722 * l.2
                    let k = max(0, 1 + u)
                    rgb = (encoded(lum + (l.0 - lum) * k),
                           encoded(lum + (l.1 - lum) * k),
                           encoded(lum + (l.2 - lum) * k))
                case .luminance:
                    let b = hsv(hue, 0.7, 0.9)
                    let k = pow(2, stops * u)
                    rgb = (encoded(linear(b.0) * k),
                           encoded(linear(b.1) * k),
                           encoded(linear(b.2) * k))
                }
                let o = (j * n + i) * 4
                px[o] = UInt8((rgb.0 * alpha * 255).rounded())
                px[o + 1] = UInt8((rgb.1 * alpha * 255).rounded())
                px[o + 2] = UInt8((rgb.2 * alpha * 255).rounded())
                px[o + 3] = UInt8((alpha * 255).rounded())
            }
        }
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let provider = CGDataProvider(data: Data(px) as CFData),
              let img = CGImage(
                width: n, height: n, bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: n * 4, space: space,
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo
                    .premultipliedLast.rawValue),
                provider: provider, decode: nil, shouldInterpolate: true,
                intent: .defaultIntent) else { return nil }
        made[c] = img
        return img
    }

    /// HSV to sRGB as encoded, 0...1.
    private static func hsv(_ degrees: Double, _ s: Double,
                            _ v: Double) -> (Double, Double, Double) {
        let h = MixCurve.wrap(degrees) / 60
        let f = { (n: Double) -> Double in
            let k = (n + h).truncatingRemainder(dividingBy: 6)
            return v - v * s * max(0, min(k, 4 - k, 1))
        }
        return (f(5), f(3), f(1))
    }

    private static func linear(_ c: Double) -> Double {
        c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4)
    }

    private static func encoded(_ c: Double) -> Double {
        let c = min(1, max(0, c))
        return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1 / 2.4) - 0.055
    }
}
