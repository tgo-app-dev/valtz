import AppKit
import SwiftUI

/// A splitter's grip: an AppKit view, so the drag is the mouse's own --
/// down, dragged, up -- with the resize cursor over it. `changed` gets
/// how far it has moved from the press, along the axis: downward for a
/// vertical split (the dock's top edge, the inspector's sections),
/// rightward for a horizontal one. A double-click resets. VoiceOver
/// hears a splitter named `label`, reading `value` (the height it sets,
/// say), and adjusts it in steps of `step`.
struct DragHandle: NSViewRepresentable {
    var axis: Axis = .vertical
    var label: String
    var value: String? = nil
    var changed: (CGFloat) -> Void
    var ended: () -> Void = {}
    var reset: () -> Void = {}
    var step: CGFloat = 20

    func makeNSView(context: Context) -> Grip {
        let v = Grip()
        v.setAccessibilityElement(true)
        v.setAccessibilityRole(.splitter)
        describe(v)
        return v
    }

    func updateNSView(_ v: Grip, context: Context) { describe(v) }

    private func describe(_ v: Grip) {
        v.handle = self
        // The grip's own orientation is across the split it moves: a
        // vertical drag moves a horizontal bar.
        v.setAccessibilityOrientation(axis == .horizontal ? .vertical
                                                           : .horizontal)
        v.setAccessibilityLabel(label)
        v.setAccessibilityValue(value)
    }

    final class Grip: NSView {
        var handle: DragHandle?
        private var pressed: CGPoint?

        override func resetCursorRects() {
            addCursorRect(bounds, cursor: handle?.axis == .horizontal
                          ? .resizeLeftRight : .resizeUpDown)
        }

        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        override func mouseDown(with event: NSEvent) {
            if event.clickCount == 2 {
                pressed = nil
                handle?.reset()
                return
            }
            pressed = event.locationInWindow
        }

        override func mouseDragged(with event: NSEvent) {
            guard let p = pressed, let h = handle else { return }
            let now = event.locationInWindow
            // Window coordinates grow upward; a vertical drag counts down.
            h.changed(h.axis == .horizontal ? now.x - p.x : p.y - now.y)
        }

        override func mouseUp(with event: NSEvent) {
            guard pressed != nil else { return }
            pressed = nil
            handle?.ended()
        }

        override func accessibilityPerformIncrement() -> Bool {
            guard let h = handle else { return false }
            h.changed(h.step)
            h.ended()
            return true
        }

        override func accessibilityPerformDecrement() -> Bool {
            guard let h = handle else { return false }
            h.changed(-h.step)
            h.ended()
            return true
        }
    }
}
