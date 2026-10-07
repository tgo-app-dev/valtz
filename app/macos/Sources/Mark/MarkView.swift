import MetalKit
import SwiftUI

/// The Valtz mark, drawn with Metal from geometry (MarkRenderer) rather
/// than shown from a bitmap: sharp at any size and scale.
///
/// Idle, it is ONE frame rendered offscreen at the view's exact pixel
/// size and shown as an image: nothing runs per frame, and the window
/// can be captured like any other view. While `animating` (a job is
/// loading its model) a live Metal view takes over and the ribbon
/// waltzes (the app does not use this today): it sways, and a slow wave
/// travels along its twist. The
/// motion eases in and out; once it has settled, the live view (whose
/// last frame is the still one) gives way to the image again.
///
/// Each change of `sweep` plays the LIGHT SWEEP once: for one second a
/// bright light passes from the right of the window to the left, and the
/// glass's reflections and refractions follow it; then it is gone.
struct MarkView: View {
    var animating: Bool
    var material: MarkRenderer.Material = .glass
    var sweep: Int = 0
    var shadow: Bool = false

    @Environment(\.displayScale) private var scale
    @Environment(\.colorScheme) private var scheme
    @State private var still: CGImage?
    @State private var live = false
    @State private var liveShowing = false   // its first frame is on screen
    @State private var sweepPending = false

    var body: some View {
        GeometryReader { geo in
            let pw = Int((geo.size.width * scale).rounded())
            let ph = Int((geo.size.height * scale).rounded())
            ZStack {
                // The still stays until the live view has drawn, so the
                // hand-over never shows an empty frame.
                if let still, !(live && liveShowing) {
                    Image(decorative: still, scale: scale)
                        .resizable()
                        .interpolation(.high)
                }
                if live {
                    LiveMarkView(animating: animating, material: material,
                                 lightGround: scheme == .light, shadow: shadow,
                                 sweep: sweepPending,
                                 sweepTaken: { sweepPending = false },
                                 showing: { liveShowing = true },
                                 settled: { live = false; liveShowing = false })
                }
            }
            .frame(width: geo.size.width, height: geo.size.height)
            .task(id: "\(pw)x\(ph)-\(scheme == .light)") {
                guard pw > 0, ph > 0, let r = MarkRenderer.shared else { return }
                r.material = material
                r.lightGround = scheme == .light
                r.shadow = shadow
                r.motion = 0
                still = r.image(width: pw, height: ph, style: .markFit,
                                supersample: 2,
                                pointSize: Float(geo.size.width))
            }
        }
        .onChange(of: animating, initial: true) { _, on in
            if on { live = true }
        }
        .onChange(of: sweep) { _, _ in
            sweepPending = true
            live = true
        }
    }
}

/// The live half: an MTKView that runs its display link only while the
/// mark is moving or lit.
private struct LiveMarkView: NSViewRepresentable {
    var animating: Bool
    var material: MarkRenderer.Material
    var lightGround: Bool
    var shadow: Bool
    var sweep: Bool
    var sweepTaken: () -> Void
    var showing: () -> Void
    var settled: () -> Void

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> MTKView {
        let c = context.coordinator
        let view = MTKView(frame: .zero, device: c.renderer?.device)
        view.colorPixelFormat = .bgra8Unorm
        view.clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        view.layer?.isOpaque = false
        view.preferredFramesPerSecond = 60
        view.delegate = c
        c.view = view
        return view
    }

    func updateNSView(_ view: MTKView, context: Context) {
        let c = context.coordinator
        c.settled = settled
        c.showing = showing
        c.renderer?.material = material
        c.renderer?.lightGround = lightGround
        c.renderer?.shadow = shadow
        c.setAnimating(animating)
        if sweep {
            c.startSweep()
            DispatchQueue.main.async { sweepTaken() }
        }
    }

    @MainActor
    final class Coordinator: NSObject, MTKViewDelegate {
        let renderer = MarkRenderer()
        weak var view: MTKView?
        var settled: () -> Void = {}
        var showing: () -> Void = {}
        private var target: Float = 0
        private var last = CACurrentMediaTime()
        private var sweepStart: CFTimeInterval?
        private var drawn = false

        /// The light sweep lasts this long.
        static let sweepSeconds: CFTimeInterval = 1.0

        func setAnimating(_ on: Bool) {
            target = on ? 1 : 0
            view?.isPaused = false
        }

        func startSweep() {
            // One request can reach here twice (SwiftUI may update before
            // it is cleared); a sweep just started is the same sweep.
            let now = CACurrentMediaTime()
            if let s = sweepStart, now - s < 0.1 { return }
            sweepStart = now
            view?.isPaused = false
        }

        func mtkView(_ view: MTKView, drawableSizeWillChange size: CGSize) {}

        func draw(in view: MTKView) {
            guard let r = renderer, let drawable = view.currentDrawable,
                  let cmd = r.commandQueue.makeCommandBuffer() else { return }
            let now = CACurrentMediaTime()
            let dt = Float(min(max(now - last, 0), 0.1))
            last = now
            r.motion += (target - r.motion) * min(1, dt * 2.5)
            if abs(r.motion - target) < 0.002 { r.motion = target }
            r.phase += dt * r.motion
            if let s = sweepStart {
                let p = (now - s) / Self.sweepSeconds
                if p >= 1 {
                    r.sweep = -1
                    sweepStart = nil
                } else {
                    r.sweep = Float(max(p, 0))
                }
            }
            r.encode(cmd, output: drawable.texture, style: .markFit,
                     pointSize: Float(view.bounds.width))
            cmd.present(drawable)
            cmd.commit()
            if !drawn {
                drawn = true
                // The frame is presented at the next refresh; hide the
                // still on the next run-loop turn, after it.
                DispatchQueue.main.async { [weak self] in self?.showing() }
            }
            if target == 0, r.motion == 0, sweepStart == nil {
                view.isPaused = true
                settled()
            }
        }
    }
}
