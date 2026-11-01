import CoreGraphics
import Foundation

/// CONNECTORS and ANCHORS (core media/markup.h, DESIGN §10a Markup): the
/// points on an outline other lines' and paths' points anchor to, and
/// those points kept on them -- worked out as the core works them out
/// (markup.mm's connectors section), so what the stage draws live while a
/// shape moves is where the core will put it.
extension MarkupRender {
    /// An outline's SEGMENTS in its own pixels, as its connectors lie on
    /// them: a line's one, a rectangle's four from (x0, y0) clockwise, a
    /// path's pieces. An ellipse's is its turn: none here.
    static func outlinePieces(_ o: MarkupObject) -> [Piece] {
        func straight(_ a: CGPoint, _ b: CGPoint) -> Piece {
            Piece(a: a, c1: a, c2: b, b: b)
        }
        switch o.kind {
        case .line:
            return [straight(CGPoint(x: o.x0, y: o.y0),
                             CGPoint(x: o.x1, y: o.y1))]
        case .rect:
            let c = [CGPoint(x: o.x0, y: o.y0), CGPoint(x: o.x1, y: o.y0),
                     CGPoint(x: o.x1, y: o.y1), CGPoint(x: o.x0, y: o.y1)]
            return (0..<4).map { straight(c[$0], c[($0 + 1) % 4]) }
        case .polygon:
            return pieces(polygonPoints(o), smooth: o.smooth,
                          closed: !o.open)
        default:
            return []
        }
    }

    /// Connector (seg, t)'s place in `o`'s own pixels; nil off its
    /// outline.
    static func ownConnector(_ o: MarkupObject, seg: Int,
                             t: Double) -> CGPoint? {
        let t = min(1, max(0, t.isFinite ? t : 0))
        if o.kind == .ellipse {
            guard seg == 0 else { return nil }
            let a = 2 * Double.pi * t
            return CGPoint(x: (o.x0 + o.x1) / 2 + abs(o.x1 - o.x0) / 2 * cos(a),
                           y: (o.y0 + o.y1) / 2 + abs(o.y1 - o.y0) / 2 * sin(a))
        }
        let ps = outlinePieces(o)
        guard ps.indices.contains(seg) else { return nil }
        return ps[seg].along(CGFloat(t))
    }

    /// Its connectors where they are shown, canvas pixels.
    static func connectorPoints(_ o: MarkupObject)
        -> [(id: String, at: CGPoint)]
    {
        guard o.takesConnectors else { return [] }
        let tf = transform(o)
        return o.connectors.compactMap { c in
            ownConnector(o, seg: c.seg, t: c.t).map {
                (c.id, $0.applying(tf))
            }
        }
    }

    /// Connector `id` of `o` where it is shown; nil without it.
    static func connectorAt(_ o: MarkupObject, _ id: String) -> CGPoint? {
        guard let c = o.connectors.first(where: { $0.id == id }) else {
            return nil
        }
        return ownConnector(o, seg: c.seg, t: c.t)?.applying(transform(o))
    }

    /// Where `o`'s outline comes nearest canvas point `p`, as a connector
    /// would lie there: its segment, how far along, the point (canvas
    /// pixels) and how far it is -- an ellipse's round its turn, the rest
    /// in short steps along their segments.
    static func nearestOnOutline(_ o: MarkupObject, to p: CGPoint)
        -> (seg: Int, t: Double, at: CGPoint, distance: Double)?
    {
        guard o.takesConnectors else { return nil }
        let q = own(p, of: o)
        var best: (seg: Int, t: Double, at: CGPoint, distance: Double)?
        func consider(_ seg: Int, _ t: Double, _ at: CGPoint) {
            let d = Double(hypot(at.x - q.x, at.y - q.y))
            if d < (best?.distance ?? .infinity) { best = (seg, t, at, d) }
        }
        if o.kind == .ellipse {
            for s in 0..<720 {
                let t = Double(s) / 720
                if let at = ownConnector(o, seg: 0, t: t) {
                    consider(0, t, at)
                }
            }
        } else {
            for (k, c) in outlinePieces(o).enumerated() {
                let steps = c.straight ? 200 : 240
                for s in 0...steps {
                    let t = Double(s) / Double(steps)
                    consider(k, t, c.along(CGFloat(t)))
                }
            }
        }
        guard let b = best else { return nil }
        return (b.seg, b.t, b.at.applying(transform(o)), b.distance)
    }

    /// A new connector's id on `o`: "c1", "c2", ... not taken.
    static func newConnectorId(_ o: MarkupObject) -> String {
        let taken = Set(o.connectors.map(\.id))
        var n = 1
        while taken.contains("c\(n)") { n += 1 }
        return "c\(n)"
    }

    // MARK: Anchors

    /// Point `k` of a line (its ends) or a path, where it is shown.
    static func anchorablePoint(_ o: MarkupObject, _ k: Int) -> CGPoint? {
        switch o.kind {
        case .line:
            guard k == 0 || k == 1 else { return nil }
            return (k == 0 ? CGPoint(x: o.x0, y: o.y0)
                           : CGPoint(x: o.x1, y: o.y1))
                .applying(transform(o))
        case .polygon:
            let pts = shownPoints(o)
            return pts.indices.contains(k) ? pts[k] : nil
        default:
            return nil
        }
    }

    /// `o` with its turn and flips made its points' (the core's
    /// bake_transform): an anchored object has none of its own.
    static func baked(_ o: MarkupObject) -> MarkupObject {
        let t = transform(o)
        guard !t.isIdentity else { return o }
        var o = o
        if o.kind == .line {
            let a = CGPoint(x: o.x0, y: o.y0).applying(t)
            let b = CGPoint(x: o.x1, y: o.y1).applying(t)
            (o.x0, o.y0, o.x1, o.y1) = (a.x, a.y, b.x, b.y)
        } else {
            let pts = shownPoints(o)
            let ends = gradientHandles(o)
            setPolygon(&o, pts)
            keepGradient(&o, ends)
        }
        o.rotate = 0
        o.flipX = false
        o.flipY = false
        return o
    }

    /// `o`'s point `k` put at `p` (it has no turn): moved or not.
    private static func putting(_ o: MarkupObject, _ k: Int,
                                _ p: CGPoint) -> (MarkupObject, Bool) {
        let near = 1e-6
        var o = o
        if o.kind == .line {
            let (x, y) = k == 0 ? (o.x0, o.y0) : (o.x1, o.y1)
            guard abs(x - p.x) >= near || abs(y - p.y) >= near else {
                return (o, false)
            }
            if k == 0 { (o.x0, o.y0) = (p.x, p.y) } else {
                (o.x1, o.y1) = (p.x, p.y)
            }
            return (o, true)
        }
        var pts = polygonPoints(o)
        guard pts.indices.contains(k),
              abs(pts[k].x - p.x) >= near || abs(pts[k].y - p.y) >= near
        else { return (o, false) }
        pts[k] = p
        let ends = ownGradientHandles(o)
        setPolygon(&o, pts)
        keepGradient(&o, ends)
        return (o, true)
    }

    /// The objects with their ANCHORED points on their connectors now
    /// (the core's resolve_anchors): through chains of them, a few
    /// passes; an anchor whose object or connector is gone -- or is its
    /// own -- dropped, its point left where it was.
    static func resolve(_ objects: [MarkupObject]) -> [MarkupObject] {
        guard objects.contains(where: { !$0.anchors.isEmpty }) else {
            return objects
        }
        var out = objects
        var index: [String: Int] = [:]
        for (i, o) in out.enumerated() { index[o.id] = i }
        let passes = min(max(out.count, 1), 8)
        for _ in 0..<passes {
            var moved = false
            for i in out.indices where !out[i].anchors.isEmpty {
                var kept: [MarkupAnchor] = []
                var at: [(Int, CGPoint)] = []
                for a in out[i].anchors {
                    guard let j = index[a.to], j != i,
                          let p = connectorAt(out[j], a.connector) else {
                        continue
                    }
                    kept.append(a)
                    at.append((a.point, p))
                }
                out[i].anchors = kept
                guard !kept.isEmpty else { continue }
                out[i] = baked(out[i])
                for (k, p) in at {
                    let (o, m) = putting(out[i], k, p)
                    out[i] = o
                    moved = moved || m
                }
            }
            if !moved { break }
        }
        return out
    }

    /// `o` with its anchors kept only to `ids` (the core's detach_moved):
    /// moved or reshaped as a whole, it lets go of the shapes it was not
    /// moved with.
    static func detached(_ o: MarkupObject,
                         keeping ids: Set<String>) -> MarkupObject {
        var o = o
        o.anchors.removeAll { !ids.contains($0.to) }
        return o
    }

    /// The objects anchored to any of `ids` -- directly or through others
    /// -- among `objects`: those that move when they do.
    static func followers(of ids: Set<String>,
                          in objects: [MarkupObject]) -> Set<String> {
        var moving = ids
        var found: Set<String> = []
        var grew = true
        while grew {
            grew = false
            for o in objects where !moving.contains(o.id)
                && o.anchors.contains(where: { moving.contains($0.to) }) {
                moving.insert(o.id)
                found.insert(o.id)
                grew = true
            }
        }
        return found
    }
}

/// A layer's connectors by WHERE they are: cells of 128 canvas pixels,
/// so those near the pointer are found -- and drawn -- without going
/// through all of a layer's thousands at each move.
struct ConnectorGrid {
    struct Entry {
        var object: String
        var connector: String
        var at: CGPoint
    }

    private var cells: [Int64: [Entry]] = [:]
    private static let cell = 128.0

    init(_ objects: [MarkupObject]) {
        for o in objects where o.takesConnectors && !o.connectors.isEmpty {
            for c in MarkupRender.connectorPoints(o) {
                cells[Self.key(Self.index(c.at.x), Self.index(c.at.y)),
                      default: []]
                    .append(Entry(object: o.id, connector: c.id, at: c.at))
            }
        }
    }

    /// Those within `reach` of `p`, nearest first.
    func near(_ p: CGPoint, within reach: Double) -> [Entry] {
        var out: [Entry] = []
        for ix in Self.index(p.x - reach)...Self.index(p.x + reach) {
            for iy in Self.index(p.y - reach)...Self.index(p.y + reach) {
                for e in cells[Self.key(ix, iy)] ?? []
                where hypot(e.at.x - p.x, e.at.y - p.y) <= reach {
                    out.append(e)
                }
            }
        }
        return out.sorted {
            hypot($0.at.x - p.x, $0.at.y - p.y)
                < hypot($1.at.x - p.x, $1.at.y - p.y)
        }
    }

    private static func index(_ v: Double) -> Int {
        Int((v / cell).rounded(.down).clamped(-1e6, 1e6))
    }

    private static func key(_ x: Int, _ y: Int) -> Int64 {
        Int64(x) << 32 | Int64(UInt32(bitPattern: Int32(truncatingIfNeeded: y)))
    }
}

private extension Double {
    func clamped(_ lo: Double, _ hi: Double) -> Double {
        Swift.min(hi, Swift.max(lo, self))
    }
}

extension MarkupRender.Piece {
    /// Its point at `t`: evenly along a straight piece, by the curve's own
    /// parameter along a curve (the core's piece_at).
    func along(_ t: CGFloat) -> CGPoint {
        guard straight else { return at(t) }
        return CGPoint(x: a.x + t * (b.x - a.x), y: a.y + t * (b.y - a.y))
    }
}
