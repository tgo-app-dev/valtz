import AVFoundation
import AppKit
import SwiftUI

/// An asset opened large in the Atlas, played (AtlasExpanded): its
/// player and where it is, in the units the editor's transport counts --
/// a clip's frames at its rate, a sound's milliseconds -- so the editor's
/// own controls drive it (TransportButtons, ScrubberRow): play / pause,
/// a frame or 10 ms a step, fast forward and rewind, the start and the
/// end, the sound off or on, and Loop.
@MainActor @Observable
final class AtlasPlayback {
    let player: AVPlayer
    let units: FrameRate
    let sound: Bool
    private(set) var frame = 0
    private(set) var length = 0
    private(set) var rate: Float = 0
    private(set) var muted = false
    var loops = false
    @ObservationIgnored private var tick: Any?
    @ObservationIgnored private var end: NSObjectProtocol?

    init(url: URL, units: FrameRate, sound: Bool) {
        player = AVPlayer(url: url)
        self.units = units
        self.sound = sound
        tick = player.addPeriodicTimeObserver(
            forInterval: CMTime(value: 1, timescale: 30), queue: .main
        ) { [weak self] t in
            MainActor.assumeIsolated { self?.update(t) }
        }
        end = NotificationCenter.default.addObserver(
            forName: AVPlayerItem.didPlayToEndTimeNotification,
            object: player.currentItem, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.ended() }
            }
        // Its length at once: the time ticks come only as it plays, so
        // the scrubber read 0 to 0 until Play.
        let asset = player.currentItem?.asset
        Task { [weak self] in
            guard let d = try? await asset?.load(.duration) else { return }
            let s = d.seconds
            guard let self, s.isFinite, s > 0 else { return }
            self.length = max(1, Int((s * self.fps).rounded()))
        }
    }

    /// Let go: nothing plays on, nothing watches it.
    func close() {
        player.pause()
        if let tick { player.removeTimeObserver(tick) }
        tick = nil
        if let end { NotificationCenter.default.removeObserver(end) }
        end = nil
    }

    private var fps: Double { max(1e-6, units.fps) }

    private func update(_ t: CMTime) {
        if let d = player.currentItem?.duration.seconds, d.isFinite, d > 0 {
            length = max(1, Int((d * fps).rounded()))
        }
        let s = t.seconds
        if s.isFinite {
            frame = min(max(0, Int((s * fps + 1e-6).rounded(.down))),
                        max(0, length - 1))
        }
        rate = player.rate
    }

    private func ended() {
        if loops {
            seek(to: 0)
            player.rate = 1
            rate = 1
        } else {
            player.pause()
            rate = 0
        }
    }

    private func time(_ n: Int) -> CMTime {
        CMTime(value: CMTimeValue(n) * CMTimeValue(units.den),
               timescale: CMTimeScale(units.num))
    }

    func seek(to n: Int) {
        let n = min(max(0, n), max(0, length - 1))
        frame = n
        player.seek(to: time(n), toleranceBefore: .zero,
                    toleranceAfter: .zero)
    }

    func play(rate r: Float) {
        // At its end, played from the start again, as any player does.
        if r > 0, length > 1, frame >= length - 1 { seek(to: 0) }
        player.rate = r
        rate = r
    }

    func playPause() { play(rate: rate != 0 ? 0 : 1) }

    /// A frame on or back; a sound 10 ms, 1 ms with ⌥ held.
    func step(_ n: Int) {
        player.pause()
        rate = 0
        let fine = NSEvent.modifierFlags.contains(.option)
        seek(to: frame + (sound ? n * (fine ? 1 : 10) : n))
    }

    func fastForward() { play(rate: rate >= 2 ? min(rate * 2, 8) : 2) }

    func fastRewind() {
        guard player.currentItem?.canPlayReverse == true else { return }
        play(rate: rate <= -2 ? max(rate * 2, -8) : -2)
    }

    /// |< >|: the start, the last frame.
    func toEnd(_ last: Bool) {
        player.pause()
        rate = 0
        seek(to: last ? length - 1 : 0)
    }

    func scrubbed(to n: Int, resume: Float) {
        seek(to: n)
        if resume != 0 { play(rate: resume) }
    }

    func toggleMute() {
        player.isMuted.toggle()
        muted = player.isMuted
    }

    /// Its keys, as the stage's are (AppModel.trimKey, playerKey): Space,
    /// ← → (⇧ ten), ⌘← ⌘→ and Home / End the start and the end. Handled:
    /// true.
    func key(_ e: NSEvent) -> Bool {
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
            .subtracting([.numericPad, .function, .capsLock])
        switch e.keyCode {
        case 49 where mods.isEmpty:
            if !e.isARepeat { playPause() }
        case 115 where mods.isEmpty: toEnd(false)
        case 119 where mods.isEmpty: toEnd(true)
        case 123, 124:
            let next = e.keyCode == 124
            if mods == .command {
                toEnd(next)
            } else if mods.subtracting([.shift, .option]).isEmpty {
                let n = mods.contains(.shift) ? 10 : 1
                step(next ? n : -n)
            } else {
                return false
            }
        default: return false
        }
        return true
    }
}

extension AppModel {
    /// The Atlas's keys, on its screen: the player opened large's (as the
    /// stage's), Space a selected card's play / stop -- never the stage's
    /// player, out of sight behind it. Wherever the keyboard is but text,
    /// a list or a control.
    func atlasKey(_ e: NSEvent) -> Bool {
        guard screen == .atlas, let w = e.window, !(w is NSPanel),
              editorWindow == nil || w === editorWindow else { return false }
        let fr = w.firstResponder
        if fr is NSText || fr is NSTableView || fr is NSOutlineView
            || fr is NSCollectionView || fr is NSControl { return false }
        if let pb = atlasPlayback { return pb.key(e) }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
            .subtracting([.numericPad, .function, .capsLock])
        guard atlasExpanded == nil, e.keyCode == 49, mods.isEmpty,
              let s = atlasSelected, let i = atlas?.index[s],
              let id = atlas?.graph.nodes[i].asset,
              let a = asset(id: id), atlasPlayable(a) else { return false }
        if !e.isARepeat {
            if atlasPlaying == id { atlasStop() } else { atlasPlay(id) }
        }
        return true
    }
}

/// The editor's controls under an asset opened large in the Atlas: the
/// scrubber, then Loop and the transport, as the stage's row has them.
struct AtlasTransport: View {
    let playback: AtlasPlayback

    var body: some View {
        VStack(spacing: 8) {
            ScrubberRow(frame: playback.frame, length: playback.length,
                        units: playback.units,
                        seek: { playback.seek(to: $0) },
                        letGo: { playback.scrubbed(to: $0, resume: $1) },
                        rate: { playback.rate },
                        step: { playback.step($0) })
            HStack(spacing: 10) {
                TitleBarButton(symbol: "repeat", title: "Loop",
                               help: playback.loops
                                   ? "Play it once"
                                   : "Play it again and again",
                               on: playback.loops) {
                    playback.loops.toggle()
                }
                .titleBarCapsule()
                TransportButtons(
                    rate: playback.rate, muted: playback.muted,
                    sound: playback.sound, events: false,
                    act: .init(
                        mute: playback.toggleMute,
                        previous: { playback.toEnd(false) },
                        rewind: playback.fastRewind,
                        stepBack: { playback.step(-1) },
                        playPause: playback.playPause,
                        stepForward: { playback.step(1) },
                        forward: playback.fastForward,
                        next: { playback.toEnd(true) }))
            }
        }
    }
}
