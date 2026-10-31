import SwiftUI

/// A small ring of progress: the wedge from twelve o'clock as far as a
/// task has come, or -- while nothing is counted -- an arc going round.
/// Stop's badge in the composer, and the toolbar's Activity item, where
/// a count of what is queued sits beside it.
struct ActivityRing: View {
    var fraction: Double?
    var diameter: CGFloat = 16
    /// Off: the arc stands still (a hidden Stop draws nothing to move).
    var running = true
    /// At rest: the ring hollow, nothing in it.
    var idle = false

    var body: some View {
        ZStack {
            Circle().fill(.quaternary)
            Circle().strokeBorder(.tertiary, lineWidth: 0.5)
            if idle {
                EmptyView()
            } else if let f = fraction {
                Pie(fraction: f)
                    .fill(.secondary)
                    .padding(1.5)
                    .animation(.linear(duration: 0.2), value: f)
            } else {
                TimelineView(.animation(minimumInterval: nil,
                                        paused: !running)) { ctx in
                    let t = ctx.date.timeIntervalSinceReferenceDate
                    Circle()
                        .trim(from: 0, to: 0.28)
                        .stroke(.secondary,
                                style: StrokeStyle(lineWidth: 2,
                                                   lineCap: .round))
                        .padding(2)
                        .rotationEffect(.degrees(
                            t.truncatingRemainder(dividingBy: 1) * 360))
                }
            }
        }
        .frame(width: diameter, height: diameter)
    }

    /// The wedge from twelve o'clock to `fraction` of the way round.
    private struct Pie: Shape {
        var fraction: Double

        var animatableData: Double {
            get { fraction }
            set { fraction = newValue }
        }

        func path(in rect: CGRect) -> Path {
            var p = Path()
            let f = min(max(fraction, 0), 1)
            guard f > 0 else { return p }
            let c = CGPoint(x: rect.midX, y: rect.midY)
            p.move(to: c)
            // Clockwise as seen: SwiftUI's y runs down.
            p.addArc(center: c, radius: min(rect.width, rect.height) / 2,
                     startAngle: .degrees(-90),
                     endAngle: .degrees(-90 + 360 * f), clockwise: false)
            p.closeSubpath()
            return p
        }
    }
}
