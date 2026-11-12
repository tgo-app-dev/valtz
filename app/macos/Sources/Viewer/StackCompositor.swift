import AVFoundation
import CoreVideo
import os
import QuartzCore

/// What a stack's frames are drawn with -- the core and the plan -- as the
/// player plays, and the player item's clock. The plan is swapped while it
/// plays (the panels' live values); it is read on AVFoundation's threads.
final class StackBox: @unchecked Sendable {
    private let lock = NSLock()
    private var core: CoreService?
    private var plan: UInt64 = 0
    private var rate = FrameRate.fallback
    private var clock: CMTimebase?

    func set(core: CoreService?, plan: UInt64, rate: FrameRate) {
        lock.withLock {
            self.core = core
            self.plan = plan
            self.rate = rate
        }
    }

    /// The playing item's timebase: where the sound is.
    func set(clock: CMTimebase?) {
        lock.withLock { self.clock = clock }
    }

    func get() -> (core: CoreService?, plan: UInt64, rate: FrameRate,
                   clock: CMTimebase?) {
        lock.withLock { (core, plan, rate, clock) }
    }

    /// Frames drawn and skipped while playing, since the stack was put
    /// on (the snapshot summary's `stackFrames=`).
    private var drawn = 0
    private var skipped = 0

    /// The composition time of the newest frame drawn, seconds (the
    /// snapshot hook's `replan`: does the picture move on).
    private var drawnAt = -1.0

    func count(skipped skip: Bool, at: Double = -1) {
        lock.withLock {
            if skip { skipped += 1 } else { drawn += 1 }
            if !skip, at >= 0 { drawnAt = at }
        }
    }

    var counts: (drawn: Int, skipped: Int) {
        lock.withLock { (drawn, skipped) }
    }

    var lastDrawnAt: Double { lock.withLock { drawnAt } }

    func resetCounts() {
        lock.withLock { drawn = 0; skipped = 0; drawnAt = -1 }
    }

    /// The box of the stack on the stage (a dev and snapshot aid).
    @MainActor static weak var shown: StackBox?
}

/// The stack's one instruction: the whole timeline, every clip's track.
final class StackInstruction: NSObject, AVVideoCompositionInstructionProtocol,
    @unchecked Sendable {
    let timeRange: CMTimeRange
    let enablePostProcessing = false
    let containsTweening = true
    let requiredSourceTrackIDs: [NSValue]?
    let passthroughTrackID = kCMPersistentTrackID_Invalid
    /// The clips' tracks, in the order of the stack's video layers.
    let trackIDs: [CMPersistentTrackID]
    let box: StackBox

    init(timeRange: CMTimeRange, trackIDs: [CMPersistentTrackID],
         box: StackBox) {
        self.timeRange = timeRange
        self.trackIDs = trackIDs
        self.requiredSourceTrackIDs = trackIDs.map {
            NSNumber(value: $0) as NSValue
        }
        self.box = box
    }
}

/// A clip's STACK, played: AVFoundation decodes its clips -- one track
/// each, all starting at the timeline's start -- and for every frame this
/// hands them to the core, which composes the stack as an export does
/// (media::StackRenderer: each layer's tracks at that frame, the canvas)
/// and draws it into the frame AVFoundation shows, on the GPU. What plays
/// is what is made.
///
/// KEEPING UP WITH THE SOUND. The sound plays at its own pace whatever the
/// picture does; a frame that takes longer to draw than it is shown --
/// two hour-long 4K clips mixed, a heavy look -- must not hold the picture
/// back. Frames are drawn in order on a queue of their own, and while it
/// plays, one whose moment will have passed by the time it could be drawn
/// (the player's clock, plus how long drawing takes lately) is SKIPPED:
/// the frame on screen stays a frame longer, and the next one drawn is
/// one still to come. So the picture drops frames, never falls behind.
/// Paused, scrubbing or stepping, every frame asked for is drawn.
///
/// Sources come as YUV (the decoders' own, 1.5 to 3 bytes a pixel, not
/// BGRA's 4 -- per frame AVFoundation holds, per clip) -- but for a stack
/// with a clip that has alpha (StackCompositorAlpha: BGRA keeps it) --
/// and the frame is drawn at the composition's render size, which the
/// player keeps to the stage's pixels (StackCompositor.renderSize).
class StackCompositor: NSObject, AVVideoCompositing, @unchecked Sendable {
    fileprivate static let yuv: [String: any Sendable] = [
        kCVPixelBufferPixelFormatTypeKey as String: [
            kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            kCVPixelFormatType_32BGRA,
        ],
        kCVPixelBufferIOSurfacePropertiesKey as String: [String: Int](),
    ]
    fileprivate static let bgra: [String: any Sendable] = [
        kCVPixelBufferPixelFormatTypeKey as String:
            kCVPixelFormatType_32BGRA,
        kCVPixelBufferIOSurfacePropertiesKey as String: [String: Int](),
    ]
    private static let output = bgra

    var sourcePixelBufferAttributes: [String: any Sendable]? { Self.yuv }
    var requiredPixelBufferAttributesForRenderContext:
        [String: any Sendable] { Self.output }

    /// A new render size (the stage resized): the frame kept for skips is
    /// the old size, and goes.
    func renderContextChanged(_ newRenderContext:
                              AVVideoCompositionRenderContext) {
        queue.async { [self] in last = nil }
    }

    private let queue = DispatchQueue(label: "com.tgous.valtz.stack-frames",
                                      qos: .userInteractive)
    /// Touched on `queue` only, but for `generation` (any thread, locked).
    private var last: CVPixelBuffer?
    private var lastDrawn = 0.0     // when a frame was last drawn
    private var drawTime = 0.0      // seconds a frame takes, lately
    private let lock = NSLock()
    private var generation = 0

    /// A picture that stands still under its sound, said in the system log
    /// (category stack-player): each two seconds of playing in which a
    /// frame could not be drawn -- its plan let go, the core refusing it
    /// -- or more were skipped than drawn, with what they took. Reported
    /// (2026-11-07) while an agent worked and not reproduced: the next
    /// one says which (`/usr/bin/log show --predicate 'subsystem ==
    /// "com.tgous.valtz" && category == "stack-player"'`).
    private static let log = Logger(subsystem: "com.tgous.valtz",
                                    category: "stack-player")
    /// This window's counts (on `queue`).
    private var window = (start: 0.0, drawn: 0, skipped: 0, failed: 0,
                          slowest: 0.0)

    /// A frame counted, and the window said once it is two seconds old
    /// and anything in it was amiss.
    private func tally(drawn: Bool = false, skipped: Bool = false,
                       failed: Bool = false, took: Double = 0, plan: UInt64,
                       clock: Double) {
        let now = CACurrentMediaTime()
        if window.start == 0 { window.start = now }
        if drawn { window.drawn += 1 }
        if skipped { window.skipped += 1 }
        if failed { window.failed += 1 }
        window.slowest = max(window.slowest, took)
        guard now - window.start >= 2 else { return }
        let w = window
        window = (now, 0, 0, 0, 0)
        guard w.failed > 0 || w.skipped > w.drawn else { return }
        Self.log.notice("""
            stack frames in \(now - w.start, format: .fixed(precision: 1)) s: \
            drawn \(w.drawn), skipped \(w.skipped), failed \(w.failed) \
            (plan \(plan)); slowest draw \(Int(w.slowest * 1000)) ms, \
            lately \(Int(self.drawTime * 1000)) ms; clock \
            \(clock, format: .fixed(precision: 2)) s
            """)
    }

    func startRequest(_ request: AVAsynchronousVideoCompositionRequest) {
        let gen = lock.withLock { generation }
        queue.async { [self] in
            if lock.withLock({ gen != generation }) {
                request.finishCancelledRequest()
                return
            }
            draw(request)
        }
    }

    /// A seek: what was asked for before it is not wanted.
    func cancelAllPendingVideoCompositionRequests() {
        lock.withLock { generation += 1 }
    }

    private func draw(_ request: AVAsynchronousVideoCompositionRequest) {
        guard let ins = request.videoCompositionInstruction
                  as? StackInstruction else {
            request.finish(with: Self.failure)
            return
        }
        let (core, plan, rate, clock) = ins.box.get()
        let at = request.compositionTime
        let wall = CACurrentMediaTime()
        if let clock, let last, CMTimebaseGetRate(clock) != 0 {
            // Playing: drawn, would it still be in time? Its moment ends a
            // frame after it starts; the clock is where the sound is. At
            // least four frames a second are drawn, late or not.
            let now = CMTimebaseGetTime(clock).seconds
            let ends = at.seconds + 1 / max(1, rate.fps)
            if now + drawTime > ends, wall - lastDrawn < 0.25 {
                ins.box.count(skipped: true)
                tally(skipped: true, plan: plan, clock: now)
                request.finish(withComposedVideoFrame: last)
                return
            }
        }
        let clockAt = clock.map { CMTimebaseGetTime($0).seconds } ?? -1
        guard let out = request.renderContext.newPixelBuffer() else {
            tally(failed: true, plan: plan, clock: clockAt)
            request.finish(with: Self.failure)
            return
        }
        let frames = ins.trackIDs.map { request.sourceFrame(byTrackID: $0) }
        let n = rate.frame(at: at.seconds)
        guard let core, core.renderStack(plan: plan, frame: n, clips: frames,
                                         into: out) else {
            tally(failed: true, took: CACurrentMediaTime() - wall,
                  plan: plan, clock: clockAt)
            request.finish(with: Self.failure)
            return
        }
        if Self.slowFrames > 0 {
            Thread.sleep(forTimeInterval: Self.slowFrames)
        }
        let took = CACurrentMediaTime() - wall
        // Lately: mostly the last few frames.
        drawTime = drawTime == 0 ? took : drawTime * 0.7 + took * 0.3
        lastDrawn = CACurrentMediaTime()
        last = out
        ins.box.count(skipped: false, at: at.seconds)
        tally(drawn: true, took: took, plan: plan, clock: clockAt)
        request.finish(withComposedVideoFrame: out)
    }

    /// VALTZ_SNAPSHOT_SLOW_FRAMES=<ms>: every frame that long slower to
    /// draw -- a heavy stack, simulated, to see the player skip.
    private static let slowFrames = (ProcessInfo.processInfo
        .environment["VALTZ_SNAPSHOT_SLOW_FRAMES"].flatMap(Double.init) ?? 0)
        / 1000

    private static let failure = NSError(
        domain: "com.tgous.valtz", code: 1,
        userInfo: [NSLocalizedDescriptionKey: "The frame could not be drawn."])

    /// A clip whose pictures carry alpha: flagged so, or ProRes 4444 at
    /// depth 32.
    nonisolated static func hasAlpha(_ fd: CMFormatDescription) -> Bool {
        let ext = { (k: CFString) in
            CMFormatDescriptionGetExtension(fd, extensionKey: k) }
        if let a = ext(kCMFormatDescriptionExtension_ContainsAlphaChannel)
            as? Bool, a { return true }
        let sub = CMFormatDescriptionGetMediaSubType(fd)
        let prores4444 = sub == kCMVideoCodecType_AppleProRes4444
            || sub == kCMVideoCodecType_AppleProRes4444XQ
        return prores4444
            && (ext(kCMFormatDescriptionExtension_Depth) as? Int) == 32
    }

    /// The size the frames are drawn at: the canvas, made no larger than
    /// `room` (the stage's pixels) -- never larger than itself -- in even
    /// pixels, as video wants.
    static func renderSize(_ stack: StackPlayback, room: CGSize?) -> CGSize {
        let w = CGFloat(stack.width)
        let h = CGFloat(stack.height)
        var s: CGFloat = 1
        if let room, room.width > 0, room.height > 0 {
            s = min(1, room.width / w, room.height / h)
        }
        func even(_ v: CGFloat) -> CGFloat { max(2, (v / 2).rounded() * 2) }
        return s < 1 ? CGSize(width: even(w * s), height: even(h * s))
                     : CGSize(width: w, height: h)
    }

    /// Seconds, exactly enough for the player.
    private static func time(_ s: Double) -> CMTime {
        CMTime(seconds: s, preferredTimescale: 48000)
    }

    /// Before anything a timeline holds, and before the player's earliest
    /// buffer of it (its first labelled one buffer early: -0.085 s).
    static let beforeAll = CMTime(seconds: -1, preferredTimescale: 48000)

    /// Ten seconds of silence (48 kHz, mono, 16-bit): what pads a
    /// timeline past its clips. Written once, in the temporary folder --
    /// beside, then moved, so two Valtzes never read half of it.
    private static let silence: URL? = {
        let fm = FileManager.default
        let url = fm.temporaryDirectory
            .appendingPathComponent("valtz-silence-48k.caf")
        if fm.fileExists(atPath: url.path) { return url }
        let part = url.appendingPathExtension("\(getpid())")
        guard let fmt = AVAudioFormat(standardFormatWithSampleRate: 48000,
                                      channels: 1),
              let buf = AVAudioPCMBuffer(pcmFormat: fmt,
                                         frameCapacity: 480_000)
        else { return nil }
        buf.frameLength = 480_000
        do {
            let file = try AVAudioFile(
                forWriting: part,
                settings: [AVFormatIDKey: kAudioFormatLinearPCM,
                           AVSampleRateKey: 48000,
                           AVNumberOfChannelsKey: 1,
                           AVLinearPCMBitDepthKey: 16,
                           AVLinearPCMIsFloatKey: false])
            try file.write(from: buf)
        } catch {
            try? fm.removeItem(at: part)
            return nil
        }
        // Another got there first: theirs is as good.
        if (try? fm.moveItem(at: part, to: url)) == nil {
            try? fm.removeItem(at: part)
        }
        return fm.fileExists(atPath: url.path) ? url : nil
    }()

    /// `comp` runs from `from` to `to`: silence laid over the stretch, ten
    /// seconds at a time.
    @MainActor
    private static func padWithSilence(_ comp: AVMutableComposition,
                                       from: CMTime, to: CMTime) async {
        // The asset held while its track goes in: released, the insert
        // fails.
        guard let url = silence else { return }
        let asset = AVURLAsset(url: url)
        guard let src = try? await asset
                  .loadTracks(withMediaType: .audio).first,
              let range = try? await src.load(.timeRange),
              range.duration > .zero,
              let track = comp.addMutableTrack(
                  withMediaType: .audio,
                  preferredTrackID: kCMPersistentTrackID_Invalid)
        else { return }
        var at = from
        while at < to {
            let d = CMTimeMinimum(range.duration, CMTimeSubtract(to, at))
            do {
                try track.insertTimeRange(
                    CMTimeRange(start: range.start, duration: d),
                    of: src, at: at)
            } catch { return }
            at = CMTimeAdd(at, d)
        }
    }

    /// `src`'s spans into `track` where they play on the timeline (core
    /// media::time_segments / sound_segments: [at, length, source, source
    /// length]) -- each scaled to its length where a speed applies, and a
    /// sound's never past its track's own end (a shaped sound's tail asks
    /// for what its file holds, a little less where its source was short).
    /// With none: from the start, as long as it runs.
    @MainActor
    private static func lay(_ src: AVAssetTrack, into track:
                                AVMutableCompositionTrack,
                            segments: [[Double]], start: CMTime,
                            limit: CMTime) async {
        let range = try? await src.load(.timeRange)
        guard !segments.isEmpty else {
            guard let range else { return }
            try? track.insertTimeRange(
                CMTimeRange(start: range.start,
                            duration: CMTimeMinimum(range.duration, limit)),
                of: src, at: .zero)
            return
        }
        // A sound's alone: a clip's spans as they were.
        let own = src.mediaType == .audio
            ? range.map { $0.duration.seconds }
                .flatMap { $0.isFinite ? $0 : nil }
            : nil
        for seg in segments where seg.count == 4 && seg[3] > 0 {
            var sourceLength = seg[3], length = seg[1]
            if let own {
                sourceLength = min(sourceLength, own - seg[2])
                guard sourceLength > 1e-6 else { continue }
                length = seg[1] * sourceLength / seg[3]
            }
            let at = time(seg[0])
            let from = CMTimeRange(start: CMTimeAdd(start, time(seg[2])),
                                   duration: time(sourceLength))
            do {
                try track.insertTimeRange(from, of: src, at: at)
            } catch { continue }
            if abs(length - sourceLength) > 1e-6 {
                track.scaleTimeRange(CMTimeRange(start: at,
                                                 duration: time(sourceLength)),
                                     toDuration: time(length))
            }
        }
    }

    /// A stack's player item and, for each of its sounds (by its place in
    /// `stack.audio`), the track it plays on -- what a gain changed alone
    /// lays new ramps on (`audioMix`).
    struct Made {
        let item: AVPlayerItem
        let audioTracks: [Int: CMPersistentTrackID]
    }

    /// The sounds' volume ramps, on their tracks -- each sound's first
    /// volume held from well before it starts. AVFoundation plays a track
    /// at 1.0 before the first time a volume is set, and the player times
    /// a buffer's volume one buffer behind the sound in it (4,096 frames:
    /// a sound placed at 0.5 s is tapped in a buffer labelled from 0.41,
    /// one at 0 in one labelled from -0.085 -- whatever its pitch
    /// algorithm, from a start or after a seek): a ramp set from a
    /// sound's start alone played its first 85 ms -- two frames -- at
    /// full volume, then the keys' (2026-11-09). The exporter's mix
    /// (core media/sound.mm) glided from 1.0 for ~10 ms: it holds its
    /// first volume from the mix's start (2026-11-11).
    static func audioMix(_ stack: StackPlayback,
                         tracks: [Int: CMPersistentTrackID]) -> AVAudioMix {
        let mix = AVMutableAudioMix()
        var params: [AVMutableAudioMixInputParameters] = []
        for (i, s) in stack.audio.enumerated() where !s.volume.isEmpty {
            guard let id = tracks[i], let first = s.volume.first,
                  first.count == 2 else { continue }
            let p = AVMutableAudioMixInputParameters()
            p.trackID = id
            p.setVolume(Float(first[1]), at: Self.beforeAll)
            for (a, b) in zip(s.volume, s.volume.dropFirst())
                where a.count == 2 && b.count == 2 && b[0] > a[0] {
                p.setVolumeRamp(fromStartVolume: Float(a[1]),
                                toEndVolume: Float(b[1]),
                                timeRange: CMTimeRange(start: time(a[0]),
                                                       end: time(b[0])))
            }
            params.append(p)
        }
        mix.inputParameters = params
        return mix
    }

    /// The player's item for `stack`: a composition of its clips -- each
    /// where it plays on the timeline -- and its sound: every layer's, at
    /// its volume (a layer its pitch or equalizer shapes, its shaped span),
    /// as long as its timeline; every frame drawn by the core with `box`'s plan, at
    /// most `room` pixels. Sound alone: no frames.
    @MainActor
    static func item(_ stack: StackPlayback, _ box: StackBox,
                     room: CGSize? = nil) async -> Made? {
        let comp = AVMutableComposition()
        let duration = CMTime(value: Int64(stack.frames) * Int64(stack.rate.den),
                              timescale: Int32(stack.rate.num))
        var ids: [CMPersistentTrackID] = []
        var alpha = false
        for (i, url) in stack.clips.enumerated() {
            let asset = AVURLAsset(url: url)
            guard let vt = try? await asset.loadTracks(withMediaType: .video)
                      .first,
                  let range = try? await vt.load(.timeRange),
                  let track = comp.addMutableTrack(
                      withMediaType: .video,
                      preferredTrackID: kCMPersistentTrackID_Invalid)
            else { return nil }
            if let fds = try? await vt.load(.formatDescriptions),
               fds.contains(where: Self.hasAlpha) {
                alpha = true
            }
            await lay(vt, into: track,
                      segments: i < stack.segments.count ? stack.segments[i]
                                                         : [],
                      start: range.start, limit: duration)
            ids.append(track.trackID)
        }
        // The sound: each layer's spans with its ramps (or the mixer's
        // file whole).
        var tracks: [Int: CMPersistentTrackID] = [:]
        let sounds: [StackAudio] = stack.mix.map {
            [StackAudio(file: $0, segments: [], volume: [])]
        } ?? stack.audio
        for (i, s) in sounds.enumerated() {
            let asset = AVURLAsset(url: s.file)
            guard let at = try? await asset.loadTracks(withMediaType: .audio)
                      .first,
                  let ar = try? await at.load(.timeRange),
                  let track = comp.addMutableTrack(
                      withMediaType: .audio,
                      preferredTrackID: kCMPersistentTrackID_Invalid)
            else { continue }
            await lay(at, into: track, segments: s.segments, start: ar.start,
                      limit: duration)
            tracks[i] = track.trackID
        }
        let mix = stack.mix == nil ? audioMix(stack, tracks: tracks)
                                   : AVMutableAudioMix()
        // A timeline longer than its clips -- where they show nothing,
        // black past a clip's end, a frame to start a layer at -- runs to
        // its own end: a track of silence over the rest. (An empty range
        // at a composition's end is dropped: the player stopped on the
        // first frame past the clips.)
        if comp.duration < duration {
            await padWithSilence(comp, from: comp.duration, to: duration)
        }
        let item = AVPlayerItem(asset: comp)
        item.audioMix = mix
        // A layer played faster keeps its pitch.
        item.audioTimePitchAlgorithm = .spectral
        let made = Made(item: item,
                        audioTracks: stack.mix == nil ? tracks : [:])
        guard !stack.soundOnly else { return made }
        let vc = AVMutableVideoComposition()
        vc.customVideoCompositorClass = alpha ? StackCompositorAlpha.self
                                              : StackCompositor.self
        vc.renderSize = renderSize(stack, room: room)
        vc.frameDuration = CMTime(value: Int64(stack.rate.den),
                                  timescale: Int32(stack.rate.num))
        vc.colorPrimaries = AVVideoColorPrimaries_ITU_R_709_2
        vc.colorTransferFunction = AVVideoTransferFunction_ITU_R_709_2
        vc.colorYCbCrMatrix = AVVideoYCbCrMatrix_ITU_R_709_2
        vc.instructions = [StackInstruction(
            timeRange: CMTimeRange(start: .zero, duration: duration),
            trackIDs: ids, box: box)]
        item.videoComposition = vc
        return made
    }
}

/// A stack with a clip that has alpha: its sources as BGRA, which keeps
/// it (Core Image reads no YUV with alpha).
final class StackCompositorAlpha: StackCompositor, @unchecked Sendable {
    override var sourcePixelBufferAttributes: [String: any Sendable]? {
        Self.bgra
    }
}
