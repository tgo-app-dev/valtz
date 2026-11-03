import AppKit
import SwiftUI

extension View {
    /// A popover AppKit shows that STAYS while Valtz is left. SwiftUI's
    /// own is transient: it shuts as soon as another app is used -- and
    /// Tune's panel has to stay open while a LoRA, a DiT or a VAE is
    /// dragged in from the Finder. This one shuts on a click in Valtz
    /// outside it (once that click has done its work: a press on what
    /// opened it still toggles it) and on Escape -- never because another
    /// app is used or something is dragged in. `anchor` is the point of
    /// this view it hangs from, `arrowEdge` the side of it the popover is
    /// on (SwiftUI's sense).
    ///
    /// `fitted`: the popover is the size its content REPORTS (the
    /// environment's `popoverFit`), within the room beside its anchor --
    /// set by Valtz once a size is reported, after AppKit's layout, not
    /// followed by AppKit within it. A popover following its content's
    /// preferred size, whose content measures itself and resizes (Tune's
    /// panel: a scroll view as tall as its rows), crashed Valtz on macOS
    /// 27.2 as it opened: each frame AppKit's layout gave the hosting
    /// view asked for another constraints pass, until AppKit gave up.
    func appKitPopover<Content: View>(
        isPresented: Binding<Bool>, anchor: UnitPoint = .center,
        arrowEdge: Edge = .bottom, fitted: Bool = false,
        @ViewBuilder content: @escaping () -> Content) -> some View {
        background(AppKitPopover(isPresented: isPresented, anchor: anchor,
                                 edge: arrowEdge, fitted: fitted,
                                 content: content))
    }
}

/// What a fitted popover's content is told (appKitPopover(fitted:)):
/// the room it may take -- its screen's, beside its anchor, less the
/// popover's frame -- and where to report the size it would take whole.
/// The popover takes that size, held to the room, in whole points.
struct PopoverFit: Sendable {
    let room: CGSize
    let fit: @MainActor @Sendable (CGSize) -> Void
}

extension EnvironmentValues {
    /// The fitted popover holding this view, if one does.
    @Entry var popoverFit: PopoverFit? = nil
}

/// The popover's anchor: a view the size of the one it hangs from, which
/// shows the NSPopover, shuts it on a click elsewhere in Valtz, and hears
/// it shut.
private struct AppKitPopover<Content: View>: NSViewRepresentable {
    @Binding var isPresented: Bool
    let anchor: UnitPoint
    let edge: Edge
    let fitted: Bool
    let content: () -> Content

    func makeCoordinator() -> Coordinator { Coordinator() }

    /// What the popover hosts: the content, told -- fitted -- its room
    /// and where to report its size.
    struct Root: View {
        let fit: PopoverFit?
        let content: Content
        var body: some View { content.environment(\.popoverFit, fit) }
    }

    @MainActor
    final class Coordinator: NSObject, NSPopoverDelegate {
        var popover: NSPopover?
        var host: NSHostingController<Root>?
        var monitor: Any?
        var shut: () -> Void = {}
        /// Fitted: what its content is told, and the size the popover
        /// was last given -- kept between openings, so it opens at it.
        var fitter: PopoverFit?
        var size: CGSize?

        /// The content's size reported, held to the room in whole
        /// points: the popover's, set after the layout pass that
        /// measured it -- and only when it changes, so a frame given
        /// back never asks again.
        func fit(_ want: CGSize, room: CGSize) {
            let s = CGSize(width: min(want.width, room.width).rounded(.up),
                           height: min(want.height, room.height)
                               .rounded(.up))
            guard s.width > 0, s.height > 0, s != size else { return }
            size = s
            DispatchQueue.main.async { [weak self] in
                guard let p = self?.popover, let s = self?.size,
                      p.contentSize != s else { return }
                p.contentSize = s
            }
        }

        /// A press in Valtz outside the popover shuts it -- after the
        /// press is handled -- and Escape does; a press in another app is
        /// not Valtz's, and a drag in sends none.
        func watch(_ p: NSPopover) {
            unwatch()
            monitor = NSEvent.addLocalMonitorForEvents(
                matching: [.leftMouseDown, .rightMouseDown, .keyDown]
            ) { [weak self, weak p] e in
                // Escape is swallowed; a press goes on to what it hits.
                let swallow = MainActor.assumeIsolated { () -> Bool in
                    guard let p, self?.popover === p else { return false }
                    let own = p.contentViewController?.view.window
                    if e.type == .keyDown {
                        guard e.keyCode == 53 else { return false }
                        self?.dismiss(tell: true)
                        return true
                    }
                    // An open panel or another floating panel (choosing
                    // a file for it, say) leaves it open.
                    if e.window !== own, !(e.window is NSPanel) {
                        DispatchQueue.main.async { [weak self] in
                            if self?.popover === p { self?.dismiss(tell: true) }
                        }
                    }
                    return false
                }
                return swallow ? nil : e
            }
        }

        func unwatch() {
            if let m = monitor { NSEvent.removeMonitor(m) }
            monitor = nil
        }

        /// Shut -- and, shut by a press or Escape, the binding told
        /// (NSPopover.close tells no delegate).
        func dismiss(tell: Bool) {
            let p = popover
            unwatch()
            popover = nil
            host = nil
            p?.close()
            if tell { shut() }
        }

        func popoverDidClose(_ notification: Notification) {
            if popover != nil { dismiss(tell: true) }
        }
    }

    /// The anchor: says when it is in a window and has a size -- a
    /// popover asked for before that (a view just made, as a suggestion
    /// appearing with what it points at) is shown then.
    final class Anchor: NSView {
        var placed: () -> Void = {}

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if window != nil { placed() }
        }

        override func setFrameSize(_ newSize: NSSize) {
            let was = frame.size
            super.setFrameSize(newSize)
            if window != nil, was.width == 0 || was.height == 0,
               newSize.width > 0, newSize.height > 0 {
                placed()
            }
        }
    }

    func makeNSView(context: Context) -> Anchor { Anchor() }

    func updateNSView(_ v: Anchor, context: Context) {
        let c = context.coordinator
        let presented = $isPresented
        c.shut = { if presented.wrappedValue { presented.wrappedValue = false } }
        guard isPresented else {
            c.dismiss(tell: false)
            return
        }
        if let host = c.host {
            host.rootView = Root(fit: c.fitter, content: content())
            return
        }
        // Shown once the anchor is in a window, laid out -- now, or when
        // it gets there.
        let content = content
        let anchor = anchor
        let edge = edge
        let fitted = fitted
        let show = { [weak v] in
            guard let v, presented.wrappedValue, c.popover == nil,
                  v.window != nil, v.bounds.width > 0,
                  v.bounds.height > 0 else { return }
            let room = Self.room(beside: v)
            c.fitter = fitted
                ? PopoverFit(room: room) { [weak c] s in
                      c?.fit(s, room: room)
                  }
                : nil
            let host = NSHostingController(
                rootView: Root(fit: c.fitter, content: content()))
            // Fitted: sized by Valtz alone (Coordinator.fit), at the size
            // it was last given until its content reports one.
            host.sizingOptions = fitted ? [] : .preferredContentSize
            let p = NSPopover()
            if fitted {
                let s = c.size ?? CGSize(width: 640, height: 320)
                p.contentSize = CGSize(width: min(s.width, room.width),
                                       height: min(s.height, room.height))
            }
            // Shut by the press monitor alone: never by AppKit as Valtz
            // is left.
            p.behavior = .applicationDefined
            p.animates = true
            p.contentViewController = host
            p.delegate = c
            c.popover = p
            c.host = host
            let b = v.bounds
            let at = CGRect(x: b.minX + anchor.x * b.width - 1,
                            y: b.minY + (1 - anchor.y) * b.height - 1,
                            width: 2, height: 2)
            p.show(relativeTo: at, of: v, preferredEdge: Self.side(edge))
            c.watch(p)
        }
        v.placed = { DispatchQueue.main.async(execute: show) }
        DispatchQueue.main.async(execute: show)
    }

    static func dismantleNSView(_ v: Anchor, coordinator: Coordinator) {
        coordinator.shut = {}
        coordinator.dismiss(tell: false)
    }

    /// The room a fitted popover may take beside `v`: its screen's width
    /// less a margin, and the taller of the spaces above and below it --
    /// AppKit shows it on a side where it fits -- less the popover's
    /// frame and arrow (26 points around the content on macOS 27) and a
    /// margin.
    private static func room(beside v: NSView) -> CGSize {
        guard let w = v.window,
              let screen = (w.screen ?? NSScreen.main)?.visibleFrame else {
            return CGSize(width: 1120, height: 600)
        }
        let at = w.convertToScreen(v.convert(v.bounds, to: nil))
        let side = max(screen.maxY - at.maxY, at.minY - screen.minY)
        return CGSize(width: max(320, screen.width - 80),
                      height: max(200, side - 56))
    }

    /// SwiftUI's arrow edge as AppKit's edge of the anchor (the view is
    /// not flipped: maxY is its top).
    private static func side(_ e: Edge) -> NSRectEdge {
        switch e {
        case .top: .maxY
        case .bottom: .minY
        case .leading: .minX
        case .trailing: .maxX
        }
    }
}
