import AppKit
import QuartzCore
import SwiftUI

/// The GUIDES (View › Show Guides): lines over what the stage shows -- a
/// picture, a composition, a clip, a generation's preview -- the thirds
/// until they are set by hand (GuideSet, the marks beside the stage:
/// GuideMarks). Each line is drawn as Lightroom draws its overlays, dark
/// grey, light grey, dark grey across, so it reads over any picture: a
/// light line alone vanishes in a sky, a dark one in a shadow.
enum Guides {
    static let dark = CGColor(gray: 0.12, alpha: 0.7)
    static let light = CGColor(gray: 0.86, alpha: 0.95)

    /// The light middle: a point wide, whole device pixels; the dark
    /// edges a device pixel more on either side.
    static func lightWidth(_ scale: CGFloat) -> CGFloat {
        max(1, scale.rounded()) / max(1, scale)
    }

    static func darkWidth(_ scale: CGFloat) -> CGFloat {
        (max(1, scale.rounded()) + 2) / max(1, scale)
    }

    /// The lines in `rect` (points) -- `at`'s fractions of it across and
    /// down -- each centred so its pixels are whole at `scale`; and the
    /// rectangle's edge, when `border`.
    static func path(in rect: CGRect, at f: GuideFractions = .thirds,
                     scale: CGFloat, border: Bool = false) -> CGPath {
        let s = max(1, scale)
        let px = max(1, s.rounded())
        // An odd width centres on a pixel's middle, an even one on its
        // edge.
        let half: CGFloat = px.truncatingRemainder(dividingBy: 2) == 1
            ? 0.5 : 0
        func snap(_ v: CGFloat) -> CGFloat {
            ((v * s).rounded(.down) + half) / s
        }
        let p = CGMutablePath()
        for fx in f.xs where (0...1).contains(fx) {
            let x = snap(rect.minX + rect.width * fx)
            p.move(to: CGPoint(x: x, y: rect.minY))
            p.addLine(to: CGPoint(x: x, y: rect.maxY))
        }
        // Down from the top: in a layer tree (y up) from its top edge.
        for fy in f.ys where (0...1).contains(fy) {
            let y = snap(f.flipped ? rect.maxY - rect.height * fy
                                   : rect.minY + rect.height * fy)
            p.move(to: CGPoint(x: rect.minX, y: y))
            p.addLine(to: CGPoint(x: rect.maxX, y: y))
        }
        if border {
            let inset = darkWidth(s) / 2
            p.addRect(rect.insetBy(dx: inset, dy: inset))
        }
        return p
    }
}

/// The guides in a layer tree (the stage's canvas): the dark edges, the
/// light middle over them -- where two lines cross, the light ones stay
/// whole.
@MainActor
final class GuideLayers {
    private let dark = CAShapeLayer()
    private let light = CAShapeLayer()

    init(in parent: CALayer) {
        for l in [dark, light] {
            l.fillColor = nil
            l.lineCap = .butt
            l.isHidden = true
            parent.addSublayer(l)
        }
        dark.strokeColor = Guides.dark
        light.strokeColor = Guides.light
    }

    /// Over `rect`, in the parent's coordinates (`bounds` its own); none
    /// hides them.
    func show(over rect: CGRect?, at f: GuideFractions = .thirds,
              in bounds: CGRect, scale: CGFloat, border: Bool) {
        guard let rect, rect.width >= 6, rect.height >= 6 else {
            dark.isHidden = true
            light.isHidden = true
            return
        }
        var f = f
        f.flipped = true
        let path = Guides.path(in: rect, at: f, scale: scale,
                               border: border)
        for l in [dark, light] {
            l.isHidden = false
            l.frame = bounds
            l.contentsScale = scale
            l.path = path
        }
        dark.lineWidth = Guides.darkWidth(scale)
        light.lineWidth = Guides.lightWidth(scale)
    }
}

/// The guides over a view that is the picture (a clip on the stage, its
/// preview).
struct GuidesGrid: View {
    var at: GuideFractions = .thirds
    @Environment(\.displayScale) private var scale

    var body: some View {
        Canvas { ctx, size in
            let path = Path(Guides.path(in: CGRect(origin: .zero,
                                                   size: size),
                                        at: at, scale: scale))
            ctx.stroke(path, with: .color(Color(cgColor: Guides.dark)),
                       lineWidth: Guides.darkWidth(scale))
            ctx.stroke(path, with: .color(Color(cgColor: Guides.light)),
                       lineWidth: Guides.lightWidth(scale))
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

/// Where the lines are, as fractions of what they go over: across (from
/// the left) and down (from the top).
struct GuideFractions: Equatable {
    var xs: [CGFloat]
    var ys: [CGFloat]
    /// Drawn in a layer tree, y up: down counted from its top.
    var flipped = false

    static let thirds = GuideFractions(xs: [1.0 / 3, 2.0 / 3],
                                       ys: [1.0 / 3, 2.0 / 3])
}

/// A stage's guides set by hand, in the pixels of what it shows:
/// horizontal ones `h` down from its top, vertical ones `v` across from
/// its left -- kept as typed, in the order made.
struct GuideSet: Equatable {
    var h: [Double]
    var v: [Double]

    static func thirds(_ s: CGSize) -> GuideSet {
        GuideSet(h: [s.height / 3, s.height * 2 / 3],
                 v: [s.width / 3, s.width * 2 / 3])
    }

    func fractions(_ s: CGSize) -> GuideFractions {
        GuideFractions(xs: v.map { CGFloat($0 / max(1, s.width)) },
                       ys: h.map { CGFloat($0 / max(1, s.height)) })
    }

    /// As the project's view state keeps it: {"h": [...], "v": [...]}.
    var json: [String: Any] { ["h": h, "v": v] }

    init(h: [Double], v: [Double]) {
        self.h = h
        self.v = v
    }

    init?(json: Any?) {
        guard let j = json as? [String: Any] else { return nil }
        func numbers(_ k: String) -> [Double] {
            (j[k] as? [Any] ?? []).compactMap { ($0 as? NSNumber)?.doubleValue }
        }
        h = numbers("h")
        v = numbers("v")
    }
}
