import AppKit
import QuartzCore
import SwiftUI

/// The GUIDES (View › Show Guides): a 3 × 3 grid over what the stage
/// shows -- a picture, a composition, a clip, a generation's preview.
/// Each line is drawn as Lightroom draws its overlays, dark grey, light
/// grey, dark grey across, so it reads over any picture: a light line
/// alone vanishes in a sky, a dark one in a shadow.
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

    /// The grid's lines in `rect` (points), each centred so its pixels
    /// are whole at `scale` -- and the rectangle's edge, when `border`.
    static func path(in rect: CGRect, scale: CGFloat,
                     border: Bool = false) -> CGPath {
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
        for i in 1...2 {
            let x = snap(rect.minX + rect.width * CGFloat(i) / 3)
            let y = snap(rect.minY + rect.height * CGFloat(i) / 3)
            p.move(to: CGPoint(x: x, y: rect.minY))
            p.addLine(to: CGPoint(x: x, y: rect.maxY))
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
    func show(over rect: CGRect?, in bounds: CGRect, scale: CGFloat,
              border: Bool) {
        guard let rect, rect.width >= 6, rect.height >= 6 else {
            dark.isHidden = true
            light.isHidden = true
            return
        }
        let path = Guides.path(in: rect, scale: scale, border: border)
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
    @Environment(\.displayScale) private var scale

    var body: some View {
        Canvas { ctx, size in
            let path = Path(Guides.path(in: CGRect(origin: .zero,
                                                   size: size),
                                        scale: scale))
            ctx.stroke(path, with: .color(Color(cgColor: Guides.dark)),
                       lineWidth: Guides.darkWidth(scale))
            ctx.stroke(path, with: .color(Color(cgColor: Guides.light)),
                       lineWidth: Guides.lightWidth(scale))
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}
