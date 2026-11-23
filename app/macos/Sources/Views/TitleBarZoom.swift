import AppKit
import SwiftUI

/// A double-click on an editor window's TITLE BAR -- on its empty part,
/// not the name or a control -- fills the screen the window is on, its
/// Dock and menu bar left clear (the screen's visible frame), and the
/// next puts it back where it was. Not full screen: the window stays a
/// window. The title bar is the toolbar's, which takes the clicks the
/// system's own title bar would act on, so the double-click did nothing.
enum TitleBarZoom {
    @MainActor static func install() {
        _ = NSEvent.addLocalMonitorForEvents(matching: .leftMouseDown) { e in
            nonisolated(unsafe) let ev = e
            let took = MainActor.assumeIsolated { handle(ev) }
            return took ? nil : e
        }
    }

    /// True when it was a double-click on a title bar's empty part, and
    /// the window was zoomed or put back.
    @MainActor static func handle(_ e: NSEvent) -> Bool {
        // (a snapshot run: what a double-click hit, for the hooks)
        if e.clickCount == 2, let w = e.window,
           ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] != nil {
            FileHandle.standardError.write(Data(
                "snapshot: titlebar-click empty=\(onEmptyTitleBar(e.locationInWindow, of: w)) chain=\(chain(e.locationInWindow, of: w))\n".utf8))
        }
        guard e.clickCount == 2, let w = e.window,
              let model = EditorWindows.shared.owner(of: w),
              !w.styleMask.contains(.fullScreen),
              onEmptyTitleBar(e.locationInWindow, of: w) else {
            return false
        }
        toggle(w, model)
        return true
    }

    /// The point is in the title bar (above the content's layout rect)
    /// and on nothing there that takes a click: not in a part marked so
    /// (TitleBarHitProbe: ☰, the name, the controls' capsules -- hosted
    /// in toolbar items wider than they are) nor on a control.
    @MainActor static func onEmptyTitleBar(_ p: NSPoint,
                                           of w: NSWindow) -> Bool {
        guard p.y > w.contentLayoutRect.maxY,
              let frame = w.contentView?.superview else { return false }
        func marked(_ v: NSView) -> Bool {
            if v is TitleBarHitView, !v.isHiddenOrHasHiddenAncestor,
               v.convert(v.bounds, to: nil).contains(p) {
                return true
            }
            return v.subviews.contains { marked($0) }
        }
        if marked(frame) { return false }
        var v = frame.hitTest(frame.convert(p, from: nil))
        while let view = v {
            if view is NSControl { return false }
            v = view.superview
        }
        return true
    }

    /// The toolbar's items' spans in the window (a hook's line).
    @MainActor static func items(_ w: NSWindow?) -> String {
        guard let frame = w?.contentView?.superview else { return "items=-" }
        func walk(_ v: NSView) -> [String] {
            var out: [String] = []
            if String(describing: type(of: v)).contains("ItemViewer") {
                let r = v.convert(v.bounds, to: nil)
                out.append("\(Int(r.minX))-\(Int(r.maxX))")
            }
            return out + v.subviews.flatMap { walk($0) }
        }
        return "items=" + walk(frame).joined(separator: ",")
    }

    /// The views under a point, front first (a hook's line).
    @MainActor static func chain(_ p: NSPoint, of w: NSWindow) -> String {
        guard let frame = w.contentView?.superview else { return "-" }
        var out: [String] = []
        var v = frame.hitTest(frame.convert(p, from: nil))
        while let view = v {
            let r = view.convert(view.bounds, to: nil)
            out.append("\(type(of: view))@\(Int(r.minX)),\(Int(r.minY)),\(Int(r.width))x\(Int(r.height))")
            v = view.superview
        }
        func marks(_ v: NSView) -> [String] {
            var mine: [String] = []
            if v is TitleBarHitView {
                let r = v.convert(v.bounds, to: nil)
                mine.append("\(Int(r.minX))-\(Int(r.maxX))")
            }
            return mine + v.subviews.flatMap { marks($0) }
        }
        return out.joined(separator: "<") + " marked="
            + marks(frame).joined(separator: ",")
    }

    @MainActor static func toggle(_ w: NSWindow, _ model: AppModel) {
        guard let screen = w.screen ?? NSScreen.main else { return }
        let full = screen.visibleFrame
        let filled = abs(w.frame.minX - full.minX) < 2
            && abs(w.frame.minY - full.minY) < 2
            && abs(w.frame.width - full.width) < 2
            && abs(w.frame.height - full.height) < 2
        if filled, let back = model.unzoomedFrame {
            model.unzoomedFrame = nil
            w.setFrame(back, display: true, animate: true)
        } else if !filled {
            model.unzoomedFrame = w.frame
            w.setFrame(full, display: true, animate: true)
        }
    }
}

/// A part of the title bar that takes clicks, marked for TitleBarZoom: a
/// view the size of what it is behind, which itself takes none.
final class TitleBarHitView: NSView {
    override func hitTest(_ point: NSPoint) -> NSView? { nil }
}

struct TitleBarHitProbe: NSViewRepresentable {
    func makeNSView(context: Context) -> TitleBarHitView { TitleBarHitView() }
    func updateNSView(_ v: TitleBarHitView, context: Context) {}
}
