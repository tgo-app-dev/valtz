import AVKit
import QuartzCore
import SwiftUI

/// A video's live preview: the TAE's decode of the WHOLE clip at one
/// step of the denoise, looped at its rate until the next step replaces
/// it -- the motion is what a clip's preview is for.
struct PreviewClip {
    var frames: [CGImage]
    var fps: Double
    /// The step it shows; a new step is a new clip.
    var step: Int
}

/// Plays a PreviewClip by Core Animation: the frames are the keyframes of
/// one discrete `contents` animation that repeats, so nothing runs per
/// frame on the main thread. A new clip replaces the animation.
struct ClipView: NSViewRepresentable {
    let clip: PreviewClip

    final class Coordinator {
        var step = -1
        var count = 0
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        v.wantsLayer = true
        let layer = CALayer()
        layer.contentsGravity = .resizeAspect
        layer.backgroundColor = NSColor.black.cgColor
        v.layer = layer
        return v
    }

    func updateNSView(_ v: NSView, context: Context) {
        guard let layer = v.layer, !clip.frames.isEmpty,
              context.coordinator.step != clip.step
                || context.coordinator.count != clip.frames.count
        else { return }
        context.coordinator.step = clip.step
        context.coordinator.count = clip.frames.count
        layer.removeAnimation(forKey: "clip")
        // Underneath, when the animation is not running: the last frame.
        layer.contents = clip.frames.last
        guard clip.frames.count > 1 else { return }
        let n = clip.frames.count
        let a = CAKeyframeAnimation(keyPath: "contents")
        a.values = clip.frames
        a.calculationMode = .discrete
        // Discrete: one more key time than values, from 0 to 1.
        a.keyTimes = (0...n).map { NSNumber(value: Double($0) / Double(n)) }
        a.duration = Double(n) / max(1, clip.fps)
        a.repeatCount = .infinity
        a.isRemovedOnCompletion = false
        layer.add(a, forKey: "clip")
    }
}

/// A finished clip on the stage: AVKit's player, its controls floating
/// over the picture as in QuickTime Player (play, the scrubber, volume,
/// full screen). It plays once when it lands -- with its sound: the
/// soundtrack was made with the picture -- and stops on the last frame;
/// `loops` plays it again and again. It says which frame it shows
/// (`onFrame`, by `rate`) as it plays and as it is scrubbed, and goes to a
/// frame exactly when asked (`seek`: the Trim panel's jumps).
///
/// A clip with a LOOK (its adjustment and crop keyframes) is drawn with
/// it, frame by frame, at each frame's own values: a Core Image video
/// composition that asks `look` for every frame. A changed look draws
/// the frame on screen again, paused or playing.
struct VideoPlayerView: NSViewRepresentable {
    let url: URL
    var loops = false
    var autoplay = true
    var rate = FrameRate.fallback
    var command: VideoCommand? = nil
    /// Drawn on every frame; a change draws the frame on screen again.
    var look: ClipLook? = nil
    /// A clip with a stack: played as its composition, drawn by the core.
    var stack: StackPlayback? = nil
    /// A sound's picture (its waveform), over the player's empty frame.
    var artwork: CGImage? = nil
    /// AVKit's own controls over the picture -- none while the timeline,
    /// whose transport is the fuller one, is open.
    var controls = true
    var core: CoreService? = nil
    var onFrame: (Int) -> Void = { _ in }
    var onRate: (Float) -> Void = { _ in }

    /// What the composition draws with: read on AVFoundation's thread.
    final class LookBox: @unchecked Sendable {
        private let lock = NSLock()
        private var look: ClipLook?
        private var core: CoreService?

        func set(_ look: ClipLook?, _ core: CoreService?) {
            lock.withLock {
                self.look = look
                self.core = core
            }
        }

        func render(_ image: CIImage, at time: CMTime) -> CIImage {
            let (look, core) = lock.withLock { (self.look, self.core) }
            guard let look, let core, !look.isIdentity else { return image }
            return look.apply(image, frame: look.rate.frame(at: time.seconds),
                              core: core)
        }
    }

    @MainActor
    final class Coordinator {
        var url: URL?
        var loops = false
        var commandToken: Int?
        var player: AVPlayer?
        var rateObservation: NSKeyValueObservation?
        var onRate: (Float) -> Void = { _ in }
        var ended: NSObjectProtocol?
        var timeObserver: Any?
        var onFrame: (Int) -> Void = { _ in }
        var lastFrame = -1
        /// The clip's last frame, from its PICTURE's length (a soundtrack
        /// may run longer); nil until the track has been read. The
        /// timeline ends where that frame ENDS, and a time there shows no
        /// frame at all -- a composition draws it black -- so the player
        /// is kept on the frames that exist.
        var lastIndex: Int?
        var rate = FrameRate.fallback
        let box = LookBox()
        var look: ClipLook?
        var lookKnown = false
        var hasLook = false
        var composing = false
        var recomposeAgain = false
        /// The stack on show: what its composition is made of, and the
        /// plan its frames are drawn with.
        var stackShape: [String]?
        var stackPlan: UInt64 = 0
        let stackBox = StackBox()
        /// The stack on show, and the view it plays in: its frames are
        /// drawn at the stage's pixels, again when the stage resizes.
        var stack: StackPlayback?
        weak var view: AVPlayerView?
        var resized: NSObjectProtocol?
        var resizeTask: Task<Void, Never>?

        /// The stage's size in pixels; nil before it has one.
        var room: CGSize? {
            guard let v = view, v.bounds.width > 0 else { return nil }
            let scale = v.window?.backingScaleFactor
                ?? NSScreen.main?.backingScaleFactor ?? 2
            return CGSize(width: v.bounds.width * scale,
                          height: v.bounds.height * scale)
        }

        /// Watched once: the stage resized, its frames drawn at the new
        /// size (a quarter second after the last change).
        func watchResize(_ v: AVPlayerView) {
            view = v
            guard resized == nil else { return }
            v.postsFrameChangedNotifications = true
            resized = NotificationCenter.default.addObserver(
                forName: NSView.frameDidChangeNotification, object: v,
                queue: .main
            ) { [weak self] _ in
                MainActor.assumeIsolated {
                    guard let self else { return }
                    self.resizeTask?.cancel()
                    self.resizeTask = Task { @MainActor [weak self] in
                        try? await Task.sleep(for: .milliseconds(250))
                        guard !Task.isCancelled else { return }
                        self?.fitStack()
                    }
                }
            }
        }

        /// The stack's frames drawn at the stage's pixels: a copy of the
        /// composition at the new size, when it changed by more than a
        /// little (drawn at once, paused or playing).
        func fitStack() {
            guard let stack, let item = player?.currentItem,
                  let vc = item.videoComposition,
                  let cls = vc.customVideoCompositorClass,
                  cls is StackCompositor.Type
            else { return }
            let want = StackCompositor.renderSize(stack, room: room)
            let have = vc.renderSize
            guard abs(want.width - have.width) > have.width * 0.08,
                  let comp = vc.mutableCopy() as? AVMutableVideoComposition
            else { return }
            comp.renderSize = want
            item.videoComposition = comp
        }

        /// The frame on screen drawn again with the new plan: a fresh copy
        /// of the composition is drawn at once, paused or playing.
        func redrawStack() {
            guard let item = player?.currentItem,
                  let comp = item.videoComposition?.mutableCopy()
                      as? AVVideoComposition else { return }
            item.videoComposition = comp
        }

        /// The item through a FRESH composition of the look. The box
        /// alone reaches only frames drawn from now on: a paused player
        /// keeps the frame it has (a seek to where it already is draws
        /// nothing), and adjusting is done paused. A new composition is
        /// drawn at once, paused or playing. One at a time; changes that
        /// land meanwhile make one more, so a slider drag shows its newest
        /// value without a queue of stale ones.
        func recompose() {
            guard hasLook, let item = player?.currentItem else { return }
            if composing {
                recomposeAgain = true
                return
            }
            composing = true
            let box = box
            Task { @MainActor [weak self] in
                let comp = await VideoPlayerView.composition(item.asset, box)
                guard let self else { return }
                self.composing = false
                if let comp, self.player?.currentItem === item {
                    item.videoComposition = comp
                }
                if self.recomposeAgain {
                    self.recomposeAgain = false
                    self.recompose()
                }
            }
        }

        /// Frame `n`, held to the clip's frames.
        func clamp(_ n: Int) -> Int { max(0, min(n, lastIndex ?? n)) }

        /// When frame `n` starts.
        func start(of n: Int) -> CMTime {
            let t = rate.time(of: n)
            return CMTime(value: t.value, timescale: t.timescale)
        }

        /// Play pressed while it rests at the end (`at`: where it was when
        /// it started) -- the floating controls' button as well as the
        /// Trim panel's: from the start again. The floating button plays
        /// the player directly, so from where it rested it played the
        /// last frame's sliver and stopped again.
        func startOverIfAtEnd(_ at: CMTime) {
            guard let p = player, let last = lastIndex, at.isNumeric,
                  rate.frame(at: at.seconds) >= last else { return }
            p.seek(to: .zero, toleranceBefore: .zero, toleranceAfter: .zero)
            p.play()
        }

        /// Frame `n`, exactly, said once there: a player just made for a
        /// new stack sits at 0:00, and a seek there moves no clock -- its
        /// time observer stayed silent, and the panel kept the old time.
        func seek(to n: Int) {
            player?.seek(to: start(of: n), toleranceBefore: .zero,
                         toleranceAfter: .zero) { [weak self] done in
                guard done else { return }
                Task { @MainActor [weak self] in
                    guard let self, self.lastFrame != n else { return }
                    self.lastFrame = n
                    self.onFrame(n)
                }
            }
        }

        /// On the last frame, exactly (not after it).
        func restOnLast() {
            guard let p = player, let last = lastIndex else { return }
            p.seek(to: start(of: last), toleranceBefore: .zero,
                   toleranceAfter: .zero)
        }

        /// A step's sound, apart from the player.
        let scrubber = AudioScrubber()

        func stop() {
            scrubber.stop()
            stackBox.set(clock: nil)
            player?.pause()
            if let ended { NotificationCenter.default.removeObserver(ended) }
            if let timeObserver { player?.removeTimeObserver(timeObserver) }
            rateObservation?.invalidate()
            rateObservation = nil
            ended = nil
            timeObserver = nil
            player = nil
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> AVPlayerView {
        let v = AVPlayerView()
        v.controlsStyle = .floating
        v.showsFullScreenToggleButton = true
        v.videoGravity = .resizeAspect
        v.allowsVideoFrameAnalysis = false
        return v
    }

    func updateNSView(_ v: AVPlayerView, context: Context) {
        let c = context.coordinator
        let style: AVPlayerViewControlsStyle = controls ? .floating : .none
        if v.controlsStyle != style { v.controlsStyle = style }
        if let overlay = v.contentOverlayView {
            let iv = overlay.subviews.compactMap { $0 as? NSImageView }.first
                ?? {
                    let iv = NSImageView(frame: overlay.bounds)
                    iv.autoresizingMask = [.width, .height]
                    iv.imageScaling = .scaleProportionallyUpOrDown
                    overlay.addSubview(iv)
                    return iv
                }()
            iv.image = artwork.map { NSImage(cgImage: $0, size: .zero) }
        }
        c.loops = loops
        c.onFrame = onFrame
        c.onRate = onRate
        defer { applyCommand(c) }
        let lookChanged = !c.lookKnown || c.look != look
        if lookChanged {
            c.look = look
            c.lookKnown = true
            c.hasLook = look != nil
            c.box.set(look, core)
        }
        // A clip with a STACK -- its layers, a canvas, a timeline of its
        // own -- plays as a composition of its clips, every frame drawn
        // by the core (StackCompositor).
        c.watchResize(v)
        c.stack = stack
        if let stack {
            if c.stackShape == stack.shape && c.url == url {
                if c.stackPlan != stack.plan {
                    c.stackPlan = stack.plan
                    c.stackBox.set(core: core, plan: stack.plan,
                                   rate: stack.rate)
                    c.redrawStack()
                }
                return
            }
            // The same clip made again (a canvas, a length): it stays
            // where it was, playing or paused.
            let resume = c.url == url ? c.player?.currentTime() : nil
            let playing = (c.player?.rate ?? 0) != 0
            c.stop()
            c.url = url
            c.stackShape = stack.shape
            c.stackPlan = stack.plan
            c.stackBox.set(core: core, plan: stack.plan, rate: stack.rate)
            c.hasLook = false
            c.rate = stack.rate
            let box = c.stackBox
            box.resetCounts()
            StackBox.shown = box
            let autoplay = resume == nil ? autoplay : playing
            let room = c.room
            Task { @MainActor [weak c] in
                guard let item = await StackCompositor.item(stack, box,
                                                            room: room),
                      let c, c.stackShape == stack.shape else { return }
                attach(item, to: v, c, rate: stack.rate, autoplay: autoplay,
                       lastIndex: max(0, stack.frames - 1), at: resume)
                // A request that came with the new stack -- the player
                // was being made: it goes to the new one, not lost.
                applyCommand(c)
            }
            return
        }
        var resume: CMTime?
        var playing = false
        // The same file counted at another rate -- a sound's milliseconds,
        // known only once its asset is: its frames reported again at
        // that rate, from where it is.
        if c.stackShape == nil, c.url == url, c.rate != rate {
            resume = c.player?.currentTime()
            playing = (c.player?.rate ?? 0) != 0
            c.url = nil
        }
        if c.stackShape != nil {
            // Its stack gone: the clip alone again, where it was.
            if c.url == url {
                resume = c.player?.currentTime()
                playing = (c.player?.rate ?? 0) != 0
            }
            c.stackShape = nil
            c.url = nil
        }
        guard c.url != url else {
            if lookChanged { c.recompose() }
            return
        }
        c.stop()
        c.url = url
        let item = AVPlayerItem(asset: AVURLAsset(url: url))
        c.lastIndex = nil
        attach(item, to: v, c, rate: rate,
               autoplay: resume == nil ? autoplay : playing, lastIndex: nil,
               at: resume)
        // Every frame through the look (Core Image).
        c.recompose()
    }

    /// The player for `item`, on `v`: its end, its frame and its rate
    /// reported, playing if asked. `lastIndex`: its last frame, when it
    /// is known (else read from the item's picture).
    private func attach(_ item: AVPlayerItem, to v: AVPlayerView,
                        _ c: Coordinator, rate: FrameRate, autoplay: Bool,
                        lastIndex: Int?, at resume: CMTime? = nil) {
        let asset = item.asset
        let player = AVPlayer(playerItem: item)
        player.actionAtItemEnd = .pause
        c.player = player
        // Where the sound is: a stack's late frames are skipped by it.
        c.stackBox.set(clock: item.timebase)
        let rate = rate
        c.rate = rate
        c.lastIndex = nil
        // Its frames: the last is the one that starts before the picture
        // ends. Playback ends with the picture, too.
        if let known = lastIndex {
            // A stack's timeline: as long as it was made.
            c.lastIndex = known
            item.forwardPlaybackEndTime = c.start(of: known + 1)
        } else { Task { @MainActor [weak c] in
            // A sound has no picture: its own track ends it.
            let picture = try? await asset.loadTracks(withMediaType: .video)
                .first
            let sound = picture == nil
                ? try? await asset.loadTracks(withMediaType: .audio).first
                : nil
            guard let track = picture ?? sound,
                  let range = try? await track.load(.timeRange),
                  range.end.isNumeric,
                  let c, c.player?.currentItem === item else { return }
            c.lastIndex = max(0, rate.frame(
                at: range.end.seconds - 0.5 / rate.fps))
            item.forwardPlaybackEndTime = range.end
        } }
        // At the end: again from the start while looping; otherwise it
        // rests on the last frame, and play starts it over.
        c.ended = NotificationCenter.default.addObserver(
            forName: AVPlayerItem.didPlayToEndTimeNotification,
            object: player.currentItem, queue: .main
        ) { [weak c] _ in
            MainActor.assumeIsolated {
                guard let c, let p = c.player else { return }
                if c.loops {
                    p.seek(to: .zero)
                    p.play()
                } else {
                    c.restOnLast()
                }
            }
        }
        // Which frame shows, twice a frame while it plays, and whenever
        // the time jumps (a scrub, a seek). A paused time past the last
        // frame -- the scrubber's far end -- goes back onto it.
        // (At most 60 times a second: a sound counts milliseconds, and the
        // screen shows no more than that.)
        let every = CMTimeMaximum(
            CMTime(value: Int64(rate.den), timescale: Int32(rate.num * 2)),
            CMTime(value: 1, timescale: 60))
        c.lastFrame = -1
        c.timeObserver = player.addPeriodicTimeObserver(
            forInterval: every, queue: .main
        ) { [weak c] t in
            MainActor.assumeIsolated {
                guard let c, t.isNumeric else { return }
                let n = rate.frame(at: t.seconds)
                if let last = c.lastIndex, n > last, c.player?.rate == 0 {
                    c.restOnLast()
                }
                let shown = c.clamp(n)
                if shown != c.lastFrame {
                    c.lastFrame = shown
                    c.onFrame(shown)
                }
            }
        }
        // Its rate, for the transport's pressed-in button -- and, played
        // from a stop at the end, from the start again.
        c.rateObservation = player.observe(\.rate,
                                           options: [.initial, .old, .new]) {
            [weak c] p, change in
            let rate = p.rate
            let started = (change.oldValue ?? 0) == 0 && rate > 0
            let at = p.currentTime()
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    if started { c?.startOverIfAtEnd(at) }
                    c?.onRate(rate)
                }
            }
        }
        v.player = player
        if let resume {
            player.seek(to: resume, toleranceBefore: .zero,
                        toleranceAfter: .zero)
        }
        if autoplay { player.play() }
    }

    /// Every frame of `asset` through `box`'s look.
    static func composition(_ asset: AVAsset,
                            _ box: LookBox) async -> AVVideoComposition? {
        // No picture, no composition: AVFoundation answers a sound's with
        // neither one nor an error, which traps.
        guard (try? await asset.loadTracks(withMediaType: .video).first)
                != nil else { return nil }
        return try? await AVVideoComposition(applyingFiltersTo: asset) { p in
            AVCIImageFilteringResult(
                resultImage: box.render(p.sourceImage, at: p.compositionTime))
        }
    }

    /// Frame `frame` of `url` as the player draws it with `look` (a dev
    /// and snapshot aid: the same composition, through an image
    /// generator).
    static func frame(_ url: URL, _ frame: Int, look: ClipLook,
                      core: CoreService) async -> CGImage? {
        let asset = AVURLAsset(url: url)
        let box = LookBox()
        box.set(look, core)
        guard let comp = await composition(asset, box) else { return nil }
        let gen = AVAssetImageGenerator(asset: asset)
        gen.videoComposition = comp
        gen.requestedTimeToleranceBefore = .zero
        gen.requestedTimeToleranceAfter = .zero
        let t = look.rate.time(of: frame)
        return try? await gen.image(
            at: CMTime(value: t.value, timescale: t.timescale)).image
    }

    /// A new request: a frame exactly, paused; a rate (a backward or fast
    /// one only as far as the clip can play it); a step.
    private func applyCommand(_ c: Coordinator) {
        guard let command, command.token != c.commandToken,
              let p = c.player, let item = p.currentItem else {
            return
        }
        c.commandToken = command.token
        switch command.kind {
        case .seek(let n):
            p.pause()
            c.seek(to: c.clamp(n))
        case .place(let n):
            c.seek(to: c.clamp(n))
        case .step(let n):
            // From the frame on screen, to one that exists: a step past
            // either end stays on it -- and its sound heard, a frame's (at
            // least 60 ms, to be heard at all), to find a place by ear.
            p.pause()
            let now = c.clamp(rate.frame(at: p.currentTime().seconds))
            let to = c.clamp(now + n)
            c.seek(to: to)
            c.scrubber.play(item, at: c.start(of: to),
                            seconds: max(1 / rate.fps, 0.06))
        case .play(var r):
            if r < -1 && !item.canPlayFastReverse {
                r = item.canPlayReverse ? -1 : 0
            } else if r < 0 && !item.canPlayReverse {
                r = 0
            } else if r > 1 && !item.canPlayFastForward {
                r = 1
            }
            // At an end, playing on starts from the other.
            if let last = c.lastIndex {
                let now = rate.frame(at: p.currentTime().seconds)
                if r > 0, now >= last {
                    p.seek(to: .zero, toleranceBefore: .zero,
                           toleranceAfter: .zero)
                } else if r < 0, now <= 0 {
                    c.restOnLast()
                }
            }
            p.rate = r
        }
    }

    static func dismantleNSView(_ v: AVPlayerView, coordinator: Coordinator) {
        coordinator.stop()
        if let r = coordinator.resized {
            NotificationCenter.default.removeObserver(r)
            coordinator.resized = nil
        }
        coordinator.resizeTask?.cancel()
        v.player = nil
    }
}
