import AVFoundation
import SwiftUI

/// A source sound can be recorded from (core media::CaptureSource): a
/// microphone, or the system's audio -- every app's but Valtz's.
struct CaptureSourceDTO: Decodable, Sendable, Hashable, Identifiable {
    var id: String
    var name: String
    var kind: String       // "microphone" | "system"
    var preferred: Bool
}

/// A recording as the core reports it (Controller::capture_state).
struct CaptureStateDTO: Decodable, Sendable, Equatable {
    var recording = false
    var asset: String?
    var source: String?
    var name: String?
    var seconds: Double = 0
    var level: Double = 0
}

/// Sound CAPTURED (DESIGN §7b): a blank composition of sound alone on the
/// stage records into itself -- a microphone, or the system's audio as
/// the Screenshot app records it. Stopped, the recording is a sound of
/// the project's, on the composition's layer 0.
extension AppModel {
    /// The blank composition of sound alone on the stage: nothing on it
    /// yet, so it records.
    var captureTarget: AssetDTO? {
        guard clipOnStage, let id = stageAssetId,
              let c = assets.first(where: { $0.id == id }),
              c.isTimeline, c.kind == "audio",
              (c.layers ?? []).allSatisfy(\.isEmpty) else { return nil }
        return c
    }

    /// The stage shows the capture controls: on a blank sound, or while
    /// it records.
    var showsCapture: Bool {
        captureTarget != nil
            || (capture.recording && !(capture.asset ?? "").isEmpty)
    }

    /// The sources there now: the microphones (the default first), then
    /// the system's audio; the one chosen kept while it is there.
    func refreshCaptureSources() {
        guard let core else { return }
        let r = core.captureSources()
        guard r.ok,
              let list = DTO.decode([CaptureSourceDTO].self, r["sources"])
        else { return }
        captureSources = list
        if !list.contains(where: { $0.id == captureSource }) {
            captureSource = list.first?.id ?? ""
        }
    }

    /// Start recording into the composition on the stage. A microphone
    /// is asked for first (macOS's own prompt), as is the system's audio
    /// by its first use.
    func startCapture() {
        guard let core, let projectId, let target = captureTarget,
              !capture.recording else { return }
        let source = captureSource
        let mic = captureSources.first { $0.id == source }?.kind
            == "microphone"
        Task { @MainActor [weak self] in
            if mic {
                switch AVCaptureDevice.authorizationStatus(for: .audio) {
                case .notDetermined:
                    guard await AVCaptureDevice.requestAccess(for: .audio)
                    else { return }
                case .authorized:
                    break
                default:
                    self?.flash(String(localized: "Valtz may not use the microphone: allow it in System Settings › Privacy & Security › Microphone."))
                    return
                }
            }
            // ScreenCaptureKit answers asynchronously: off the main queue.
            let r = await Task.detached {
                core.captureOp(["op": "start", "project": projectId,
                                "asset": target.id, "source": source])
            }.value
            guard let self else { return }
            guard r.ok else {
                self.flash(r.message)
                return
            }
            self.pollCapture()
        }
    }

    /// The recording's time and level, ten times a second while it runs.
    func pollCapture() {
        capturePoll?.cancel()
        capturePoll = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self, let core = self.core else { return }
                let r = core.captureOp(["op": "state"])
                let s = DTO.decode(CaptureStateDTO.self, r.payload)
                    ?? CaptureStateDTO()
                if s != self.capture { self.capture = s }
                if !s.recording { return }
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
    }

    /// Stopped: the recording on the composition, which the stage then
    /// plays.
    func stopCapture() {
        guard let core, capture.recording else { return }
        capturePoll?.cancel()
        Task { @MainActor [weak self] in
            let r = await Task.detached { core.captureOp(["op": "stop"]) }
                .value
            guard let self else { return }
            self.capture = CaptureStateDTO()
            guard r.ok else {
                self.flash(r.message)
                return
            }
            self.layersChanged()
        }
    }

    /// Stopped and thrown away.
    func cancelCapture() {
        guard let core, capture.recording else { return }
        capturePoll?.cancel()
        _ = core.captureOp(["op": "cancel"])
        capture = CaptureStateDTO()
    }

    /// "0:07.4": a recording's length so far.
    var captureTime: String {
        let s = max(0, capture.seconds)
        return String(format: "%d:%04.1f", Int(s) / 60,
                      s.truncatingRemainder(dividingBy: 60))
    }
}

/// Over a blank sound on the stage: where it records from, Record /
/// Stop, and while it runs its time and level.
struct CaptureOverlay: View {
    @Bindable var model: AppModel

    var body: some View {
        let recording = model.capture.recording
        VStack(spacing: 14) {
            Image(systemName: recording ? "waveform" : "mic")
                .font(.system(size: 34, weight: .light))
                .foregroundStyle(recording ? Color.red : .secondary)
                .symbolEffect(.variableColor.iterative, isActive: recording)
            Picker("Source", selection: $model.captureSource) {
                ForEach(model.captureSources) { s in
                    Label(s.name, systemImage: s.kind == "system"
                          ? "macwindow" : "mic")
                        .tag(s.id)
                }
            }
            .labelsHidden()
            .frame(width: 260)
            .disabled(recording)
            .help("Record from a microphone, or the system's audio: what every app plays, Valtz's own left out")
            HStack(spacing: 12) {
                if recording {
                    Button {
                        model.stopCapture()
                    } label: {
                        Label("Stop", systemImage: "stop.fill")
                            .frame(minWidth: 70)
                    }
                    .buttonStyle(.glassProminent)
                    .tint(.red)
                    .keyboardShortcut(.space, modifiers: [])
                    Text(verbatim: model.captureTime)
                        .font(.title3.monospacedDigit())
                    LevelMeter(level: model.capture.level)
                        .frame(width: 120, height: 6)
                } else {
                    Button {
                        model.startCapture()
                    } label: {
                        Label("Record", systemImage: "record.circle")
                            .frame(minWidth: 70)
                    }
                    .buttonStyle(.glassProminent)
                    .tint(.red)
                    .disabled(model.captureSource.isEmpty)
                }
            }
            Text(recording
                 ? "Stop puts the recording on this composition."
                 : "An empty sound: record into it.")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .padding(22)
        .glassEffect(in: RoundedRectangle(cornerRadius: 18))
        // Over the whole stage: a blank sound has nothing to play.
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color.black.opacity(0.92))
        .onAppear { model.refreshCaptureSources() }
    }
}

/// A recording's level: green, amber near the top, red at it.
private struct LevelMeter: View {
    let level: Double

    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.12))
                Capsule()
                    .fill(level > 0.9 ? Color.red
                          : level > 0.6 ? Color.orange : Color.green)
                    .frame(width: g.size.width * min(1, max(0, level)))
                    .animation(.linear(duration: 0.1), value: level)
            }
        }
        .accessibilityLabel(Text("Level"))
    }
}
