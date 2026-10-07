import AVFoundation
import SwiftUI

/// What the attach menu CAPTURES for the prompt's row (DESIGN §7b): a
/// sound -- a microphone, the system's audio -- or a still or a clip from
/// a camera. Each lands as an asset of the project, staged in the row as
/// a file attached would be; a card over the prompt card runs it.
enum AttachCapture: Equatable, Sendable {
    case sound
    case camera
}

/// The camera as the core reports it (Controller::camera_state).
struct CameraStateDTO: Decodable, Sendable, Equatable {
    var on = false
    var name: String?
    var recording = false
    var seconds: Double = 0
    var width = 0
    var height = 0
    var frames = 0
}

extension AppModel {
    /// A sound recorded for the prompt: the default microphone, or
    /// ("system") the system's audio.
    func attachSound(system: Bool) {
        guard let core, let projectId, attachCapture == nil,
              !capture.recording else { return }
        refreshCaptureSources()
        guard let source = captureSources.first(where: {
            $0.kind == (system ? "system" : "microphone")
        })?.id else {
            flash(String(localized: "There is no microphone."))
            return
        }
        Task { @MainActor [weak self] in
            if !system {
                guard await Self.allowed(.audio) else {
                    self?.flash(String(localized: "Valtz may not use the microphone: allow it in System Settings › Privacy & Security › Microphone."))
                    return
                }
            }
            // ScreenCaptureKit answers asynchronously: off the main queue.
            let r = await Task.detached {
                core.captureOp(["op": "start", "project": projectId,
                                "source": source])
            }.value
            guard let self else { return }
            guard r.ok else {
                self.flash(r.message)
                return
            }
            withAnimation(Self.motion) { self.attachCapture = .sound }
            self.pollCapture()
        }
    }

    /// The recording stopped: a sound of the project's, in the row.
    func finishAttachSound() {
        guard let core, capture.recording else { return }
        capturePoll?.cancel()
        Task { @MainActor [weak self] in
            let r = await Task.detached { core.captureOp(["op": "stop"]) }
                .value
            guard let self else { return }
            self.capture = CaptureStateDTO()
            guard r.ok, let id = r["asset"] as? String else {
                withAnimation(Self.motion) { self.attachCapture = nil }
                self.flash(r.message)
                return
            }
            self.landCapture(id)
        }
    }

    /// What was captured into the row: the card becomes its thumbnail.
    func landCapture(_ id: String) {
        reloadAssets()
        capturedThumbFrame = nil
        capturedItem = addAssetReferences([id]).first
        // The card goes at once: its ghost takes its place and flies into
        // the thumbnail (ComposerStack).
        attachCapture = nil
        // A thumbnail never laid out (the row out of sight): shown anyway.
        let item = capturedItem
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(2 * Self.animationScale))
            if let self, self.capturedItem == item {
                self.capturedItem = nil
                self.capturedThumbFrame = nil
            }
        }
    }

    func cancelAttachSound() {
        cancelCapture()
        withAnimation(Self.motion) { attachCapture = nil }
    }

    /// The camera on, for a still or a clip (its microphone asked for
    /// too: a clip's sound).
    func attachCamera() {
        guard let core, let projectId, attachCapture == nil else { return }
        Task { @MainActor [weak self] in
            guard await Self.allowed(.video) else {
                self?.flash(String(localized: "Valtz may not use the camera: allow it in System Settings › Privacy & Security › Camera."))
                return
            }
            _ = await Self.allowed(.audio)
            let r = await Task.detached {
                core.cameraOp(["op": "start", "project": projectId,
                               "sound": true])
            }.value
            guard let self else { return }
            guard r.ok else {
                self.flash(r.message)
                return
            }
            withAnimation(Self.motion) { self.attachCapture = .camera }
            self.pollCamera()
        }
    }

    /// A still of what the camera shows, in the row; the camera off.
    func cameraSnap() {
        cameraTake("snap")
    }

    func cameraRecord() {
        guard let core else { return }
        let r = core.cameraOp(["op": "record"])
        if !r.ok { flash(r.message) }
    }

    /// The clip stopped: in the row; the camera off.
    func cameraStopRecording() {
        cameraTake("stop-recording")
    }

    private func cameraTake(_ op: String) {
        guard let core else { return }
        Task { @MainActor [weak self] in
            let r = await Task.detached { core.cameraOp(["op": op]) }.value
            guard let self else { return }
            self.cameraOff()
            guard r.ok, let id = r["asset"] as? String else {
                withAnimation(Self.motion) { self.attachCapture = nil }
                self.flash(r.message)
                return
            }
            self.landCapture(id)
        }
    }

    /// The camera off, nothing taken (a clip being recorded dropped).
    func closeCamera() {
        cameraOff()
        withAnimation(Self.motion) { attachCapture = nil }
    }

    private func cameraOff() {
        cameraPoll?.cancel()
        _ = core?.cameraOp(["op": "stop"])
        camera = CameraStateDTO()
    }

    /// The card's (x): the capture let go, nothing kept.
    func abortAttachCapture() {
        switch attachCapture {
        case .camera: closeCamera()
        case .sound: cancelAttachSound()
        case nil: break
        }
    }

    /// The camera's state -- recording, its time -- four times a second.
    private func pollCamera() {
        cameraPoll?.cancel()
        cameraPoll = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self, let core = self.core else { return }
                let r = core.cameraOp(["op": "state"])
                let s = DTO.decode(CameraStateDTO.self, r.payload)
                    ?? CameraStateDTO()
                if s != self.camera { self.camera = s }
                if !s.on { return }
                try? await Task.sleep(for: .milliseconds(250))
            }
        }
    }

    /// "0:07": a clip's length so far.
    var cameraTime: String {
        let s = Int(max(0, camera.seconds))
        return String(format: "%d:%02d", s / 60, s % 60)
    }

    /// macOS's leave for a device: asked when it has not been (its own
    /// prompt), else as given.
    nonisolated static func allowed(_ media: AVMediaType) async -> Bool {
        switch AVCaptureDevice.authorizationStatus(for: media) {
        case .authorized: return true
        case .notDetermined:
            return await AVCaptureDevice.requestAccess(for: media)
        default: return false
        }
    }
}

/// The attach menu's capture, in the prompt card's row region -- a row of
/// its own after the references: a sound's recorder (its source, time
/// and level) or the camera's picture, live, with Take Photo and Record.
/// Its (x) lets it go; what it captures, it BECOMES -- the card flies
/// into the new reference's thumbnail (ComposerStack's ghost).
struct AttachCaptureCard: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 6) {
            switch model.attachCapture {
            case .sound: sound
            case .camera: cameraView
            case nil: EmptyView()
            }
            abort
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 10, style: .continuous)
            .fill(Color.primary.opacity(0.05)))
        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
            .strokeBorder(Color.primary.opacity(0.1), lineWidth: 0.5))
    }

    /// The (x), inside on the right, vertically centred -- the prompt's
    /// own Clear (which goes while the card is here): nothing kept.
    private var abort: some View {
        Button(action: model.abortAttachCapture) {
            Image(systemName: "xmark.circle.fill")
                .font(.system(size: 14))
                .frame(width: 26, height: 26)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.tertiary)
        .keyboardShortcut(.cancelAction)
        .help(model.attachCapture == .camera
              ? "Close the camera: nothing is kept"
              : "Stop recording: nothing is kept")
        .accessibilityLabel(Text(model.attachCapture == .camera
                                 ? "Close the camera" : "Stop recording"))
    }

    private var sound: some View {
        HStack(spacing: 10) {
            Circle().fill(Color.red).frame(width: 9, height: 9)
                .opacity(model.capture.recording ? 1 : 0.3)
            Text(verbatim: model.capture.name ?? "")
                .lineLimit(1)
            Text(verbatim: model.captureTime)
                .monospacedDigit()
                .foregroundStyle(.secondary)
            LevelBar(level: model.capture.level)
                .frame(width: 120, height: 6)
            Spacer(minLength: 12)
            Button(action: model.finishAttachSound) {
                Text("Stop and Attach").frame(minWidth: 70)
            }
            .startButtonStyle()
            .keyboardShortcut(.defaultAction)
        }
    }

    private var cameraView: some View {
        HStack(alignment: .top, spacing: 12) {
            CameraPreview(model: model)
                .aspectRatio(model.camera.width > 0 && model.camera.height > 0
                             ? CGFloat(model.camera.width)
                                 / CGFloat(model.camera.height)
                             : 16 / 9, contentMode: .fit)
                .frame(height: 150)
                .background(Color.black)
                .clipShape(RoundedRectangle(cornerRadius: 8,
                                            style: .continuous))
                .overlay(alignment: .topLeading) {
                    if model.camera.recording {
                        HStack(spacing: 6) {
                            Circle().fill(Color.red).frame(width: 8,
                                                           height: 8)
                            Text(verbatim: model.cameraTime).monospacedDigit()
                        }
                        .font(.callout)
                        .foregroundStyle(.white)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(Capsule().fill(.black.opacity(0.5)))
                        .padding(8)
                    }
                }
            VStack(alignment: .leading, spacing: 8) {
                Text(verbatim: model.camera.name ?? "")
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Spacer(minLength: 0)
                HStack(spacing: 8) {
                    if model.camera.recording {
                        Button(action: model.cameraStopRecording) {
                            Text("Stop and Attach").frame(minWidth: 70)
                        }
                        .startButtonStyle()
                        .keyboardShortcut(.defaultAction)
                    } else {
                        Button(action: model.cameraRecord) {
                            Text("Record Video").frame(minWidth: 70)
                        }
                        .startButtonStyle()
                        Button(action: model.cameraSnap) {
                            Text("Take Photo").frame(minWidth: 70)
                        }
                        .startButtonStyle()
                        .keyboardShortcut(.defaultAction)
                    }
                }
            }
            .frame(height: 150)
            Spacer(minLength: 0)
        }
    }
}

/// A sound's level, 0...1: a bar that fills.
private struct LevelBar: View {
    let level: Double

    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.1))
                // The app's accent (black; white in dark), as its other
                // meters.
                Capsule().fill(Color.accentColor)
                    .frame(width: g.size.width * min(1, max(0, level)))
            }
        }
    }
}

/// The camera's picture, live: its newest frame's surface on a layer, 30
/// times a second, mirrored as a mirror shows you (what is taken is not).
private struct CameraPreview: NSViewRepresentable {
    let model: AppModel

    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        v.wantsLayer = true
        let c = context.coordinator
        c.layer.contentsGravity = .resizeAspect
        c.layer.setAffineTransform(CGAffineTransform(scaleX: -1, y: 1))
        c.core = model.core
        v.layer = c.layer
        c.timer = Timer.scheduledTimer(withTimeInterval: 1.0 / 30,
                                       repeats: true) { _ in
            MainActor.assumeIsolated { c.tick() }
        }
        return v
    }

    func updateNSView(_ v: NSView, context: Context) {}

    static func dismantleNSView(_ v: NSView, coordinator: Coordinator) {
        coordinator.timer?.invalidate()
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    @MainActor
    final class Coordinator {
        let layer = CALayer()
        var core: CoreService?
        var timer: Timer?

        func tick() {
            guard let surface = core?.cameraFrame() else { return }
            // The same surface again holds a newer frame: let go first.
            if layer.contents as AnyObject? === surface {
                layer.contents = nil
            }
            layer.contents = surface
        }
    }
}
