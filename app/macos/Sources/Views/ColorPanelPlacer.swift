import AppKit

/// The colour panel OPENED BY ITS WELL: a click on any colour well -- the
/// markup toolbar's Color and Fill, a gradient's stop, an effect's, the
/// Crop card's background -- puts the system's colour panel right under
/// that well (above it where the screen ends), not where it was last
/// left, which on a large screen could be far from the control that
/// opened it. A panel already open goes to the well clicked.
@MainActor
enum ColorPanelPlacer {
    private static var monitor: Any?
    /// The panel's frame and the well's, both on the screen, as the last
    /// click placed it (the snapshot hooks print them).
    private(set) static var last: (panel: CGRect, well: CGRect)?

    static func install() {
        guard monitor == nil else { return }
        monitor = NSEvent.addLocalMonitorForEvents(matching: .leftMouseUp) {
            e in
            MainActor.assumeIsolated { clicked(e) }
            return e
        }
    }

    /// A click let go: on a colour well, the panel placed by it once the
    /// well has opened it (it does as it takes the click).
    private static func clicked(_ e: NSEvent) {
        guard let w = e.window,
              var v = w.contentView?.superview?.hitTest(e.locationInWindow)
        else { return }
        while !(v is NSColorWell) {
            guard let up = v.superview else { return }
            v = up
        }
        guard let well = v as? NSColorWell else { return }
        DispatchQueue.main.async {
            MainActor.assumeIsolated { place(by: well) }
        }
    }

    static func place(by well: NSColorWell) {
        let panel = NSColorPanel.shared
        guard panel.isVisible, well.isActive, let w = well.window else {
            return
        }
        let r = w.convertToScreen(well.convert(well.bounds, to: nil))
        guard let vis = (w.screen ?? NSScreen.main)?.visibleFrame else {
            return
        }
        let size = panel.frame.size
        let gap: CGFloat = 6
        var o = NSPoint(x: r.minX, y: r.minY - gap - size.height)
        if o.y < vis.minY { o.y = r.maxY + gap }
        o.x = min(max(o.x, vis.minX), max(vis.minX, vis.maxX - size.width))
        o.y = min(max(o.y, vis.minY), max(vis.minY, vis.maxY - size.height))
        panel.setFrameOrigin(o)
        last = (panel.frame, r)
    }
}
