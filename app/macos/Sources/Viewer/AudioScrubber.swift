import AVFoundation

/// A step's SOUND (the Trim panel's frame steps, ← and →): the stretch of
/// audio at the frame stepped onto, as the player sounds it -- its tracks
/// through its audio mix and its pitch algorithm -- read apart by an
/// asset reader over just that range and played on an engine of its own,
/// so the picture stays on the frame. A step cuts the one before.
@MainActor
final class AudioScrubber {
    private let engine = AVAudioEngine()
    private let node = AVAudioPlayerNode()
    private let format = AVAudioFormat(standardFormatWithSampleRate: 48_000,
                                       channels: 2)!
    private var attached = false
    private var ticket = 0
    /// The samples the last step played, and their loudest (a snapshot
    /// hook reads them).
    static var lastFrames: Int = 0
    static var lastPeak: Float = 0

    /// `seconds` of `item`'s sound from `time`.
    func play(_ item: AVPlayerItem, at time: CMTime, seconds: Double) {
        ticket += 1
        let mine = ticket
        let asset = item.asset
        let mix = item.audioMix
        let pitch = item.audioTimePitchAlgorithm
        Task { @MainActor [weak self] in
            guard let tracks = try? await asset.loadTracks(
                      withMediaType: .audio), !tracks.isEmpty,
                  let self, mine == self.ticket,
                  let buf = Self.read(asset, tracks, mix, pitch, at: time,
                                      seconds: seconds, format: self.format)
            else { return }
            Self.lastFrames = Int(buf.frameLength)
            Self.lastPeak = (0..<Int(buf.format.channelCount)).map { ch in
                (0..<Int(buf.frameLength)).reduce(Float(0)) {
                    max($0, abs(buf.floatChannelData![ch][$1]))
                }
            }.max() ?? 0
            self.schedule(buf)
        }
    }

    func stop() {
        ticket += 1
        if attached && engine.isRunning { node.stop() }
    }

    /// Played on an engine that RUNS and whose output has TURNED. A player
    /// node played otherwise fails -- on a stopped engine (AVAudioEngine
    /// stops itself when the sound's output changes: a device unplugged or
    /// switched), or before the output's first IO cycle ("player did not
    /// see an IO cycle", -10863: right after a start, or never, with no
    /// output running) -- and the deprecated play() RAISES then, an
    /// Objective-C exception Swift cannot catch: a frame's step aborted
    /// Valtz on macOS 27 (2026-10-18). The engine is started again when
    /// stopped, the play waits a moment for the output's first cycle (a
    /// few milliseconds on a Mac whose sound runs) and is skipped without
    /// one -- the step silent; macOS 27's throwing play reports what is
    /// left.
    private func schedule(_ buf: AVAudioPCMBuffer, tries: Int = 0) {
        if !attached {
            engine.attach(node)
            engine.connect(node, to: engine.mainMixerNode, format: format)
            attached = true
        }
        if !engine.isRunning {
            do { try engine.start() } catch { return }
            guard engine.isRunning else { return }
        }
        guard engine.outputNode.lastRenderTime?.isSampleTimeValid == true
        else {
            // About 0.3 s at most, the step's own sound still the latest.
            guard tries < 10 else { return }
            let mine = ticket
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .milliseconds(30))
                guard let self, mine == self.ticket else { return }
                self.schedule(buf, tries: tries + 1)
            }
            return
        }
        node.stop()
        node.scheduleBuffer(buf, completionHandler: nil)
        if #available(macOS 27.0, *) {
            try? node.playAudio()
        } else {
            node.play()
        }
    }

    /// The range decoded to 48 kHz stereo float, faded in and out over 3
    /// ms so it neither clicks where it starts nor where it stops.
    private static func read(_ asset: AVAsset, _ tracks: [AVAssetTrack],
                             _ mix: AVAudioMix?,
                             _ pitch: AVAudioTimePitchAlgorithm,
                             at time: CMTime, seconds: Double,
                             format: AVAudioFormat) -> AVAudioPCMBuffer? {
        guard let reader = try? AVAssetReader(asset: asset) else { return nil }
        let out = AVAssetReaderAudioMixOutput(audioTracks: tracks,
                                              audioSettings: [
            AVFormatIDKey: kAudioFormatLinearPCM,
            AVLinearPCMBitDepthKey: 32,
            AVLinearPCMIsFloatKey: true,
            AVLinearPCMIsNonInterleaved: true,
            AVLinearPCMIsBigEndianKey: false,
            AVSampleRateKey: 48_000,
            AVNumberOfChannelsKey: 2,
        ])
        out.audioMix = mix
        out.audioTimePitchAlgorithm = pitch
        guard reader.canAdd(out) else { return nil }
        reader.add(out)
        reader.timeRange = CMTimeRange(
            start: time,
            duration: CMTime(seconds: seconds, preferredTimescale: 48_000))
        guard reader.startReading() else { return nil }
        defer { reader.cancelReading() }
        let want = AVAudioFrameCount(seconds * 48_000)
        guard want > 0,
              let all = AVAudioPCMBuffer(pcmFormat: format,
                                         frameCapacity: want) else {
            return nil
        }
        let channels = Int(format.channelCount)
        while all.frameLength < all.frameCapacity,
              let sb = out.copyNextSampleBuffer() {
            let n = AVAudioFrameCount(CMSampleBufferGetNumSamples(sb))
            guard n > 0,
                  let part = AVAudioPCMBuffer(pcmFormat: format,
                                              frameCapacity: n) else {
                continue
            }
            part.frameLength = n
            guard CMSampleBufferCopyPCMDataIntoAudioBufferList(
                      sb, at: 0, frameCount: Int32(n),
                      into: part.mutableAudioBufferList) == noErr,
                  let src = part.floatChannelData,
                  let dst = all.floatChannelData else { continue }
            let room = min(n, all.frameCapacity - all.frameLength)
            for ch in 0..<channels {
                (dst[ch] + Int(all.frameLength))
                    .update(from: src[ch], count: Int(room))
            }
            all.frameLength += room
        }
        let n = Int(all.frameLength)
        guard n > 0, let data = all.floatChannelData else { return nil }
        let ramp = min(n / 2, 144)
        for ch in 0..<channels {
            let p = data[ch]
            for i in 0..<ramp {
                let g = Float(i) / Float(ramp)
                p[i] *= g
                p[n - 1 - i] *= g
            }
        }
        return all
    }
}
