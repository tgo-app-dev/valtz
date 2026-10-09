import AppKit
import SwiftUI

/// A slider whose NOTHING is marked -- the Adjust card's, the rotation's:
/// a tick where the value changes nothing, and the accent filled from
/// there to the knob, rounded at both ends, rather than from the left
/// edge. Drawn whole here: macOS 26's own slider draws its bar itself,
/// past a cell's drawing. A press on the track takes the knob there, a
/// drag moves it; VoiceOver steps it a fiftieth of its range.
struct CenteredSlider: View {
    @Binding var value: Double
    var range: ClosedRange<Double>
    /// Where nothing changes.
    var zero = 0.0
    @Environment(\.isEnabled) private var enabled

    private static let knob = CGSize(width: 20, height: 12)
    private static let bar: CGFloat = 4

    var body: some View {
        GeometryReader { g in
            let lo = Self.knob.width / 2
            let hi = max(lo, g.size.width - Self.knob.width / 2)
            let span = range.upperBound - range.lowerBound
            let x = { (v: Double) -> CGFloat in
                span > 0 ? lo + CGFloat((min(max(v, range.lowerBound),
                                             range.upperBound)
                                         - range.lowerBound) / span)
                    * (hi - lo) : lo
            }
            let mid = g.size.height / 2
            let kx = x(value), zx = x(zero)
            ZStack {
                Capsule()
                    .fill(Color.primary.opacity(0.12))
                    .frame(width: max(0, g.size.width - 2), height: Self.bar)
                    .position(x: g.size.width / 2, y: mid)
                // Taller than the knob: it shows at zero too.
                Rectangle()
                    .fill(Color.secondary)
                    .frame(width: 1, height: 17)
                    .position(x: zx.rounded() + 0.5, y: mid)
                if abs(kx - zx) > 0.5 {
                    Capsule()
                        .fill(Color.accentColor.opacity(enabled ? 1 : 0.35))
                        .frame(width: abs(kx - zx) + Self.bar,
                               height: Self.bar)
                        .position(x: (kx + zx) / 2, y: mid)
                }
                Capsule()
                    .fill(Color.white)
                    .overlay(Capsule().strokeBorder(Color.black.opacity(0.12),
                                                    lineWidth: 0.5))
                    .shadow(color: .black.opacity(enabled ? 0.28 : 0.1),
                            radius: 1.2, y: 0.5)
                    .frame(width: Self.knob.width, height: Self.knob.height)
                    .position(x: kx, y: mid)
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { v in
                guard enabled, hi > lo else { return }
                let t = Double((v.location.x - lo) / (hi - lo))
                value = range.lowerBound + min(1, max(0, t)) * span
            })
        }
        .frame(height: 18)
        .opacity(enabled ? 1 : 0.6)
        .accessibilityElement()
        .accessibilityValue(Text(verbatim: String(format: "%.2f", value)))
        .accessibilityAdjustableAction { dir in
            let step = (range.upperBound - range.lowerBound) / 50
            value = min(range.upperBound, max(range.lowerBound,
                value + (dir == .increment ? step : -step)))
        }
    }
}

/// A JOG for a value, as a camera's wheel: a ring with a dot in it.
/// Dragged left the value goes down, right it goes up -- `perPoint` a
/// point (⌥ a tenth of that, ⇧ ten times) -- the dot following the drag
/// inside the ring and springing back when let go.
struct JogWheel: View {
    let perPoint: Double
    let help: LocalizedStringKey
    /// By how much it changes: called as the drag goes.
    let nudge: (Double) -> Void
    @State private var shift: CGFloat = 0
    @State private var last: CGFloat?
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled

    static let size: CGFloat = 20

    var body: some View {
        let dragging = last != nil
        let travel = Self.size / 2 - 5
        ZStack {
            Circle()
                .fill(Color.primary.opacity(hovering || dragging ? 0.09
                                                                 : 0.04))
            Circle()
                .strokeBorder(Color.primary.opacity(0.35), lineWidth: 1)
            Circle()
                .fill(dragging ? Color.accentColor : Color.primary)
                .frame(width: 6, height: 6)
                .offset(x: min(travel, max(-travel, shift)))
        }
        .frame(width: Self.size, height: Self.size)
        .opacity(enabled ? 1 : 0.4)
        .contentShape(Circle())
        .pointerStyle(.columnResize)
        .onHover { hovering = $0 }
        .gesture(DragGesture(minimumDistance: 0)
            .onChanged { v in
                let x = v.translation.width
                let d = x - (last ?? 0)
                last = x
                shift = x
                guard d != 0 else { return }
                let mods = NSEvent.modifierFlags
                let k = mods.contains(.option) ? 0.1
                    : mods.contains(.shift) ? 10 : 1
                nudge(Double(d) * perPoint * k)
            }
            .onEnded { _ in
                last = nil
                withAnimation(.spring(duration: 0.25)) { shift = 0 }
            })
        .help(help)
        .accessibilityElement()
        .accessibilityLabel(Text(help))
        .accessibilityAdjustableAction { dir in
            nudge((dir == .increment ? 10 : -10) * perPoint)
        }
    }
}
