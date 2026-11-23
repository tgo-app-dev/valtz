import SwiftUI

/// A Dial's SCHEDULE (MiniMax H3's steering, Favor's Custom): its
/// strength over the clip's time, edited as a gradient's stops are -- the
/// clip left to right (its first frame to its end, in percent: no time
/// is named), the strength up and down from -3 to 3 round a zero line.
/// A click where there is no point adds one there; a point dragged moves
/// (between its neighbours, held to the square); a double-click on one,
/// or a drag off the top or bottom, removes it (two stay at least). The
/// strength goes linearly from point to point, held flat before the
/// first and after the last, as vpipe's curve does.
struct DialCurveEditor: View {
    let points: [[Double]]
    let negative: String
    let positive: String
    let changed: ([[Double]]) -> Void
    @State private var dragging: Int?
    @State private var live: [[Double]]?

    static let range = -3.0...3.0
    private let dot: CGFloat = 9

    var body: some View {
        let pts = live ?? points
        VStack(alignment: .leading, spacing: 3) {
            GeometryReader { g in
                let size = g.size
                ZStack(alignment: .topLeading) {
                    RoundedRectangle(cornerRadius: 5)
                        .fill(Color.primary.opacity(0.04))
                    RoundedRectangle(cornerRadius: 5)
                        .strokeBorder(Color.primary.opacity(0.12))
                    // The strengths' scale: 0 a line, ±1 and ±2 faint.
                    ForEach([-2.0, -1.0, 1.0, 2.0], id: \.self) { v in
                        Path { p in
                            let y = yOf(v, size)
                            p.move(to: CGPoint(x: 0, y: y))
                            p.addLine(to: CGPoint(x: size.width, y: y))
                        }
                        .stroke(Color.primary.opacity(0.07),
                                style: StrokeStyle(lineWidth: 1, dash: [2, 3]))
                    }
                    Path { p in
                        let y = yOf(0, size)
                        p.move(to: CGPoint(x: 0, y: y))
                        p.addLine(to: CGPoint(x: size.width, y: y))
                    }
                    .stroke(Color.primary.opacity(0.25), lineWidth: 1)
                    // The scale, at the left: +3, 0, -3.
                    ForEach([3.0, 0.0, -3.0], id: \.self) { v in
                        Text(verbatim: v == 0 ? "0" : String(format: "%+.0f", v))
                            .font(.system(size: 8).monospacedDigit())
                            .foregroundStyle(.tertiary)
                            .position(x: 9, y: min(max(yOf(v, size), 6),
                                                   size.height - 6))
                            .allowsHitTesting(false)
                    }
                    // The curve, held flat to the edges.
                    curve(pts, size)
                        .stroke(Color.accentColor, lineWidth: 1.6)
                    ForEach(pts.indices, id: \.self) { i in
                        Circle()
                            .fill(dragging == i ? Color.accentColor
                                                : Color(nsColor: .textBackgroundColor))
                            .overlay(Circle().strokeBorder(Color.accentColor,
                                                           lineWidth: 1.5))
                            .frame(width: dot, height: dot)
                            .position(x: xOf(pts[i][0], size),
                                      y: yOf(pts[i][1], size))
                            .help(tip(pts[i]))
                    }
                }
                .contentShape(Rectangle())
                .gesture(press(size))
                .simultaneousGesture(SpatialTapGesture(count: 2)
                    .onEnded { e in removeNear(e.location, size) })
            }
            .frame(height: 84)
            HStack {
                Text(verbatim: "0%")
                Spacer()
                Text(verbatim: "50%")
                Spacer()
                Text(verbatim: "100%")
            }
            .font(.system(size: 9).monospacedDigit())
            .foregroundStyle(.tertiary)
            if !negative.isEmpty || !positive.isEmpty {
                Text(verbatim: "−: \(negative)   +: \(positive)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
        }
    }

    // MARK: Geometry

    private func xOf(_ x: Double, _ s: CGSize) -> CGFloat {
        let inset = dot / 2
        return inset + CGFloat(x) * (s.width - 2 * inset)
    }

    private func yOf(_ v: Double, _ s: CGSize) -> CGFloat {
        let inset = dot / 2
        let t = (Self.range.upperBound - v)
            / (Self.range.upperBound - Self.range.lowerBound)
        return inset + CGFloat(t) * (s.height - 2 * inset)
    }

    private func point(_ p: CGPoint, _ s: CGSize) -> [Double] {
        let inset = dot / 2
        let x = Double((p.x - inset) / max(1, s.width - 2 * inset))
        let t = Double((p.y - inset) / max(1, s.height - 2 * inset))
        let v = Self.range.upperBound
            - t * (Self.range.upperBound - Self.range.lowerBound)
        return [min(1, max(0, x)),
                (min(Self.range.upperBound,
                     max(Self.range.lowerBound, v)) * 20).rounded() / 20]
    }

    private func curve(_ pts: [[Double]], _ s: CGSize) -> Path {
        Path { p in
            guard let first = pts.first, let last = pts.last else { return }
            p.move(to: CGPoint(x: 0, y: yOf(first[1], s)))
            for q in pts {
                p.addLine(to: CGPoint(x: xOf(q[0], s), y: yOf(q[1], s)))
            }
            p.addLine(to: CGPoint(x: s.width, y: yOf(last[1], s)))
        }
    }

    private func tip(_ q: [Double]) -> String {
        let pct = String(Int((q[0] * 100).rounded()))
        let v = String(format: "%+.2f", q[1])
        return String(localized: "\(pct)% of the clip: strength \(v)")
    }

    // MARK: Editing

    /// A press: on a point, it is dragged; elsewhere, a point is added
    /// there and dragged on. Let go off the top or the bottom, it goes.
    private func press(_ s: CGSize) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { g in
                var pts = live ?? points
                if dragging == nil {
                    if let i = nearest(g.startLocation, pts, s) {
                        dragging = i
                    } else {
                        let q = point(g.startLocation, s)
                        let at = pts.firstIndex { $0[0] > q[0] } ?? pts.count
                        pts.insert(q, at: at)
                        dragging = at
                    }
                }
                guard let i = dragging else { return }
                var q = point(g.location, s)
                // Between its neighbours: the order kept.
                let lo = i > 0 ? pts[i - 1][0] + 0.005 : 0
                let hi = i < pts.count - 1 ? pts[i + 1][0] - 0.005 : 1
                q[0] = min(max(q[0], lo), max(lo, hi))
                pts[i] = q
                live = pts
            }
            .onEnded { g in
                var pts = live ?? points
                if let i = dragging, pts.count > 2,
                   g.location.y < -12 || g.location.y > s.height + 12 {
                    pts.remove(at: i)
                }
                dragging = nil
                live = nil
                changed(pts)
            }
    }

    private func nearest(_ p: CGPoint, _ pts: [[Double]],
                         _ s: CGSize) -> Int? {
        pts.indices.min { a, b in
            dist(p, pts[a], s) < dist(p, pts[b], s)
        }.flatMap { dist(p, pts[$0], s) <= 9 ? $0 : nil }
    }

    private func dist(_ p: CGPoint, _ q: [Double], _ s: CGSize) -> CGFloat {
        hypot(p.x - xOf(q[0], s), p.y - yOf(q[1], s))
    }

    private func removeNear(_ p: CGPoint, _ s: CGSize) {
        var pts = points
        guard pts.count > 2, let i = nearest(p, pts, s) else { return }
        pts.remove(at: i)
        changed(pts)
    }
}
