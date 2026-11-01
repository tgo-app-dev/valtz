import AVFoundation
import SwiftUI

/// The VOICE-OVER (DESIGN §7b): the audio target's attach menu, while a
/// timeline -- or a composition of sound -- is active on the stage,
/// records TAKES onto it as it plays. A take starts where the player is:
/// the microphone (the card's source) first, then the stage plays at 1x,
/// muted, from a moment the player names on the host's clock -- the core
/// lines the recording up by it (Controller::cue_capture) -- the scrubber,
/// the transport and their keys held meanwhile. Stopped, the player
/// pauses, its sound as it was, and the take lands as a new layer where it
/// was heard, one command. The mode stays for the next take until its
/// (x); the prompt is not read in it.
extension AppModel {
    /// What a voice-over goes onto: the active composition on the stage,
    /// in time -- a timeline, a composition of sound holding something
    /// (a blank one records into itself), not a still's pages -- from the
    /// audio target's box, outside the Prompt Editor.
    var voiceOverTarget: AssetDTO? {
        guard activeModality == .audio, !promptImmersive, !agentHolds,
              clipOnStage, captureTarget == nil,
              let t = timelineAsset, !t.isPaged else { return nil }
        return t
    }

    var canVoiceOver: Bool { voiceOverTarget != nil && attachCapture == nil }

    /// The mode is on: its card shown, the prompt not read.
    var inVoiceOver: Bool { attachCapture == .voiceOver }

    /// A take under way: from Record until it has landed.
    var voiceOverRecording: Bool { inVoiceOver && voiceOverFrom != nil }

    /// The mode on, for the composition on the stage: the default
    /// microphone its source, unless one was chosen before.
    func openVoiceOver() {
        guard let t = voiceOverTarget, attachCapture == nil else { return }
        refreshCaptureSources()
        if !captureSources.contains(where: { $0.id == voiceOverSource }) {
            voiceOverSource = captureSources.first {
                $0.kind == "microphone"
            }?.id ?? captureSources.first?.id ?? ""
        }
        guard !voiceOverSource.isEmpty else {
            flash(String(localized: "There is no microphone."),
                  level: .warning)
            return
        }
        voiceOverComposition = t.id
        voiceOverTakes = 0
        voiceOverLast = nil
        withAnimation(Self.motion) { attachCapture = .voiceOver }
    }

    /// A take from where the player is.
    func voiceOverRecord() {
        guard inVoiceOver, voiceOverFrom == nil, !capture.recording,
              let core, let projectId, let comp = voiceOverComposition,
              voiceOverTarget?.id == comp else { return }
        let from = max(0, videoFrame)
        let source = voiceOverSource
        let mic = captureSources.first { $0.id == source }?.kind
            == "microphone"
        // Under way from the press: the transport held from now.
        voiceOverFrom = from
        Task { @MainActor [weak self] in
            if mic, !(await Self.allowed(.audio)) {
                self?.voiceOverFrom = nil
                self?.flash(String(localized: "Valtz may not use the microphone: allow it in System Settings › Privacy & Security › Microphone."), level: .warning)
                return
            }
            // ScreenCaptureKit answers asynchronously: off the main queue.
            let r = await Task.detached {
                core.captureOp(["op": "start", "project": projectId,
                                "asset": comp, "source": source,
                                "voice_over_at": from])
            }.value
            guard let self else { return }
            // Left meanwhile (its (x), the stage elsewhere): not kept.
            guard self.inVoiceOver, self.voiceOverFrom == from else {
                if r.ok { _ = core.captureOp(["op": "cancel"]) }
                return
            }
            guard r.ok else {
                self.voiceOverFrom = nil
                self.flash(r.message, level: .error)
                return
            }
            self.voiceOverMutedBefore = self.videoMuted
            self.sendVideo(.cued(1, muted: true))
            self.pollCapture()
        }
    }

    /// The player began at `host`: the take lined up by it.
    func voiceOverCued(_ host: Double) {
        lastCue = host
        guard voiceOverRecording, let core else { return }
        _ = core.captureOp(["op": "cue", "host": host])
    }

    /// Stopped: the player paused, its sound as it was, the take on the
    /// timeline where it was heard.
    func voiceOverStop() {
        guard voiceOverRecording, let core else { return }
        capturePoll?.cancel()
        sendVideo(.cued(0, muted: voiceOverMutedBefore))
        let from = voiceOverFrom
        Task { @MainActor [weak self] in
            let r = await Task.detached { core.captureOp(["op": "stop"]) }
                .value
            guard let self else { return }
            self.capture = CaptureStateDTO()
            self.voiceOverFrom = nil
            guard r.ok else {
                self.flash(r.message, level: .error)
                return
            }
            self.voiceOverTakes += 1
            self.voiceOverLast = from
            self.layersChanged()
        }
    }

    /// Its (x): a take under way dropped -- nothing of it kept -- and the
    /// mode gone.
    func closeVoiceOver() {
        if voiceOverRecording {
            if capture.recording { cancelCapture() }
            sendVideo(.cued(0, muted: voiceOverMutedBefore))
            voiceOverFrom = nil
        }
        voiceOverComposition = nil
        withAnimation(Self.motion) { attachCapture = nil }
    }

    /// The stage or the box gone elsewhere -- another asset viewed,
    /// another target, the Prompt Editor, another project: a take under
    /// way lands as if stopped, and the mode goes.
    func voiceOverLostTarget() {
        guard inVoiceOver else { return }
        if voiceOverRecording && capture.recording { voiceOverStop() }
        voiceOverFrom = nil
        voiceOverComposition = nil
        withAnimation(Self.motion) { attachCapture = nil }
    }
}

/// The voice-over's row in the attach card: the microphone, where the
/// take starts -- while one runs, its time and level -- and Record /
/// Stop; the card's (x) leaves the mode.
struct VoiceOverRow: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 10) {
            icon
            Text("Voice-over")
                .fontWeight(.medium)
            source
            state
            Spacer(minLength: 8)
            recordButton
        }
        // The stage or the box gone elsewhere: the mode goes, a take
        // under way kept.
        .onChange(of: model.voiceOverTarget?.id) { _, now in
            if now != model.voiceOverComposition {
                model.voiceOverLostTarget()
            }
        }
    }

    private var icon: some View {
        let recording = model.voiceOverRecording
        return Image(systemName: recording ? "waveform" : "mic.fill")
            .foregroundStyle(recording ? Color.red : Color.secondary)
            .symbolEffect(.variableColor.iterative, isActive: recording)
            .frame(width: 18)
    }

    private var source: some View {
        Picker("Source", selection: $model.voiceOverSource) {
            ForEach(model.captureSources) { s in
                Text(verbatim: s.name).tag(s.id)
            }
        }
        .labelsHidden()
        .frame(maxWidth: 190)
        .disabled(model.voiceOverRecording)
    }

    /// While a take runs, its time and level; else where the next starts
    /// -- or where the last went.
    @ViewBuilder private var state: some View {
        let rate = model.stageFrameRate
        if model.voiceOverRecording {
            Text(verbatim: model.captureTime)
                .monospacedDigit()
                .foregroundStyle(.secondary)
            LevelBar(level: model.capture.level)
                .frame(width: 90, height: 6)
        } else if let last = model.voiceOverLast, model.voiceOverTakes > 0 {
            let place = rate.timecode(last)
            Text("Take \(String(model.voiceOverTakes)) added at \(place)")
                .foregroundStyle(.secondary)
                .lineLimit(1)
        } else {
            let place = rate.timecode(max(0, model.videoFrame))
            Text("From \(place)")
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
    }

    private var recordButton: some View {
        let recording = model.voiceOverRecording
        return Button {
            if recording {
                model.voiceOverStop()
            } else {
                model.voiceOverRecord()
            }
        } label: {
            Text(recording ? "Stop" : "Record").frame(minWidth: 70)
        }
        .startButtonStyle()
        .keyboardShortcut(.defaultAction)
        .help(recording
              ? "Stop: the take goes on the timeline where it was heard"
              : "Record a take: the timeline plays from here, muted, as you speak")
    }
}
