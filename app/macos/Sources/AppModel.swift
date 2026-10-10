import AVFoundation
import AppKit
import CoreGraphics
import ImageIO
import Observation
import SwiftUI
import UniformTypeIdentifiers

/// What the window shows, picked in the ☰ column: the editor (the prompt,
/// its cards and the stage) or the log of what the engine reports.
enum AppScreen: String, CaseIterable, Identifiable, Sendable {
    case editor, log
    var id: String { rawValue }

    var label: String {
        switch self {
        case .editor: String(localized: "Editor")
        case .log: String(localized: "Log")
        }
    }

    var symbol: String {
        switch self {
        case .editor: "square.and.pencil"
        case .log: "text.alignleft"
        }
    }
}

/// Compare modes, after vpipe's compare-image view: A, B, side by side,
/// stacked, vertical and horizontal wipe -- plus difference.
enum CompareMode: String, CaseIterable, Identifiable, Sendable {
    case a, b, sideBySide, stacked, wipe, wipeHorizontal, difference
    var id: String { rawValue }

    var label: String {
        switch self {
        case .a: "A"
        case .b: "B"
        case .sideBySide: "A | B"
        case .stacked: "A / B"
        case .wipe: String(localized: "Wipe")
        case .wipeHorizontal: String(localized: "Wipe")
        case .difference: String(localized: "Difference")
        }
    }

    var symbol: String {
        switch self {
        case .a: "a.square"
        case .b: "b.square"
        case .sideBySide: "rectangle.split.2x1"
        case .stacked: "rectangle.split.1x2"
        case .wipe: "square.split.2x1"
        case .wipeHorizontal: "square.split.1x2"
        case .difference: "minus.square"
        }
    }

    var help: String {
        switch self {
        case .a: String(localized: "Show A")
        case .b: String(localized: "Show B")
        case .sideBySide: String(localized: "Side by side")
        case .stacked: String(localized: "Stacked")
        case .wipe: String(localized: "Vertical wipe")
        case .wipeHorizontal: String(localized: "Horizontal wipe")
        case .difference: String(localized: "Difference |A − B|")
        }
    }

    /// Every mode but A needs a second image.
    var needsB: Bool { self != .a }
}

/// How an image lies on a viewer's CANVAS -- the pixel grid of the image
/// it is compared with. Most images are the canvas (`.identity`). An
/// edit's original is not: the engine filled the result's frame with it
/// and centre-cropped the rest (graph-builder.cc's base fit, vpipe
/// image-resample `fit: crop`). To line up with the result, the original
/// is shown cropped to that frame and scaled by the ratio the engine
/// resampled it with -- at its own pixels, all of them kept: the viewer
/// scales, it never resamples.
struct CanvasFit: Equatable, Sendable {
    /// Canvas pixels per image pixel.
    var scale: CGFloat = 1
    /// Where the image's top-left corner lies, in canvas pixels (a crop
    /// that starts part-way into a pixel starts a little before 0).
    var origin: CGPoint = .zero

    static let identity = CanvasFit()

    /// `original` placed as the engine fitted it onto a canvas of
    /// `canvas` pixels: scaled to fill it, centre-cropped. The crop is
    /// to whole pixels; `origin` keeps the fraction, so the two line up
    /// exactly.
    static func centreCrop(_ original: CGImage, onto canvas: CGSize)
        -> (image: CGImage, fit: CanvasFit) {
        let iw = CGFloat(original.width), ih = CGFloat(original.height)
        guard iw > 0, ih > 0, canvas.width > 0, canvas.height > 0 else {
            return (original, .identity)
        }
        let s = max(canvas.width / iw, canvas.height / ih)
        // The frame, in the original's pixels (vpipe's crop geometry).
        let x0 = (iw - canvas.width / s) / 2
        let y0 = (ih - canvas.height / s) / 2
        let eps: CGFloat = 1e-4
        let ix0 = max(0, (x0 + eps).rounded(.down))
        let iy0 = max(0, (y0 + eps).rounded(.down))
        let ix1 = min(iw, (iw - x0 - eps).rounded(.up))
        let iy1 = min(ih, (ih - y0 - eps).rounded(.up))
        let rect = CGRect(x: ix0, y: iy0, width: ix1 - ix0, height: iy1 - iy0)
        let fit = CanvasFit(scale: s,
                            origin: CGPoint(x: (ix0 - x0) * s,
                                            y: (iy0 - y0) * s))
        guard rect.size != CGSize(width: iw, height: ih) else {
            return (original, fit)
        }
        return (original.cropping(to: rect) ?? original, fit)
    }
}

/// One side of the stage's compare.
enum StageSlot: Sendable {
    case a, b
    var other: StageSlot { self == .a ? .b : .a }
}

/// What a viewer shows: image A and optionally image B to compare with.
///
/// Each image has its CanvasFit; a new image starts as the canvas
/// (`.identity`), so code that sets an image without a fit never
/// inherits the last one's.
struct ViewerState {
    var a: CGImage? {
        didSet {
            if a !== oldValue { aFit = .identity; aCompared = nil; aSurface = nil }
        }
    }
    /// A's pixels as a GPU surface, when they were drawn into one
    /// (GPUPicture): what the canvas shows, with no copy.
    var aSurface: IOSurface?
    var aFit = CanvasFit.identity
    var aLabel = ""
    /// What A shows, put there to compare (inspector › Assets): an
    /// asset's id, or the history entry of a base as the model got it.
    var aCompared: String?
    var b: CGImage? {
        didSet {
            if b !== oldValue { bFit = .identity; bCompared = nil; bSurface = nil }
        }
    }
    var bSurface: IOSurface?
    var bFit = CanvasFit.identity
    var bLabel = ""
    var bCompared: String?
    /// A clip on show instead of a still: a finished video (its file; `a`
    /// is then its poster frame, for the layout and a drag), or a video's
    /// live preview, looped as each step arrives.
    var video: URL?
    var clip: PreviewClip?
    var mode: CompareMode = .a
    /// The wipe's divider is drawn. Hidden, it still moves: the pointer
    /// over where it is still turns into the resize cursor.
    var wipeLine = true
    /// Bumped to ask the canvas to fit / show 1:1 / zoom a step / stop
    /// fitting.
    var fitRequest = 0
    var actualSizeRequest = 0
    var zoomInRequest = 0
    var zoomOutRequest = 0
    var unfitRequest = 0
    /// As the canvas reports it: device pixels per image pixel (1 is
    /// 1:1; nil with no image), and whether it is fitting -- fitted again
    /// whenever the view or the image changes.
    private(set) var zoom: CGFloat?
    private(set) var fitting = true

    mutating func setViewport(_ v: CompareCanvas.Viewport) {
        if zoom != v.zoom { zoom = v.zoom }
        if fitting != v.fitting { fitting = v.fitting }
    }

    /// It shows a clip -- played, not zoomed or compared.
    var showsMotion: Bool { video != nil || clip != nil }

    /// Showing actual pixels (to within rounding of the percentage).
    var isActualSize: Bool {
        zoom.map { abs($0 - 1) < 0.005 } ?? false
    }

    var zoomText: String {
        zoom.map { "\(Int(($0 * 100).rounded()))%" } ?? ""
    }

    /// A and B change places, each with its fit and label.
    mutating func swapAB() {
        let (oa, ob, fa, fb, la, lb) = (a, b, aFit, bFit, aLabel, bLabel)
        let (ha, hb) = (aCompared, bCompared)
        let (sa, sb) = (aSurface, bSurface)
        a = ob
        b = oa
        aFit = fb
        bFit = fa
        aLabel = lb
        bLabel = la
        aCompared = hb
        bCompared = ha
        aSurface = sb
        bSurface = sa
    }

    /// Width / height of what the canvas lays out (two images side by
    /// side are twice as wide).
    var layoutAspect: CGFloat {
        guard let a, a.height > 0 else { return 1 }
        let r = CGFloat(a.width) / CGFloat(a.height)
        guard b != nil, !showsMotion else { return r }
        switch mode {
        case .sideBySide: return r * 2
        case .stacked: return r / 2
        default: return r
        }
    }
}

/// Favor: a preset of steps and adapters, or -- Custom -- the model
/// family's options at values of one's own (core models/tuning.h).
enum Preference: String, CaseIterable, Identifiable, Sendable {
    case speed, balanced, quality, custom
    var id: String { rawValue }
    /// Short, to keep the Favor row narrow: its hint says what it does.
    var label: String {
        switch self {
        case .speed: String(localized: "Fast", comment: "Favor: fewer steps")
        case .balanced: String(localized: "Med",
                               comment: "Favor: the model's default steps")
        case .quality: String(localized: "Fine", comment: "Favor: more steps")
        case .custom: String(localized: "Tune",
                             comment: "Favor: one's own settings")
        }
    }
    /// The longer names it went by, still found by the settings search.
    var searchName: String {
        switch self {
        case .speed: String(localized: "Speed")
        case .balanced: String(localized: "Balanced")
        case .quality: String(localized: "Quality")
        case .custom: String(localized: "Custom")
        }
    }
    /// What it does to the step count.
    var hint: String {
        switch self {
        case .speed: String(localized: "Half the steps")
        case .balanced: String(localized: "Model default")
        case .quality: String(localized: "1.5× the steps")
        case .custom: String(localized: "Your own settings")
        }
    }
    static let presets: [Preference] = [.speed, .balanced, .quality]
}

/// The inspector's sections (its navigation row, or its stacked panels).
enum InspectorTab: String, CaseIterable, Identifiable, Sendable {
    case info, layers, assets
    var id: String { rawValue }

    var label: LocalizedStringKey {
        switch self {
        case .info: "Information"
        case .layers: "Layers"
        case .assets: "Assets"
        }
    }

    var symbol: String {
        switch self {
        case .info: "info.circle"
        case .layers: "square.3.layers.3d"
        case .assets: "photo.stack"
        }
    }
}

/// The title bar's zoom group.
enum ZoomStep: Sendable {
    case zoomOut, actualSize, fit, zoomIn
}

/// What a generation makes: a picture, a clip or a sound -- a song (the
/// tab over the prompt). Its raw value is the core's modality name.
enum Modality: String, CaseIterable, Identifiable, Sendable {
    case video, image, audio
    var id: String { rawValue }

    var label: String {
        switch self {
        case .image: String(localized: "Image")
        case .video: String(localized: "Video")
        case .audio: String(localized: "Audio")
        }
    }

    /// Its glyph on the tab.
    var symbol: String {
        switch self {
        case .image: "photo"
        case .video: "video"
        case .audio: "waveform"
        }
    }
}

/// What a song plans before it is sung (YuE2's `cot`): a score of its
/// melody and chords, of its melody only, or none.
enum SongPlan: String, CaseIterable, Identifiable, Sendable {
    case full, melody, off
    var id: String { rawValue }

    var label: String {
        switch self {
        case .full: String(localized: "Full")
        case .melody: String(localized: "Melody")
        case .off: String(localized: "None")
        }
    }

    /// What it does (the segment's tooltip).
    var hint: String {
        switch self {
        case .full: String(localized: "Plan a score of melody and chords first, then sing and play to it")
        case .melody: String(localized: "Plan the melody only, then arrange around it")
        case .off: String(localized: "No score: straight to the song")
        }
    }
}

/// The output's shape: square, or which way its long side runs. With a
/// custom size it is not chosen but read off the size (an indicator).
enum Orientation: String, CaseIterable, Identifiable, Sendable {
    case square, portrait, landscape
    var id: String { rawValue }
    var label: String {
        switch self {
        case .square: String(localized: "Square")
        case .portrait: String(localized: "Portrait")
        case .landscape: String(localized: "Landscape")
        }
    }
    /// Its glyph on the Orientation row (the label is its tooltip and
    /// what VoiceOver reads).
    var symbol: String {
        switch self {
        case .square: "square"
        case .portrait: "rectangle.portrait"
        case .landscape: "rectangle"
        }
    }

    init(width: Int, height: Int) {
        self = width == height ? .square
            : width > height ? .landscape : .portrait
    }
}

/// How much longer the long side is, for a portrait or landscape output.
enum AspectRatio: String, CaseIterable, Identifiable, Sendable {
    case fourThree = "4:3", threeTwo = "3:2", wide = "16:9"
    var id: String { rawValue }
    /// Long side / short side.
    var ratio: Double {
        let p = rawValue.split(separator: ":").compactMap { Double($0) }
        return p.count == 2 ? p[0] / p[1] : 1
    }
    /// As the picture is shaped: "3:2" landscape, "2:3" portrait.
    func label(_ o: Orientation) -> String {
        guard o == .portrait else { return rawValue }
        return rawValue.split(separator: ":").reversed().joined(separator: ":")
    }
}

/// A size typed by hand (or taken from the base), in pixels.
struct PixelSize: Equatable, Sendable {
    var width: Int
    var height: Int

    /// Edges a typed size is held to.
    static let edges = 256...4096

    /// "1200x800", "1200 × 800" or "1200*800".
    init?(parsing text: String) {
        let parts = text.split { "xX×*".contains($0) }
            .map { $0.trimmingCharacters(in: .whitespaces) }
        guard parts.count == 2, let w = Int(parts[0]), let h = Int(parts[1]),
              w > 0, h > 0 else { return nil }
        self.init(width: w.clamped(to: Self.edges),
                  height: h.clamped(to: Self.edges))
    }

    init(width: Int, height: Int) {
        self.width = width
        self.height = height
    }
}

extension Comparable {
    func clamped(to r: ClosedRange<Self>) -> Self {
        min(max(self, r.lowerBound), r.upperBound)
    }
}

enum SizeClass: Int, CaseIterable, Identifiable, Sendable {
    case draft = 512, standard = 1024, large = 1536
    var id: Int { rawValue }
    /// The side of the square of its area, for what is made: a picture is
    /// about rawValue² pixels; a clip about 832 × 480, 960 × 544 or
    /// 1280 × 720 at 16:9 -- the sizes video models are made for, and
    /// time grows with every pixel of every frame.
    func edge(_ m: Modality) -> Int {
        guard m == .video else { return rawValue }
        switch self {
        case .draft: return 632
        case .standard: return 723
        case .large: return 960
        }
    }
    var label: String {
        switch self {
        case .draft: String(localized: "Draft")
        case .standard: String(localized: "Standard")
        case .large: String(localized: "Large")
        }
    }
}

/// The output size settings, kept per modality (AppModel.setModality).
struct OutputShape: Sendable {
    var orientation: Orientation
    var aspectRatio: AspectRatio
    var sizeClass: SizeClass
    var customSize: PixelSize?
    var sizeIsBase: Bool

    /// A picture starts square at the Standard size; a clip landscape,
    /// 16:9, at the Draft size (832 × 480).
    static func defaults(for m: Modality) -> OutputShape {
        m == .video
            ? OutputShape(orientation: .landscape, aspectRatio: .wide,
                          sizeClass: .draft, customSize: nil,
                          sizeIsBase: false)
            : OutputShape(orientation: .square, aspectRatio: .threeTwo,
                          sizeClass: .standard, customSize: nil,
                          sizeIsBase: false)
    }
}

/// The simple-mode composer's panels, opened from their tabs under the
/// prompt card.
enum ComposerPanel: Sendable, CaseIterable {
    case adjust     // adjustments: a picture's, or a clip's (keyed)
    case crop       // crop and rotation: a picture's, or a clip's (keyed)
    case trim       // a clip's mark-in and mark-out
    case generate   // the generation settings
}

/// A request to the stage's player; a new `token` is a new request.
///   seek    show frame N, exactly, paused
///   play    play at a rate: 1 forward, -1 backward, faster beyond, 0 is
///           pause
///   step    N frames on (or back), paused
struct VideoCommand: Equatable, Sendable {
    enum Kind: Equatable, Sendable {
        case seek(Int)
        /// Frame n, playing on if it played (a mark just set).
        case place(Int)
        case play(Float)
        case step(Int)
    }
    var kind: Kind
    var token: Int
}

/// What Save… writes the image or video on screen as: its own file, or
/// an export -- the core's export-media job, through vpipe's
/// Apple-native writers: deeper, or another codec, its colour and EXIF
/// kept. The raw values are the core's format names. The save panel
/// lists PNG and TIFF once each, their depth an option of their own
/// (SaveOptions).
enum ExportChoice: String, CaseIterable, Identifiable, Sendable {
    case original
    case png16, tiff16, exr, png, tiff, jpeg
    case prores4444, prores422hq, hevc10, h264
    case wav, m4a
    var id: String { rawValue }

    var video: Bool {
        switch self {
        case .prores4444, .prores422hq, .hevc10, .h264: true
        default: false
        }
    }

    /// For a sound: written by the core to the sample, its trim cut.
    var sound: Bool { self == .wav || self == .m4a }

    /// What it writes: "image", "video" or "audio" (an asset's kind).
    var kind: String { sound ? "audio" : video ? "video" : "image" }

    /// The file's extension; the original keeps its own.
    var ext: String? {
        switch self {
        case .original: nil
        case .png16, .png: "png"
        case .tiff16, .tiff: "tif"
        case .exr: "exr"
        case .jpeg: "jpg"
        case .prores4444, .prores422hq, .hevc10: "mov"
        case .h264: "mp4"
        case .wav: "wav"
        case .m4a: "m4a"
        }
    }

    /// The format, whatever its depth: the 16-bit choices as their
    /// 8-bit ones.
    var format: ExportChoice {
        switch self {
        case .png16: .png
        case .tiff16: .tiff
        default: self
        }
    }

    /// The choice in `format` at a depth: 16 bits per channel or 8. Only
    /// PNG and TIFF have one.
    func deep(_ on: Bool) -> ExportChoice {
        switch format {
        case .png: on ? .png16 : .png
        case .tiff: on ? .tiff16 : .tiff
        default: self
        }
    }

    var hasDepth: Bool { format == .png || format == .tiff }

    func label(originalExt: String) -> String {
        switch self {
        case .original:
            String(localized: "Original (\(originalExt.uppercased()))")
        case .png, .png16: String(localized: "PNG")
        case .tiff, .tiff16: String(localized: "TIFF")
        case .exr: String(localized: "OpenEXR (linear, half float)")
        case .jpeg: String(localized: "JPEG")
        case .prores4444: String(localized: "ProRes 4444 (keeps alpha)")
        case .prores422hq: String(localized: "ProRes 422 HQ")
        case .hevc10: String(localized: "HEVC")
        case .h264: String(localized: "H.264")
        case .wav: String(localized: "WAV (uncompressed)")
        case .m4a: String(localized: "AAC (.m4a)")
        }
    }
}

/// The assistant's rewrite of the prompt, offered until accepted or
/// dismissed. `aspect` ("3:2") or `follow` ("<image1>"): the shape a
/// model maker's rewriter wrote it for.
struct EnhancedPrompt: Sendable {
    var prompt: String
    var negative: String
    var title: String
    var aspect: String
    var follow: String
}

/// B set aside while the compare is off (AppModel.toggleCompare).
struct ParkedSide {
    var image: CGImage
    var fit: CanvasFit
    var label: String
    var surface: IOSurface?
    var compared: String?
    var aLabel: String
}

/// A prompt open in immersive editing (PromptStudio), as a tab: its
/// words as the text view holds them -- mentions (U+FFFC, each of
/// `mentions`) and positional tags in place -- the prompt asset they came
/// from, and the assistant's suggestion for them. The row of media is
/// the session's, shared by every tab: a tag names a place in it, a
/// mention one of its media.
struct PromptTab: Identifiable, Sendable {
    let id = UUID()
    var marked = ""
    var mentions: [UUID] = []
    var assetId: String?
    var enhanced: EnhancedPrompt?
    /// Its name, given by the person before its prompt is in Assets.
    var name: String?
    /// A TEXT ASSET opened from Assets to look at (DESIGN §10c): the asset,
    /// its words as they were, and whether something was made from it --
    /// READ ONLY, then. DO-NOT-APPLY while it is read only or unedited: it
    /// never becomes the prompt box's (AppModel.tabDoNotApply).
    var source: String?
    var sourceWords: String?
    var readOnly = false

    /// Its words alone: mentions, tags and Markdown's markers left out.
    var words: String {
        marked.replacingOccurrences(of: "\u{FFFC}", with: "")
    }
}

/// A file put into the prompt: staged in the row over its text, as a
/// thumbnail, and mentioned in the text where it was dragged.
struct PromptAttachment: Identifiable, Hashable, Sendable {
    let id: UUID
    let url: URL
    let kind: String  // "image" | "video" | "audio" | "file"
    /// The BASE: the picture being edited -- the result dragged from the
    /// stage, or a picture dropped on the stage. At most one; the other
    /// pictures are REFERENCES, drawn from as they are.
    var isBase = false
    /// The asset it stands for, once known: one put in from the asset
    /// list, or the MODIFIED copy made when it was first changed here --
    /// a file of its own resolves when its import lands.
    var asset: String?
    /// `asset` is this item's own modified copy, made here: changed in
    /// place. Any other asset is copied before it is changed.
    var ownCopy = false
    /// A clip CONTINUED (Ref2VA): its tail is what the new clip carries
    /// on from -- picture and sound -- rather than a reference to draw
    /// on. At most one.
    var continues = false
    /// What its copy was made from.
    var original: String?
}

struct LogLine: Identifiable {
    let id = UUID()
    let level: String
    let text: String
}

struct JobInfo {
    var title: String
    var purpose: String
    var state: String
    var progress: Double
    /// The model a download fetches.
    var model = ""
    /// What it is doing now, while it runs.
    var phase: JobPhase?
}

/// What a running job is doing -- the core's phases (engine.h, kPhase*)
/// -- and how far along it is, counted as finely as vpipe counts it: a
/// denoise per transformer block, a decode per tile, a download per
/// byte. Between counts the core's ESTIMATE moves on at the recent pace,
/// at least once a second, never past the next count: a block of a long
/// clip can take 20 s.
struct JobPhase: Equatable {
    var phase: String
    var done = 0
    var total = 0
    var detail = ""
    var label = ""
    var estimate = -1.0   // 0...1; -1 while nothing is counted
    var rate = 0.0        // of the whole per second; 0 unknown
    /// What an uncounted phase has made so far: a song's score in
    /// tokens, the song in seconds; nil for none.
    var made: Double?
    /// Seconds the whole job has left (core controller/job-timing.h):
    /// nil until a count has passed 5%.
    var left: Double?

    init(phase: String) { self.phase = phase }

    /// From a job.progress event; nil for one that names no phase (an
    /// import's).
    init?(_ p: [String: Any]) {
        guard let phase = p["phase"] as? String else { return nil }
        self.phase = phase
        done = p["done"] as? Int ?? 0
        total = p["total"] as? Int ?? 0
        detail = p["detail"] as? String ?? ""
        label = p["label"] as? String ?? ""
        estimate = p["estimate"] as? Double ?? -1
        rate = p["rate"] as? Double ?? 0
        left = p["left"] as? Double
        made = p["made"] as? Double
    }

    /// The phases in the order a generation runs them.
    static let order = ["prepare", "references", "score", "song", "speech",
                        "transcribe", "summarize", "denoise", "decode",
                        "sound", "restore", "finish"]

    /// A phase's name, as the inspector lists how long each took -- a
    /// song's sound rendered and decoded, a clip's soundtrack.
    static func name(_ phase: String, kind: Modality = .image) -> String {
        switch phase {
        case "prepare": String(localized: "Preparing")
        case "references": String(localized: "Reading the pictures")
        case "score": String(localized: "Planning the score")
        case "song": String(localized: "Writing the song")
        case "speech": String(localized: "Speaking")
        case "transcribe": String(localized: "Transcribing")
        case "summarize": String(localized: "Summarizing")
        case "denoise": kind == .audio ? String(localized: "Rendering")
            : String(localized: "Generating")
        case "decode": String(localized: "Decoding")
        case "sound": kind == .audio ? String(localized: "Decoding")
            : String(localized: "Soundtrack")
        case "restore": String(localized: "Restoring detail")
        case "finish": String(localized: "Finishing")
        case "export": String(localized: "Exporting")
        default: phase
        }
    }

    /// How far along (the estimate between counts); nil while nothing is
    /// counted.
    var fraction: Double? {
        if estimate >= 0 { return min(1, estimate) }
        return total > 0 ? min(1, Double(done) / Double(total)) : nil
    }

    /// "47%", "47.3%", "47.35%": as many decimals as it takes to move at
    /// least every five seconds at the pace it is going (up to three: a
    /// percent in 80 minutes). Cut, not rounded, so it never shows the
    /// next figure early.
    var percent: String? {
        guard let f = fraction else { return nil }
        let five = rate * 5   // of the whole, in five seconds
        let digits = rate <= 0 || five >= 0.01 ? 0
            : five >= 0.001 ? 1 : five >= 0.0001 ? 2 : 3
        let scale = pow(10, Double(2 + digits))
        let cut = ((f * scale) + 1e-9).rounded(.down) / scale
        return cut.formatted(.percent.precision(.fractionLength(digits)))
    }

    /// "Generating · 47%", "Preparing…", "Writing the song · 1:12": what
    /// a job that makes `kind` is doing.
    func caption(kind: Modality) -> String {
        let name: String
        switch phase {
        case "prepare": return String(localized: "Preparing…")
        case "finish": return String(localized: "Finishing…")
        case "references": name = String(localized: "Reading the pictures")
        // A song's two passes before its sound: uncounted (the model says
        // when each ends), the song's length growing as it is written.
        case "score": return String(localized: "Planning the score…")
        case "song":
            guard let made else {
                return String(localized: "Writing the song…")
            }
            let t = AppModel.songTime(made)
            return String(localized: "Writing the song · \(t)")
        // Speech: its seconds so far -- counted when a length was asked.
        case "speech":
            if percent == nil {
                guard let made else { return String(localized: "Speaking…") }
                let t = AppModel.songTime(made)
                return String(localized: "Speaking · \(t)")
            }
            name = String(localized: "Speaking")
        case "denoise":
            name = kind == .audio ? String(localized: "Rendering the sound")
                : String(localized: "Generating")
        case "decode":
            name = kind == .video ? String(localized: "Decoding the clip")
                : String(localized: "Decoding the picture")
        case "sound":
            name = kind == .audio ? String(localized: "Decoding the sound")
                : String(localized: "Making the soundtrack")
        case "restore": name = String(localized: "Restoring detail")
        case "export": name = String(localized: "Exporting")
        case "download": name = label.isEmpty
            ? String(localized: "Downloading")
            : String(localized: "Downloading \(label)")
        default: name = label   // the engine's own words
        }
        guard let pct = percent else { return name }
        return String(localized: "\(name) · \(pct)")
    }
}

@MainActor @Observable
final class AppModel {
    let core: CoreService?
    var startupError: String?

    // Navigation
    var navOpen = false
    var screen: AppScreen = .editor
    /// The Log screen's rows (read while it is shown).
    let logState = LogState()

    // Title bar
    /// Simple mode's name in the title bar. Simple mode works in the
    /// shared default project, so its session is anonymous: a name that
    /// is fresh each launch.
    var anonymousName = AppModel.randomAnonymousName()
    /// The open project is the anonymous session -- incognito: a package
    /// in the temporary directory, kept while the session lives and
    /// deleted, with everything made in it, on Start Over and at exit.
    private(set) var isAnonymous = false
    /// The title is being renamed (a field in the title bar).
    var renamingTitle = false
    var inspectorOpen = false
    /// The status bar along the window's bottom (View › Show Status Bar):
    /// the machine's load and thermal state, remembered between launches
    /// -- except by a scripted snapshot run, which neither reads nor
    /// writes it.
    var showsStatusBar = AppModel.storedStatusBar {
        didSet {
            if Self.keepsViewState {
                UserDefaults.standard.set(showsStatusBar,
                                          forKey: Self.statusBarKey)
            }
        }
    }
    /// What the status bar shows (it polls only while the bar is shown).
    let monitor: MachineMonitor
    /// Auto-update (Sparkle, when built in): Check for Updates…, and
    /// Settings › General › Updates.
    let updater = Updater()
    private static let statusBarKey = "view.statusBar"
    private static var keepsViewState: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
    }
    private static var storedStatusBar: Bool {
        keepsViewState && UserDefaults.standard.bool(forKey: statusBarKey)
    }
    /// The pointer is over the title bar: its bottom edge shows, as in
    /// Preview.
    var titlebarHover = false
    /// The pointer is just outside the result on the stage: it can be
    /// dragged -- into the prompt, as the picture to edit.
    var stageDragReady = false
    /// The markup toolbar (its tools are still to come).
    var markupOpen = false
    /// The title bar's search: it finds the text in the prompt and the
    /// settings in the composer's cards.
    ///
    /// Its effects run from the view (RootView's onChange), not a didSet:
    /// a change made inside an @Observable property's didSet happens
    /// within that property's own mutation, and the card it opened was
    /// never seen by the views.
    var searchText = ""
    /// Bumped by Find (⌘F) to focus the search field.
    var findRequest = 0
    /// Bumped to open the share button's menu (a scripted snapshot).
    var shareRequest = 0
    /// The composer rows that match the search (SettingsRow ids).
    var searchHits: Set<String> = []
    /// The card that was open before a search opened another.
    private var panelBeforeSearch: ComposerPanel?
    private var searchOpenedPanel = false

    // Machine and models
    var hardware: HardwareInfo?
    /// Settings › Capabilities: families, features, resources.
    var capabilityTree: CapabilityTree?
    /// Settings › Agentic Helper: the helpers, the chosen one, the one in
    /// use.
    var helpers: HelperList?
    /// The fleet (DESIGN §11): this Mac's place in one, the job it serves
    /// for a member (its window given over to it, the mark in blue), the
    /// offers waiting for an answer.
    var fleet: FleetStatus?
    var fleetServing: FleetServing?
    var fleetAsks: [FleetJob] = []
    var capabilities: [CapabilityStatus] = []
    var catalog: [CatalogModel] = []
    var engine = ""
    var paths: PathsInfo?

    // Project
    var projectId: String?
    var projectName = ""
    var projectPath = ""
    var assets: [AssetDTO] = []
    /// The asset list's folders, in order.
    var assetFolders: [FolderDTO] = []
    /// The project's output: what exporting it writes (ProjectOutput).
    var projectOutput = OutputSettingsDTO()
    /// The folders folded in the asset list (each opens by default).
    var closedFolders: Set<String> = []
    /// The asset whose name is being typed in the asset list.
    var renamingAsset: String?

    // Prompt
    var prompt = ""
    /// Bumped when `prompt` is replaced programmatically, so the inline
    /// text view reloads it.
    var promptRevision = 0
    /// The prompt's MEDIA, staged in the row over its text, in the order
    /// they go to the model: the base (the picture to edit) leads, then
    /// the references as the row has them.
    var promptAttachments: [PromptAttachment] = []
    /// Their thumbnails, as they load.
    var referenceThumbs: [UUID: CGImage] = [:]
    /// The picture whose pencil the EDIT-BASE SUGGESTION points at: the
    /// first picture staged in an image prompt, not marked the base
    /// (DESIGN §10a). Gone with a click anywhere else.
    var baseHint: UUID?
    /// A task's latest preview frame, by the asset it makes: its row's
    /// picture in Assets while it runs.
    var taskThumbs: [String: CGImage] = [:]
    /// The staged media the text MENTIONS, in order (a thumbnail dragged
    /// from the row into the text): one per U+FFFC of `promptMarked`.
    var promptMentions: [UUID] = []
    /// The prompt with its mentions in place: U+FFFC where each sits.
    /// Sent with the assets at those places ("inline"), the core turns a
    /// picture into what the model calls it ("<image2>").
    var promptMarked = ""
    /// The row item ACTIVE on the stage (clicked in the row): its own
    /// copy is what every panel works on; none: the stage's own work.
    var focusedReference: UUID?
    @ObservationIgnored var stageReturn: StageReturn?
    @ObservationIgnored var lastStageDrag: URL?
    /// The picture dropped on the stage, while the stage shows it: it
    /// drags back into the prompt from the band around it, as a result
    /// does -- so a base taken out of the prompt can be put back.
    private(set) var stageBaseURL: URL?
    /// The base of the generation in flight: compared against (B) when
    /// its result lands. `fitted`: the engine edits it, so it is cropped
    /// to the result's frame and lines up with it (CanvasFit).
    private var generationBase: (image: CGImage, label: String,
                                 fitted: Bool)?
    var negative = ""
    var intent: IntentInfo?

    // Generation settings (the simple-mode drawer)
    /// What to make: a picture or a clip.
    var modality: Modality = .image
    /// The model field: "" is AUTO -- the core's pick for the modality and
    /// for what Start does (generate, or edit when pictures are attached;
    /// `auto`) -- else a model, which runs for both (editing only if it
    /// can; otherwise the pictures stay compare-only).
    var modelChoice = ""
    /// Auto's picks, by modality and op (core auto_models).
    private(set) var auto: [String: [String: AutoChoice]] = [:]
    /// The output size: a preset (size class, orientation and, unless
    /// square, aspect ratio), or a CUSTOM size -- typed, or the base's
    /// own, taken when a base comes into the prompt. A custom size sets
    /// the orientation itself and has no aspect ratio to pick.
    var orientation: Orientation = .square
    var aspectRatio: AspectRatio = .threeTwo
    var sizeClass: SizeClass = .standard
    var customSize: PixelSize?
    /// The custom size is the base's own (adoptBaseSize), not one the
    /// user chose: the assistant is not told it (an edit follows its
    /// base anyway).
    private var sizeIsBase = false
    var preference: Preference = .balanced
    /// Favor's Custom: each model family's option values, kept between
    /// launches, and the preset Custom starts from (and sends with them).
    var customTunings: [String: Tuning] = AppModel.loadCustomTunings()
    var customBase: Preference = .balanced
    /// The options the Custom panel draws, for the model Start runs.
    var tuningInfo: TuningInfo?
    /// The Custom panel is open.
    var showsTuning = false
    /// The Prompt Editor's popover of the generation card's settings (its
    /// toolbar's button) -- Tune's options open inside it.
    var showsGenerationSettings = false
    /// It was open when the mouse last went down. The popover shuts itself
    /// on a press outside it -- on the very control that opens it, too --
    /// before that click lands; asked then whether to open, a click on
    /// Custom saw it shut and opened it again.
    @ObservationIgnored private var tuningOpenAtPress = false
    @ObservationIgnored private var tuningPressWatch: Any?
    var seedText = ""      // empty = random
    /// A clip's length: a preset, in seconds as offered, or -- nil -- a
    /// length typed in the Length field, kept as `clipCustomFrames`.
    /// Either is made as the nearest length the video model makes
    /// (`clipFrames`: 17n + 5 frames for H3).
    var clipSeconds: Int? = AppModel.defaultClipSeconds
    var clipCustomFrames: Int?
    static let clipLengths = [2, 3, 5, 10, 15]
    static let defaultClipSeconds = 5
    /// A clip made from references keeps the first sound reference as
    /// its soundtrack -- a music video's song, as it is -- rather than the
    /// one generated with its picture.
    var keepSongSound = true
    /// The prompt asset the box's words came from (DESIGN §10c): one used
    /// in the box, or the one the last Start captured. Start gives it
    /// back to the core, which keeps it for the same words, changes it in
    /// place while nothing is made from it, or makes a new one.
    var promptAssetId: String?
    /// The name the person gave the box's prompt (the Prompt Editor's
    /// tab, DESIGN §10c) while it is in no prompt asset yet: Start names
    /// the prompt it captures so. One in Assets is named there
    /// (`promptTabName`).
    var promptName: String?
    /// A song's plan: the score it writes before it sings.
    var songPlan: SongPlan = .full
    /// The longest a song may run, in seconds; nil: as long as the model
    /// makes it (it ends the song itself, up to its cap -- YuE2's six
    /// minutes).
    var songSeconds: Int?
    static let songLengths = [30, 60, 120, 240]
    /// Sound CAPTURED into a blank composition of sound alone (DESIGN
    /// §7b): the sources there, the one chosen (its id), and the
    /// recording's state as the core reports it (Capture.swift).
    var captureSources: [CaptureSourceDTO] = []
    var captureSource = ""
    var capture = CaptureStateDTO()
    @ObservationIgnored var capturePoll: Task<Void, Never>?
    /// What the attach menu is capturing for the prompt's row: a sound,
    /// or the camera (AttachCapture.swift).
    var attachCapture: AttachCapture?
    /// The row item a capture just became, while the card flies into its
    /// thumbnail (ComposerStack's ghost): hidden until it lands there.
    var capturedItem: UUID?
    /// That thumbnail's frame in the prompt card: where the ghost goes.
    var capturedThumbFrame: CGRect?
    var camera = CameraStateDTO()
    @ObservationIgnored var cameraPoll: Task<Void, Never>?
    /// The asset VIEWED from the list -- a single click -- and the one to
    /// return to when it leaves: the ACTIVE one (nil: an empty stage).
    var viewedAsset: String?
    @ObservationIgnored var viewReturn: String?
    /// Where the asset list and the stage card are in the window (top-left
    /// origin): a click outside both ends a viewing (endViewingIfOutside).
    @ObservationIgnored var assetListFrame: CGRect = .zero
    @ObservationIgnored var stageCardFrame: CGRect = .zero
    /// The stage card off the desk: 0 on it, 1 lifted -- larger, faded
    /// out. A viewed asset is put down with it, and taken off.
    var stageLift = 0.0
    /// The anonymous session's owner lock (a file descriptor held while
    /// this Valtz runs it; -1 none): AppModel.removeOrphanSessions.
    @ObservationIgnored var sessionLock: Int32 = -1
    /// About how long SPEECH runs (MOSS-TTS, DESIGN §4g), in seconds; nil:
    /// as long as its words take.
    var speechSeconds: Int?
    static let speechLengths = [5, 10, 20, 30, 60]
    /// The output shape of the modality not in use, for when it comes
    /// back: pictures and clips are shaped apart (a clip starts
    /// landscape, 16:9, at the Draft size).
    private var otherShapes: [Modality: OutputShape] = [:]
    /// Which composer panel is open (at most one).
    var openPanel: ComposerPanel?
    /// The Crop card's zoom: X and Y together, their ratio kept (the
    /// lock between them), or apart. Remembered between launches.
    var cropZoomLocked = !AppModel.keepsViewState || UserDefaults.standard
        .object(forKey: AppModel.cropZoomLockKey) as? Bool ?? true {
        didSet {
            if Self.keepsViewState {
                UserDefaults.standard.set(cropZoomLocked,
                                          forKey: Self.cropZoomLockKey)
            }
        }
    }
    static let cropZoomLockKey = "crop.zoomLocked"
    /// The TIMELINE in the prompt's place (DESIGN §10a Timeline): asked
    /// for (its switch beside the box), shown while the stage works on a
    /// timeline or a still with pages (`timelineOpen`).
    var timelineWanted = false
    /// The timeline's rows of layers in view (its ⌃ ⌄), remembered
    /// between launches (not by a scripted snapshot run).
    var timelineRows = AppModel.storedTimelineRows {
        didSet {
            if Self.keepsViewState {
                UserDefaults.standard.set(timelineRows,
                                          forKey: Self.timelineRowsKey)
            }
        }
    }
    static let timelineRowRange = 1...12
    private static let timelineRowsKey = "view.timelineRows"
    private static var storedTimelineRows: Int {
        let n = keepsViewState
            ? UserDefaults.standard.integer(forKey: timelineRowsKey) : 0
        return n > 0 ? min(n, timelineRowRange.upperBound) : 3
    }
    /// The timeline's zoom: how many times the whole of it fits (1: all
    /// of it in view).
    var timelineZoom = 1.0
    /// The timeline's SCISSORS: a click on a clip cuts it there.
    var timelineCutting = false
    /// The file panels' folders kept for this session only (an anonymous
    /// one's attachments and exports; FileHistory).
    @ObservationIgnored var sessionPanelFolders: [String: URL] = [:]
    /// Layer folders folded shut, in the Layers section and the timeline
    /// ("<composition>/<folder>"): how they are looked at, not kept.
    var foldedLayerFolders: Set<String> = []
    /// A long prompt stays three rows tall and scrolls, instead of the
    /// box growing with it (to half the window).
    var promptCompact = false
    /// The box was made compact by an Adjust, Crop or Trim tray opening
    /// (not by its own toggle): it grows back when that tray goes.
    @ObservationIgnored private var compactForPanel = false
    /// IMMERSIVE prompt editing (View › Edit Prompt in Full; PromptStudio):
    /// the prompt the window's height under a toolbar of its own, the
    /// stage a small preview at the top left, the cards put away -- and
    /// several prompts open, as tabs. The ACTIVE tab is the prompt itself
    /// (`prompt`, `promptMarked`, ...); the others wait in `promptTabs`.
    /// Back in the box, the prompt is the tab last open.
    var promptImmersive = false
    /// The editor is being EDITED: something typed (or styled) in it
    /// since it opened. Then a click on an asset only views it in the
    /// small stage, the editor kept; a double-click (to work on it) still
    /// takes the editor away.
    var editorEdited = false
    /// The Prompt Editor's toolbar's foot, in the window: the small stage
    /// keeps a margin under it.
    var editorToolbarBottom: CGFloat = 0
    var promptTabs: [PromptTab] = []
    var activePromptTab = 0
    /// The tab whose name is being typed in its header, if any.
    var renamingPromptTab: UUID?
    /// The prompt box as it was when the editor opened: what it stays when
    /// the editor goes from a do-not-apply tab (closing tabs never
    /// changes it).
    @ObservationIgnored var boxTab: PromptTab?
    /// Markdown drawn as it reads (PromptMarkdown: bold bold, its markers
    /// hidden away from the caret) -- or the text as typed. Kept between
    /// launches.
    var promptMarkdown = UserDefaults.standard
        .object(forKey: "promptMarkdown") as? Bool ?? true {
        didSet { Self.keep(promptMarkdown, as: "promptMarkdown") }
    }
    /// The stage's corners rounded (View › Round Stage Corners) -- or
    /// square, to see a picture's own corners. Kept between launches.
    var roundStageCorners = UserDefaults.standard
        .object(forKey: "roundStageCorners") as? Bool ?? true {
        didSet { Self.keep(roundStageCorners, as: "roundStageCorners") }
    }
    var stageCornerRadius: CGFloat { roundStageCorners ? 10 : 0 }
    /// A view setting kept between launches -- except in a scripted
    /// snapshot run, which saves no choice (as with the language).
    nonisolated static func keep(_ value: Any, as key: String) {
        guard ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
        else { return }
        UserDefaults.standard.set(value, forKey: key)
    }
    /// The inspector's sections STACKED -- each under a header that folds
    /// it, several open at once, as an image editor's panels -- or one at
    /// a time, picked in its navigation row (the default). Kept between
    /// launches.
    var inspectorStacked = UserDefaults.standard
        .object(forKey: "inspectorStacked") as? Bool ?? false {
        didSet { Self.keep(inspectorStacked, as: "inspectorStacked") }
    }
    /// Stacked, the sections folded to their header. Kept between
    /// launches.
    var inspectorFolded: Set<InspectorTab> = Set(
        (UserDefaults.standard.stringArray(forKey: "inspectorFolded") ?? [])
            .compactMap(InspectorTab.init(rawValue:))) {
        didSet {
            Self.keep(inspectorFolded.map(\.rawValue).sorted(),
                      as: "inspectorFolded")
        }
    }

    /// Stacked, the open sections' shares of the height (each 1 until a
    /// divider between two is dragged; a double-click on one evens them).
    /// Kept between launches.
    var inspectorWeights: [InspectorTab: Double] = Dictionary(
        uniqueKeysWithValues: ((UserDefaults.standard
            .dictionary(forKey: "inspectorWeights") as? [String: Double])
            ?? [:]).compactMap { k, v in
                InspectorTab(rawValue: k).map { ($0, v) }
            }) {
        didSet {
            Self.keep(Dictionary(uniqueKeysWithValues: inspectorWeights.map {
                ($0.key.rawValue, $0.value)
            }), as: "inspectorWeights")
        }
    }
    /// A stacked section's share of the height.
    func inspectorWeight(_ tab: InspectorTab) -> Double {
        max(0.01, inspectorWeights[tab] ?? 1)
    }
    /// Stacked, each open section's height as laid out: what a divider's
    /// drag turns into shares.
    @ObservationIgnored var inspectorHeights: [InspectorTab: CGFloat] = [:]

    /// A section shown: picked -- or, stacked, unfolded.
    func revealInspector(_ tab: InspectorTab) {
        inspectorTab = tab
        if inspectorStacked { inspectorFolded.remove(tab) }
    }

    /// Stacked, a section's header clicked: folded, or open again.
    func toggleInspectorFold(_ tab: InspectorTab) {
        if inspectorFolded.contains(tab) {
            inspectorFolded.remove(tab)
        } else {
            inspectorFolded.insert(tab)
        }
    }
    /// The text view being written in -- the prompt's, or the Prompt
    /// Editor's suggestion beside it: what the toolbar's B / I / U format.
    @ObservationIgnored weak var promptTextView: NSTextView?

    // Adjustments (simple mode): of the picture on the stage
    /// Applied live to what the stage shows, and RECORDED ON THE IMAGE as
    /// its modifier (core project::Modifier) -- never baked into its
    /// file: they are applied when a file is made from it (Save..., an
    /// export) and, for the picture to edit, when the model receives it
    /// (the result's recipe records them). Layers will come; a set of
    /// adjustments may then apply to one of them only.
    /// For a CLIP on the stage it is what the clip shows at the frame on
    /// screen: its adjustment track's value there (`clipAdjustKeys`).
    var adjustments = ImageAdjustments() {
        didSet {
            if adjustments != oldValue && !keyedStage { renderAdjusted() }
        }
    }
    /// The write of `adjustments` to the asset, a moment after the last
    /// change (a slider drag is many changes).
    private var persistTask: Task<Void, Never>?

    // Crop and trim (simple mode): of what is on the stage
    /// The Crop panel's crop and rotation of the picture on the stage --
    /// like the adjustments, RECORDED ON THE IMAGE as its "crop" modifier
    /// and made real only where pixels leave Valtz. The stage lays it
    /// out live (`stageCropPlacement`).
    /// For a clip, its crop track's value at the frame on screen.
    var crop = CropSpec() {
        didSet { if crop != oldValue && !keyedStage { cropChanged() } }
    }
    /// A CLIP's adjustments and crop are tracks of keyframes (core
    /// media/keyframes.h): it starts with one key at its first frame, and
    /// the values between keys are interpolated linearly. The panels show
    /// and edit the values at the frame on screen.
    var clipAdjustKeys = Keyframes<ImageAdjustments>()
    var clipCropKeys = ClipCrop()
    /// The parts of the look a held Bypass button leaves out of what the
    /// stage shows -- only the showing: nothing recorded changes, and
    /// letting go shows them again.
    var bypassed: Set<KeyTrack> = []
    /// The layer of the picture on the stage that the panels and a
    /// generation work on ("" is the bottom one; core project::Layer).
    var selectedLayer = ""
    /// The layers selected in the layer editor (⌘-click adds one): the
    /// selected one among them; two can be merged.
    var selectedLayers: Set<String> = []
    /// The markup toolbar's tool, settings and work in hand.
    let markup = MarkupState()
    /// The inspector's section: the picture's information, or its layers.
    var inspectorTab: InspectorTab = .info
    /// Information › Canvas › Change…: the Canvas Size popover is open.
    var showsCanvasSize = false
    @ObservationIgnored private var compositing = false
    @ObservationIgnored private var compositeAgain = false
    private var clipKeysPersistTask: Task<Void, Never>?
    /// A 3 × 3 grid over what the stage shows -- a picture, a clip, a
    /// preview (View › Show Guides). Kept between launches.
    var showsGuides = UserDefaults.standard
        .object(forKey: "showsGuides") as? Bool ?? false {
        didSet { Self.keep(showsGuides, as: "showsGuides") }
    }
    /// Each picture to edit's crop, by file, as `baseAdjustments`.
    @ObservationIgnored
    private var baseCrops: [String: CropSpec] = [:]
    private var cropPersistTask: Task<Void, Never>?
    /// The selected layer's MARKS in what it shows (DESIGN §6a: a layer's
    /// time) -- in its source's own frames, a sound's milliseconds.
    var trim = TrimSpec() {
        didSet { if trim != oldValue { scheduleTrimPersist() } }
    }
    /// Where the selected layer STARTS on the timeline, in its frames.
    var trimOffset = 0 {
        didSet { if trimOffset != oldValue { scheduleTrimPersist() } }
    }
    /// The selected layer's SPEED and SOUND (DESIGN §6a) -- the Trim
    /// panel's rows -- keyed from its own start as its look is; and its
    /// pitch following its speed (a tape's) or held.
    var layerSpeed = Keyframes<LayerSpeed>() {
        didSet { if layerSpeed != oldValue { scheduleTrimPersist() } }
    }
    var layerSound = Keyframes<LayerSound>() {
        didSet { if layerSound != oldValue { scheduleTrimPersist() } }
    }
    var pitchFollowsSpeed = false {
        didSet {
            if pitchFollowsSpeed != oldValue { scheduleTrimPersist() }
        }
    }
    private var trimPersistTask: Task<Void, Never>?
    /// The frame the clip on the stage shows (its player reports it).
    var videoFrame = 0
    /// The page of the still on the stage it shows and the panels work on,
    /// from 0 (DESIGN §6a).
    var stagePage = 0
    /// Asks the stage's player to show a frame.
    var videoCommand: VideoCommand?
    /// The stage's player's rate, as it reports it (0 is paused).
    var videoRate: Float = 0
    /// The latest result as generated, before adjustments.
    private var stageOriginal: CGImage?
    /// The picture to edit as read, before adjustments (on the stage while
    /// `stageBaseURL` is set).
    private var stageBaseOriginal: CGImage?
    /// Each picture to edit's adjustments, by file: what Start lays on it.
    /// Not observed -- read at Start, and when the picture returns to the
    /// stage.
    @ObservationIgnored
    private var baseAdjustments: [String: ImageAdjustments] = [:]
    /// Its asset: what share and the inspector act on in simple mode.
    private(set) var stageAssetId: String?
    /// Where the CURRENT STATE shows -- the result as it is now (adjusted,
    /// its stack composed), or the picture to edit: A, B after a swap,
    /// nil while history states hold both sides (it is then kept aside,
    /// drawn as it changes, and Return to Current brings it back).
    private(set) var currentSlot: StageSlot? = .a
    @ObservationIgnored private var currentAside: (image: CGImage,
                                                   fit: CanvasFit,
                                                   label: String,
                                                   surface: IOSurface?)?
    /// The canvas the compare lines its pictures up on: the current
    /// state's pixel grid, kept while history states hold both sides.
    @ObservationIgnored private var stageCanvas: CGSize?
    /// The generation history, oldest first (core project::HistoryEntry).
    private(set) var history: [HistoryEntryDTO] = []
    /// The COMPARE is on: B beside A, and its controls under the stage --
    /// while B holds a picture, or turned on by hand with B still empty
    /// (to drop one there). A result fills B only when it changes what was
    /// there -- an edit, a new take over a picture in its layer -- not
    /// when it goes onto a layer of its own; the title bar's compare
    /// button turns it on and off, and a picture put in A or B turns it
    /// on.
    var compareOn: Bool { stage.b != nil || compareEngaged }
    /// Turned on by hand (its button), B or not.
    private(set) var compareEngaged = false
    /// B while the compare is off: what turning it on brings back.
    @ObservationIgnored private var parkedB: ParkedSide?
    private var adjustTask: Task<Void, Never>?

    // Assistant
    var enhanceJob: String?
    /// The suggestion as far as the assistant has written it (the core's
    /// assist.partial), shown while it writes; `enhanced` once it is done.
    var enhanceDraft = ""
    /// The tab the running suggestion is for (nil: the box, no tabs).
    var enhanceTab: UUID?
    var enhanced: EnhancedPrompt?

    // Jobs
    var jobs: [String: JobInfo] = [:]
    /// Exports running, and where each one goes.
    private var exportJobs: [String: URL] = [:]
    /// A clip's export, shown on the stage while it runs -- a long one is
    /// minutes, an hour's clip many: the frames it writes, small, as they
    /// go (a generation's preview), its progress and Stop. The stage is
    /// left as it was under it, and is there again when the export ends.
    /// (Going between the two while it runs is for later.)
    struct ExportShow {
        var job: String
        var file: String        // the destination's name
        var preview: CGImage?
        var started = Date.now
        /// When it should end, by the core's estimate; nil before one.
        var ends: Date?
    }
    var exportShow: ExportShow?
    /// The task the STAGE WATCHES -- its preview, its phase and time; nil
    /// while the stage shows anything else (the task runs on: Assets
    /// lists it, T0, T1, ...; a click there watches it again).
    var generationJob: String?
    /// The TASK QUEUE (DESIGN §3a): this project's generations, upscales
    /// and exports, in the order they run -- the core's list.
    var tasks: [TaskDTO] = []
    /// What the app keeps of each task it started (TaskContext), by job.
    @ObservationIgnored private var taskContexts: [String: TaskContext] = [:]
    /// When Start began it, and whether it makes a clip.
    var generationStarted: Date?
    /// When the running generation should end, by the core's estimate
    /// (JobPhase.left): nil until a count has passed 5%. Kept between
    /// events that carry none.
    var generationEnds: Date?
    /// What the running generation makes: a picture, a clip or a song.
    var generationKind: Modality = .image
    var generationIsClip: Bool { generationKind == .video }
    var liveStep = 0
    var liveSteps = 0

    var stage = ViewerState()
    /// Simple mode shows no result area until the first preview arrives.
    var stageVisible = false
    /// A finished clip plays again and again (the bar under the stage),
    /// rather than once.
    var loopClips = false
    /// Bumped to play the mark's light sweep once (MarkView).
    var markSweep = 0
    private var startupSweepDone = false
    private var lastStageResult: (image: CGImage, label: String)?

    var banner: String?

    /// An upscale running (Render Upscaled, the Crop panel): its job.
    var upscaleJob: String?

    /// The active layer's source, when it is a clip or a picture.
    private var activeLayerSource: AssetDTO? {
        guard let id = stageStack?.layerStack
                .first(where: { $0.id == activeLayer })?.source else {
            return nil
        }
        return assets.first { $0.id == id }
    }

    /// The Crop panel offers to RENDER UPSCALED (DESIGN §4f): the layer
    /// shows a clip scaled up at one size -- one key at most -- and an
    /// upscaler for clips runs here.
    var canUpscaleLayer: Bool {
        guard let src = activeLayerSource, !src.isDrawn,
              src.kind == "video" || src.kind == "image",
              capability("upscale-\(src.kind)")?.availability == "ready"
        else { return false }
        if keyedStage && clipCropKeys.place.keys.count > 1 { return false }
        return crop.scaleX > 1.001 || crop.scaleY > 1.001
    }

    /// Scaled up at one size, but what the layer shows is a COMPOSITION:
    /// an upscaler restores one picture or clip, so it is made flat first
    /// (Flatten First), then upscaled.
    var upscaleNeedsFlatten: Bool {
        guard let src = activeLayerSource, src.isDrawn,
              src.kind == "video" || src.kind == "image",
              capability("upscale-\(src.kind)")?.availability == "ready"
        else { return false }
        if keyedStage && clipCropKeys.place.keys.count > 1 { return false }
        return crop.scaleX > 1.001 || crop.scaleY > 1.001
    }
    /// The layer's composition being made flat (Flatten First).
    var flatteningLayer = false
    var activeLayerSourceName: String { activeLayerSource?.name ?? "" }

    /// Flatten First: the layer's composition made flat and shown in its
    /// place, its marks, look and placement kept -- then it can be
    /// upscaled. Off the main thread: a long timeline is encoded.
    func flattenLayerForUpscale() {
        guard let core, let projectId, let stack = stageStack,
              !flatteningLayer else { return }
        persistCrop()
        if clipOnStage { persistClipTracks() }
        let target = composedTarget(stack.id)
        let layer = activeLayer
        withAnimation(Self.motion) { flatteningLayer = true }
        Task { @MainActor [weak self] in
            let r = await Task.detached {
                core.layerOp(project: projectId, asset: target, "flatten",
                             layer: layer)
            }.value
            guard let self else { return }
            withAnimation(Self.motion) { self.flatteningLayer = false }
            if !r.ok {
                self.flash(r.message)
                return
            }
            self.layersChanged()
            self.selectLayer(layer)
            self.flash(String(localized: "Flattened: the layer shows a flat copy, which can be upscaled"))
        }
    }

    /// The size the upscale makes: the source's times the scale, even.
    var upscaleTarget: (width: Int, height: Int)? {
        guard let src = activeLayerSource, let w = src.info?.frame.w,
              let h = src.info?.frame.h, w > 0, h > 0 else { return nil }
        func even(_ v: Double) -> Int { max(2, Int((v / 2).rounded()) * 2) }
        return (even(Double(w) * max(1, crop.scaleX)),
                even(Double(h) * max(1, crop.scaleY)))
    }

    /// Render Upscaled: the layer's clip at its scale, by the upscaler; the
    /// layer then shows the result at scale 1 (core upscale_layer).
    func upscaleLayer() {
        guard let core, let projectId, let stack = stageStack,
              upscaleJob == nil else { return }
        persistCrop()
        if clipOnStage { persistClipTracks() }
        let r = core.upscaleLayer(["project": projectId,
                                   "asset": composedTarget(stack.id),
                                   "layer": activeLayer])
        guard r.ok, let job = r.job else {
            flash(r.message)
            return
        }
        withAnimation(Self.motion) {
            upscaleJob = job
            memoryFailure = nil
        }
    }

    func stopUpscale() {
        if let job = upscaleJob { cancel(job: job) }
    }

    /// "Restoring detail · 34%" once the frames come out; restoring, in
    /// words, while the first group is made.
    var upscaleCaption: String {
        guard let job = upscaleJob, let p = jobs[job]?.phase else {
            return String(localized: "Preparing…")
        }
        return p.phase == "restore" ? p.caption(kind: .video)
            : String(localized: "Restoring detail…")
    }

    /// The last generation refused for lack of memory: what it asked for
    /// and what to change (MemoryFailure), shown over the prompt until
    /// dismissed or the next Start.
    var memoryFailure: MemoryFailure?

    /// A setting to change for the clip to fit, highlighted while it is
    /// still at (or past) what failed: lowered, it is no longer marked.
    var suggestsShorterClip: Bool {
        guard let f = memoryFailure, let s = f.seconds,
              activeModality == .video else { return false }
        return Double(clipFrames) / clipFPS >= s - 0.01
    }
    var suggestsSmallerSize: Bool {
        guard let f = memoryFailure, let w = f.width, let h = f.height
        else { return false }
        let d = dimensions
        return d.width * d.height >= w * h
    }
    var suggestsFewerReferences: Bool {
        guard let f = memoryFailure, let n = f.references, n > 0
        else { return false }
        return promptAttachments.filter {
            ["image", "video", "audio"].contains($0.kind)
        }.count >= n
    }

    /// A row of the generation card the memory card points at.
    func memorySuggests(_ row: String) -> Bool {
        switch row {
        case SettingsRow.length: suggestsShorterClip
        case SettingsRow.size: suggestsSmallerSize
        default: false
        }
    }

    /// The memory card's suggestion: the generation card opened on it.
    func showMemorySuggestion() {
        withAnimation(Self.motion) { openPanel = .generate }
    }

    func dismissMemoryFailure() {
        withAnimation(Self.motion) { memoryFailure = nil }
    }
    var log: [LogLine] = []
    var showModels = false

    private var intentTask: Task<Void, Never>?
    private var bannerTask: Task<Void, Never>?

    /// Every state change the user sees. `VALTZ_ANIMATION_SCALE` (a dev
    /// knob) slows it down, to inspect or snapshot a transition midway.
    static let motion = Animation.smooth(duration: 0.45 * animationScale)
    static let animationScale = ProcessInfo.processInfo
        .environment["VALTZ_ANIMATION_SCALE"].flatMap(Double.init) ?? 1

    init() {
        let svc = CoreService()
        core = svc
        monitor = MachineMonitor(core: svc)
        guard let svc else {
            startupError = CoreService.createError
            return
        }
        engine = svc.engine
        refreshMachine()
        svc.startEvents { [weak self] ev in
            Task { @MainActor in self?.handle(ev) }
        }
        watchTextFocus()
        if let dev = ProcessInfo.processInfo
            .environment["VALTZ_DEFAULT_PROJECT"] {
            openProject(path: dev, createIfMissing: true)  // dev / test runs
        } else {
            Self.removeOrphanSessions()  // what a crash left behind
            startAnonymousSession()
        }
    }

    func shutdown() {
        core?.shutdown()  // jobs stopped, databases closed
        // This Valtz's own session and share files -- never another's.
        removeOwnSession()
    }

    /// Quit, and open again -- a language change takes effect at launch.
    /// The new Valtz starts once this one has gone: at launch it clears
    /// what a session left behind, which would be this one's still.
    func relaunch() {
        let pid = ProcessInfo.processInfo.processIdentifier
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/sh")
        p.arguments = [
            "-c",
            "while /bin/kill -0 \(pid) 2>/dev/null; do /bin/sleep 0.2; done; "
                + "/usr/bin/open \"$0\"",
            Bundle.main.bundleURL.path,
        ]
        do {
            try p.run()
        } catch {
            flash(String(localized: "Could not restart Valtz: \(error.localizedDescription)"))
            return
        }
        NSApp.terminate(nil)
    }

    /// Quitting would delete what this session has made (an anonymous
    /// session lives only while Valtz runs).
    var quitLosesWork: Bool { isAnonymous && !assets.isEmpty }

    // MARK: - Machine and models

    func refreshMachine() {
        guard let core else { return }
        hardware = DTO.decode(HardwareInfo.self, core.hardwareJSON())
        capabilities = DTO.decode([CapabilityStatus].self,
                                  core.capabilitiesJSON()) ?? []
        catalog = DTO.decode([CatalogModel].self, core.catalogJSON()) ?? []
        paths = DTO.decode(PathsInfo.self, core.pathsJSON())
        auto = DTO.decode([String: [String: AutoChoice]].self,
                          core.autoJSON()) ?? [:]
        capabilityTree = DTO.decode(CapabilityTree.self,
                                    core.capabilityTreeJSON())
        helpers = DTO.decode(HelperList.self, core.assistantsJSON())
        reloadFleet()
        // A chosen model that went away (deleted, moved) gives way to Auto.
        if !modelChoice.isEmpty,
           !modelOptions(for: activeModality).contains(where: {
               $0.model == modelChoice
           }) {
            modelChoice = ""
        }
    }

    /// What runs -- here, or on a fleet member (DESIGN §11) -- read
    /// again: the capabilities, the models, Auto's picks.
    func refreshRunnable() {
        guard let core else { return }
        capabilities = DTO.decode([CapabilityStatus].self,
                                  core.capabilitiesJSON()) ?? capabilities
        catalog = DTO.decode([CatalogModel].self, core.catalogJSON())
            ?? catalog
        auto = DTO.decode([String: [String: AutoChoice]].self,
                          core.autoJSON()) ?? auto
    }

    /// Auto's pick for `op` ("generate" | "edit") in the modality; "" when
    /// nothing runs here.
    func autoPick(_ op: String, _ m: Modality? = nil) -> String {
        auto[(m ?? activeModality).rawValue]?[op]?.chosen ?? ""
    }

    /// The modality Start makes: the card's Create row.
    var activeModality: Modality { modality }

    /// The models the field offers for a modality: installed, fitting
    /// this Mac, in the core's ranking -- for pictures, the ones that
    /// make or edit them; for clips, the video models.
    func modelOptions(for m: Modality) -> [ModelOption] {
        // Clips: the models that make one from words or a picture
        // (Ref2VA's references have no graph yet).
        let caps = switch m {
        case .image: ["text-to-image", "image-edit"]
        case .video: ["text-to-video", "image-to-video",
                      "reference-to-video"]
        case .audio: ["text-to-audio", "text-to-speech"]
        }
        // A quantized variant is its source, listed once by the source's
        // name: Favor picks which file runs (Fast and Med the 8-bit pack).
        var seen = Set<String>()
        var out: [ModelOption] = []
        for c in caps {
            for var o in capability(c)?.options ?? []
            where o.state == "installed" && o.fits {
                let base = baseModel(o.model)
                guard !seen.contains(base) else { continue }
                seen.insert(base)
                if base != o.model {
                    o.model = base
                    o.name = catalog.first { $0.id == base }?.name ?? o.name
                }
                out.append(o)
            }
        }
        return out
    }

    /// The model a quantized variant was made from (catalog `quantize`);
    /// any other, itself.
    func baseModel(_ id: String) -> String {
        guard let from = catalog.first(where: { $0.id == id })?
            .quantizeFrom, !from.isEmpty else { return id }
        return from
    }

    /// The model that runs when Start generates (no pictures to edit).
    var imageModel: String {
        modelChoice.isEmpty ? baseModel(autoPick("generate")) : modelChoice
    }

    /// The model that runs when Start edits: Auto's edit pick, or the
    /// chosen model if it can edit ("" if it cannot -- the pictures are
    /// then compare-only).
    var editModel: String {
        if modelChoice.isEmpty { return baseModel(autoPick("edit")) }
        return installedEditModels.contains { $0.model == modelChoice }
            ? modelChoice : ""
    }

    /// The model Start runs now.
    var runningModel: String {
        if activeModality == .audio { return audioModel }
        if usesReferences { return videoReferenceModel }
        return willEdit ? editModel : imageModel
    }

    /// The voice speech is spoken in (DESIGN §4g): the row's first sound,
    /// else its first clip -- whose sound it is. None: a voice of the
    /// model's own.
    var speechVoice: PromptAttachment? {
        promptAttachments.first { $0.kind == "audio" }
            ?? promptAttachments.first { $0.kind == "video" }
    }

    /// The model the Audio tab runs: the one chosen, else Auto's -- its
    /// speech list when the row holds a voice to speak in, as the core
    /// picks it.
    var audioModel: String {
        if !modelChoice.isEmpty { return modelChoice }
        let speech = baseModel(autoPick("edit", .audio))
        if speechVoice != nil, !speech.isEmpty { return speech }
        return baseModel(autoPick("generate", .audio))
    }

    /// The Audio tab SPEAKS: the model it runs reads words aloud
    /// (MOSS-TTS), rather than singing a song.
    var speaks: Bool {
        activeModality == .audio
            && catalog.first { $0.id == audioModel }?.capabilities
                .contains("text-to-speech") == true
    }

    /// Speech's Length as the brief says it: "about 10 s", or "Auto
    /// length" -- as long as the words take.
    var speechLengthText: String {
        guard let s = speechSeconds else {
            return String(localized: "Auto length")
        }
        return String(localized: "about \(String(s)) s")
    }

    /// What the Voice row says: the row's voice, or a voice of its own.
    var speechVoiceText: String {
        guard let v = speechVoice else {
            return String(localized: "Its own: put a sound in the prompt to speak in its voice")
        }
        let name = v.url.deletingPathExtension().lastPathComponent
        return v.kind == "video"
            ? String(localized: "\(name)'s sound") : name
    }

    /// The model a clip from references runs: the chosen one, if it reads
    /// references, else Auto's video edit pick (MiniMax H3 Ref2VA); ""
    /// when none can.
    var videoReferenceModel: String {
        if !modelChoice.isEmpty,
           catalog.first(where: { $0.id == modelChoice })?.capabilities
               .contains("reference-to-video") == true {
            return modelChoice
        }
        return autoPick("edit", .video)
    }

    /// Start makes a clip FROM REFERENCES (Ref2VA): the row holds a clip,
    /// a sound, a clip to continue, or pictures -- all but one picture
    /// marked as the one to open on, which stays FL2VA's first frame.
    var usesReferences: Bool {
        guard activeModality == .video, !promptAttachments.isEmpty,
              !videoReferenceModel.isEmpty else { return false }
        let media = promptAttachments.filter {
            ["image", "video", "audio"].contains($0.kind)
        }
        // (The generate model's own ability, not opensOnPicture: that
        // asks the running model, which is this.)
        if media.count == 1, media[0].kind == "image", media[0].isBase,
           catalog.first(where: { $0.id == imageModel })?.capabilities
               .contains("image-to-video") == true {
            return false
        }
        return !media.isEmpty
    }

    // MARK: The prompt's references (DESIGN §10c)

    /// A positional tag's kind for a row item's ("img", "vid", "aud").
    nonisolated static func refKind(_ kind: String) -> String? {
        ["image": "img", "video": "vid", "audio": "aud"][kind]
    }

    /// The positional tags in `text` ("<valtz_ref_img_0>": the row's
    /// first picture), each with its kind, index and range.
    nonisolated static func refTags(in text: String)
        -> [(text: String, kind: String, index: Int, range: NSRange)] {
        guard let re = try? NSRegularExpression(
            pattern: #"<valtz_ref_(img|vid|aud)_([0-9]{1,6})>"#) else {
            return []
        }
        let ns = text as NSString
        return re.matches(in: text,
                          range: NSRange(location: 0, length: ns.length))
            .map { m in
                (ns.substring(with: m.range),
                 ns.substring(with: m.range(at: 1)),
                 Int(ns.substring(with: m.range(at: 2))) ?? 0, m.range)
            }
    }

    /// How many of each kind the row holds: what a tag can name.
    var rowKindCounts: [String: Int] {
        var n: [String: Int] = [:]
        for att in promptAttachments {
            if let k = Self.refKind(att.kind) { n[k, default: 0] += 1 }
        }
        return n
    }

    /// What the row has at a tag's place: its `index`-th medium of the
    /// kind ("img", "vid", "aud"), in the row's order.
    func rowItem(kind: String, index: Int) -> PromptAttachment? {
        let of = promptAttachments.filter { Self.refKind($0.kind) == kind }
        return of.indices.contains(index) ? of[index] : nil
    }

    /// Does the row have a medium at the tag's place?
    func refTagBound(kind: String, index: Int) -> Bool {
        (rowKindCounts[kind] ?? 0) > index
    }

    /// The prompt's tags that name nothing in the row yet -- to fill in,
    /// as a function's arguments, before Start.
    var unboundRefTags: [String] {
        Self.refTags(in: prompt).filter {
            !refTagBound(kind: $0.kind, index: $0.index)
        }.map(\.text)
    }

    /// The row's media, in order, as the core counts the tags' places.
    var rowAssetIds: [String] {
        promptAttachments.filter { Self.refKind($0.kind) != nil }
            .compactMap { assetId(of: $0) }
    }

    /// A captured prompt into the box: its words, its media named by
    /// POSITION -- bound to whatever the row has there, red where it has
    /// nothing yet. The row is filled in as a function's arguments are;
    /// the prompt is the one Start keeps or changes (DESIGN §10c).
    func usePrompt(_ a: AssetDTO) {
        guard a.isPrompt, let text = a.text else { return }
        promptMentions = []
        promptMarked = text
        setPrompt(text)
        promptAssetId = a.id
        // Its name, if the person gave it one, is the asset's.
        promptName = nil
        dropSuggestion()
    }

    /// A sound is among the references: the Soundtrack row's choice.
    var hasSongReference: Bool {
        usesReferences && promptAttachments.contains { $0.kind == "audio" }
    }

    /// The row's media as references, in its order, and the clip it
    /// continues: asset ids, nil for one not imported yet.
    private var referenceRequest: (refs: [String?], cont: String?) {
        var refs: [String?] = []
        var cont: String?
        for att in promptAttachments where
            ["image", "video", "audio"].contains(att.kind) {
            if att.continues { cont = assetId(of: att) } else {
                refs.append(assetId(of: att))
            }
        }
        return (refs, cont)
    }

    /// What the model calls each staged reference ("<Picture 1>"), by
    /// asset -- the core's naming, in the order it reads them.
    var referenceTags: [String: String] {
        guard usesReferences, let core, let projectId else { return [:] }
        let r = referenceRequest
        return core.videoReferenceTags(
            project: projectId, model: videoReferenceModel,
            references: r.refs.compactMap { $0 }, continueFrom: r.cont)
    }

    /// The row's badge for a staged item: what the model calls it -- a
    /// picture's number ("1", the base first) for an edit, a clip's
    /// reference's kind and number ("P1", "V1", "A1").
    func referenceBadge(_ item: PromptAttachment) -> String? {
        if usesReferences {
            guard let id = assetId(of: item),
                  let tag = referenceTags[id] else { return nil }
            // "<Picture 1>" -> "P1": an identifier, the model's, as text.
            let name = tag.trimmingCharacters(in: CharacterSet(
                charactersIn: "<>"))
            return String(name.prefix(1)) + name.filter(\.isNumber)
        }
        return referenceNumber(item).map { String($0) }
    }

    /// The lengths Continue offers of the clip's end -- people guide a
    /// continuation with 2 to 5 seconds, never more -- each as the model
    /// takes it: rounded UP to its grid (17n + 5 frames for H3: 2 s is 56
    /// frames, 2.33 s at 24 fps), down to what the clip holds. Those that
    /// come out the same are offered once. The core makes them so.
    static let guideSeconds = [2, 3, 4, 5]
    struct GuideLength: Hashable {
        var asked: Int        // seconds asked for
        var frames: Int       // at the model's rate
        var seconds: Double   // what that is
    }
    var guideLengths: [GuideLength] {
        let m = catalog.first { $0.id == videoReferenceModel }
        let fps = m?.fps ?? 24
        let step = max(1, m?.frameGrid?.step ?? 1)
        let off = max(1, m?.frameGrid?.offset ?? 1)
        // The clip's length, in the model's frames.
        let fit = stageClipLength.map {
            Int((Double($0) * fps / max(stageFrameRate.fps, 1) + 1e-6)
                .rounded(.down))
        }
        var out: [GuideLength] = []
        for s in Self.guideSeconds {
            var f = Int((Double(s) * fps - 1e-6).rounded(.up))
            f = f <= off ? off : off + (f - off + step - 1) / step * step
            if let fit, f > fit {
                f = fit < off + step ? fit : off + (fit - off) / step * step
            }
            guard f > 0, !out.contains(where: { $0.frames == f }) else {
                continue
            }
            out.append(GuideLength(asked: s, frames: f,
                                   seconds: Double(f) / fps))
        }
        return out
    }

    /// Carry the clip on the stage on (Ref2VA) from its last `seconds`:
    /// a GUIDE is made of them -- a timeline of their own, as long as the
    /// model takes (core continuation_guide), in Assets -- and goes into
    /// the prompt's row as the clip to CONTINUE, picture and sound; the
    /// card makes a clip, and an empty prompt opens as the model's
    /// continuations do.
    func continueClip(seconds: Int) {
        guard let core, let projectId, let clip = currentClip,
              clip.kind == "video" else { return }
        flushPanels()
        let shown = guideLengths.first { $0.asked == seconds }
        let len = String(format: "%.1f", shown?.seconds ?? Double(seconds))
        let r = core.assetOp(project: projectId, "guide", [
            "asset": clip.id, "seconds": Double(seconds),
            "model": videoReferenceModel,
            "name": String(localized: "\(clip.name), last \(len) s"),
        ])
        guard r.ok, let guide = r["asset"] as? String else {
            flash(r.message)
            return
        }
        reloadAssets()
        continueWith(guide)
    }

    /// A FRAME of the clip on the stage, where the player is: a still
    /// composition showing it from that frame (core grab_frame), in
    /// Assets and in the prompt's row -- a picture to open a clip on, to
    /// edit, to refer to.
    func grabFrame() {
        guard let core, let projectId, let clip = currentClip,
              clip.kind == "video" else { return }
        flushPanels()
        let frame = videoFrame
        let r = core.assetOp(project: projectId, "grab", [
            "asset": clip.id, "frame": frame,
            "name": String(localized: "\(clip.name), frame \(String(frame))"),
        ])
        guard r.ok, let still = r["asset"] as? String else {
            flash(r.message)
            return
        }
        reloadAssets()
        if addAssetReferences([still]).isEmpty {
            flash(String(localized: "Grabbed in Assets"))
        }
    }

    /// The clip on the stage carried on as it is: its tail, up to its
    /// mark-out (the catalog's length).
    func continueClip() {
        guard let clip = currentClip, clip.kind == "video" else { return }
        continueWith(clip.id)
    }

    /// `asset` into the row as the clip to continue -- the only one.
    private func continueWith(_ asset: String) {
        guard let id = addAssetReferences([asset]).first else { return }
        var list = promptAttachments
        for j in list.indices { list[j].continues = list[j].id == id }
        withAnimation(Self.motion) { promptAttachments = list }
        if modality != .video {
            withAnimation(Self.motion) { setModality(.video) }
        }
        if prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty,
           let lead = catalog.first(where: { $0.id == videoReferenceModel })?
               .continuation, !lead.isEmpty {
            setPrompt(lead + " The video continues from the end of <Video 1>. ")
        }
        promptChanged()
    }

    /// A scripted snapshot asking for Continue's menu.
    var continueMenuRequest = 0

    /// A finished clip on the stage can be continued here.
    var canContinueClip: Bool {
        clipOnStage && currentClip?.kind == "video"
            && !autoPick("edit", .video).isEmpty && !isGenerating
    }

    /// Make pictures or clips. A model chosen for the other kind gives way
    /// to Auto.
    func setModality(_ m: Modality) {
        guard m != modality else { return }
        otherShapes[modality] = outputShape
        outputShape = otherShapes[m] ?? OutputShape.defaults(for: m)
        modality = m
        if !modelOptions(for: m).contains(where: { $0.model == modelChoice }) {
            modelChoice = ""
        }
        // A clip that opens on the picture in the prompt takes its shape.
        if m == .video, let b = baseAttachment, b.kind == "image" {
            adoptBaseSize(b.url)
        }
    }

    /// The size settings as one value, to keep per modality.
    private var outputShape: OutputShape {
        get {
            OutputShape(orientation: orientation, aspectRatio: aspectRatio,
                        sizeClass: sizeClass, customSize: customSize,
                        sizeIsBase: sizeIsBase)
        }
        set {
            orientation = newValue.orientation
            aspectRatio = newValue.aspectRatio
            sizeClass = newValue.sizeClass
            customSize = newValue.customSize
            sizeIsBase = newValue.sizeIsBase
        }
    }

    /// The catalog entry of the model Start runs.
    var runningModelInfo: CatalogModel? {
        catalog.first { $0.id == runningModel }
    }

    /// The length the video model makes nearest to `want` frames
    /// (offset + n × step), from one step past the offset up to its
    /// longest.
    func clipFrames(near want: Double) -> Int {
        let m = runningModelInfo
        guard let g = m?.frameGrid, g.step > 0 else {
            return max(1, Int(want.rounded()))
        }
        var n = max(1, Int(((want - Double(g.offset)) / Double(g.step))
            .rounded()))
        if let most = m?.maxFrames, most > 0 {
            n = min(n, max(1, (most - g.offset) / g.step))
        }
        return g.offset + n * g.step
    }

    /// The frames `seconds` makes on the video model.
    func clipFrames(seconds: Double) -> Int {
        clipFrames(near: seconds * clipFPS)
    }

    var clipFrames: Int {
        if let s = clipSeconds { return clipFrames(seconds: Double(s)) }
        return clipFrames(near: Double(clipCustomFrames
            ?? Self.defaultClipSeconds * 24))
    }
    var clipFPS: Double { runningModelInfo?.fps ?? 24 }

    /// "5.2 s": the clip as it will be made. A duration is a quantity,
    /// but written as text: one decimal, no grouping.
    var clipDurationText: String {
        String(format: "%.1f", Double(clipFrames) / clipFPS) + " s"
    }

    /// "124 frames · 5.2 s": what the length makes. Frame counts are
    /// written as text (no grouping).
    var clipLengthText: String {
        let n = String(clipFrames)
        return String(localized: "\(n) frames · \(clipDurationText)")
    }

    /// A length typed in the Length field: frames ("124 frames", "124 f")
    /// or seconds ("5 s", "5.2 sec", or a bare number), in English or in
    /// the UI's language. It is made as the nearest length the model
    /// makes; the preset with that length is chosen if there is one.
    /// False when it cannot be read.
    @discardableResult
    func setClipLength(_ text: String) -> Bool {
        guard let want = Self.parseClipLength(text, fps: clipFPS) else {
            return false
        }
        let frames = clipFrames(near: want)
        if let s = Self.clipLengths.first(where: {
            clipFrames(seconds: Double($0)) == frames
        }) {
            clipSeconds = s
            clipCustomFrames = nil
        } else {
            clipSeconds = nil
            clipCustomFrames = frames
        }
        return true
    }

    /// Frames wanted, from "124 frames", "124f", "5 s", "5.2 sec", "5",
    /// "124 帧", "5 秒" -- or the field's own "124 frames · 5.2 s" (its
    /// frames).
    nonisolated static func parseClipLength(_ text: String,
                                            fps: Double) -> Double? {
        let t = text.lowercased().trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ",", with: ".")
        let digits = t.prefix { $0.isNumber || $0 == "." }
        guard let v = Double(digits), v > 0, v.isFinite else { return nil }
        let unit = t.dropFirst(digits.count)
            .trimmingCharacters(in: .whitespaces)
        let frames = ["f", "fr", "frame", "frames", "帧"]
        let seconds = ["", "s", "sec", "secs", "second", "seconds", "秒"]
        if frames.contains(where: { unit == $0 || unit.hasPrefix($0 + " ") })
            || unit.hasPrefix("frames") || unit.hasPrefix("帧") {
            return v
        }
        if seconds.contains(unit) { return v * fps }
        return nil
    }

    /// "2:00", "0:30": a song's length, minutes and seconds.
    nonisolated static func songTime(_ seconds: Double) -> String {
        let s = max(0, Int(seconds.rounded()))
        return String(format: "%d:%02d", s / 60, s % 60)
    }

    /// The Length row's choice as the brief says it: "up to 2:00", or
    /// "Auto length" -- the model ends the song itself.
    var songLengthText: String {
        guard let s = songSeconds else {
            return String(localized: "Auto length")
        }
        let t = Self.songTime(Double(s))
        return String(localized: "up to \(t)")
    }

    /// The prompt as a song's words -- its style, and its lyrics under
    /// section headers -- as the core reads them.
    var songWords: SongWords {
        core?.songText(prompt) ?? .none
    }

    /// What the song will sing, as the Lyrics row says it: "3 sections ·
    /// 12 lines", or that it has no words -- the music from the
    /// description alone.
    var songLyricsText: String {
        let w = songWords
        guard !w.lyrics.isEmpty else {
            return String(localized: "None: the music from the description")
        }
        // Each counted in its own plural (the String Catalog's).
        return String(localized: "\(w.sections) sections") + " · "
            + String(localized: "\(w.lines) lines")
    }

    /// What the Length row's choice does: the model ends a song itself,
    /// up to its longest; a song past a length chosen is cut there.
    var songLengthHint: String {
        guard let s = songSeconds else {
            let most = AppModel.songTime(runningModelInfo?.maxSeconds ?? 360)
            return String(localized: "The model ends it, by \(most) at most")
        }
        let t = AppModel.songTime(Double(s))
        return String(localized: "Cut at \(t) if it runs longer")
    }

    /// A lyrics file (.txt) into the prompt, after its words: the song
    /// sings it -- vpipe's songs-from-lyrics, one file a song. A file
    /// without section headers is sung as one verse.
    func addLyrics(from url: URL) {
        guard var text = (try? String(contentsOf: url, encoding: .utf8))
            ?? (try? String(contentsOf: url, encoding: .isoLatin1)) else {
            flash(String(localized: "Could not read \(url.lastPathComponent)"))
            return
        }
        text = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty else { return }
        if (core?.songText(text).sections ?? 0) == 0 {
            text = "[Verse]\n" + text
        }
        let lead = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        prompt = lead.isEmpty ? text : lead + "\n\n" + text
        promptMarked = prompt
        promptMentions = []
        promptRevision += 1
    }

    /// The model Start runs can open a clip on a picture (the picture to
    /// edit becomes its first frame).
    var opensOnPicture: Bool {
        activeModality == .video
            && runningModelInfo?.capabilities.contains("image-to-video")
                == true
    }

    /// What a preference does on the model Start runs: its step count --
    /// a few-step adapter's when one is installed ("6 steps · Turbo") --
    /// for a clip, the flow matching's for a song; the scaling for a
    /// picture.
    var preferenceHint: String {
        if preference == .custom { return customSummary }
        guard let p = runningModelInfo?.presets?[preference.rawValue] else {
            return preference.hint
        }
        // It runs with a LoRA that is not downloaded: Start asks for it.
        if !p.missing.isEmpty { return String(localized: "Needs a Turbo LoRA") }
        // Speech: how its weights are held, and its int8 GEMMs.
        if speaks {
            let w = p.w8 == true ? String(localized: "8-bit weights")
                                 : String(localized: "bf16 weights")
            return p.i8Gemm ? String(localized: "\(w) · int8 GEMMs") : w
        }
        let steps = p.turbo.isEmpty
            ? String(localized: "\(p.steps) steps")
            : String(localized: "\(p.steps) steps · Turbo")
        // Before M5, Fast and Med give the Neural Engine a share.
        return p.aneFfn == true ? String(localized: "\(steps) · ANE") : steps
    }

    /// The LoRAs the preset Start would run with, none of them
    /// downloaded (Custom names its own).
    var presetMissingLoRAs: [PresetSummary.Lora] {
        guard preference != .custom else { return [] }
        return runningModelInfo?.presets?[preference.rawValue]?.missing ?? []
    }

    /// The preset needs a LoRA that is not downloaded: asked for, in
    /// Settings › Capabilities, on its family.
    func askForPresetLoRA(_ missing: [PresetSummary.Lora]) {
        guard let m = runningModelInfo else { return }
        let names = missing.map(\.name).joined(separator: String(localized: " or "))
        if ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] != nil {
            FileHandle.standardError.write(Data(
                "snapshot: needs-lora model=\(m.id) preference=\(preference.rawValue) loras=\(missing.map(\.id).joined(separator: ","))\n".utf8))
            return
        }
        let alert = NSAlert()
        alert.messageText = String(localized: "\(preference.label) needs a Turbo LoRA")
        alert.informativeText = String(localized: "\(m.name) runs \(preference.label) with \(names), which is not downloaded. Download it in Settings › Capabilities, or choose another setting.")
        alert.addButton(withTitle: String(localized: "Open Capabilities"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        if alert.runModal() == .alertFirstButtonReturn {
            openCapabilities(family: m.family)
        }
    }

    /// Settings on its Capabilities page, `family` opened there.
    var settingsPageRequest: String?
    var capabilitiesFamily: String?
    /// A gated model whose download sheet is open (Capabilities).
    var gatedDownload: CapabilityTree.Member?
    /// Why a model's last download failed, by model id: shown under its
    /// row until it is tried again.
    var downloadFailures: [String: String] = [:]
    /// A token the snapshot hooks type into the gated sheet.
    @ObservationIgnored var snapshotGatedToken: String?
    func openCapabilities(family: String?) {
        capabilitiesFamily = family
        settingsPageRequest = "capabilities"
        // The app menu's Settings…, as a person would choose it.
        if let app = NSApp.mainMenu?.items.first?.submenu,
           let i = app.items.firstIndex(where: { $0.keyEquivalent == "," }) {
            app.performActionForItem(at: i)
        }
    }

    // MARK: Layers

    /// The picture on the stage when it is one of the project's -- a
    /// result, or one put there -- rather than a picture to edit or a
    /// clip: what the Layers section works on.
    var stagePicture: AssetDTO? {
        guard !adjustsBase, stage.video == nil,
              stage.clip == nil, stageOriginal != nil,
              let id = stageAssetId else { return nil }
        return assets.first { $0.id == id && $0.kind == "image" }
    }

    /// The picture on the stage has a layer stack.
    var stageLayered: Bool { !(stagePicture?.layers ?? []).isEmpty }

    /// The picture on the stage is drawn by the core: it has a stack, or
    /// is shown on a canvas of its own size (Canvas Size).
    var stageComposed: Bool { stageLayered || stagePicture?.canvas != nil }

    /// What the Layers section works on: the picture on the stage, or
    /// the finished clip -- whose layers are pictures, clips and markup
    /// over it, each with its own keyed tracks.
    var stageStack: AssetDTO? {
        if let pic = stagePicture { return pic }
        guard clipOnStage, let c = currentClip,
              c.kind == "video" || c.isTimeline else {
            return nil
        }
        return c
    }

    /// The clip on the stage is drawn by the core: it has layers, a canvas
    /// of its own size or a timeline of its own length.
    var clipComposed: Bool {
        guard clipOnStage, let c = currentClip else { return false }
        return !(c.layers ?? []).isEmpty || c.canvas != nil
            || c.timeline != nil
    }

    /// The selected layer, if the stack has it; else the bottom one.
    var activeLayer: String {
        guard let stack = stageStack?.layerStack else { return "" }
        if stack.contains(where: { $0.id == selectedLayer }) {
            return selectedLayer
        }
        // Layer 0, or -- gone from the project's -- the bottom one.
        return stack.contains { $0.id.isEmpty } ? "" : stack.first?.id ?? ""
    }

    /// The stack on the stage keeps a frame of its own (the project's
    /// composition, core StackCanvas::framed): layer 0 lies on it as every
    /// layer does.
    var stageFramed: Bool { stageStack?.canvas?.framed == true }

    /// The layer is PLACED on the stack's frame by its crop -- every layer
    /// of the project's, any but the bottom one of a picture's own stack
    /// (whose crop is the frame).
    func isPlacedLayer(_ id: String) -> Bool { !id.isEmpty || stageFramed }

    /// The layer at the bottom of the stage's stack: nothing beneath it to
    /// mask.
    func isBottomLayer(_ id: String) -> Bool {
        stageStack?.layerStack.first?.id == id
    }

    var stageHasHiddenLayer: Bool {
        stagePicture?.layerStack.contains { !$0.visible } == true
    }

    /// A layer chosen: the panels show and change its values -- on a
    /// clip, its tracks.
    func selectLayer(_ id: String) {
        // A snapshot run: how long a posted click took to get here.
        if AppDelegate.postedClickAt > 0 {
            print("snapshot: select-layer \(id.isEmpty ? "0" : id) after-click="
                  + "\(Int((CACurrentMediaTime() - AppDelegate.postedClickAt) * 1000))ms")
            AppDelegate.postedClickAt = 0
        }
        if pagedOnStage, let pic = stagePicture {
            // A still with pages: its tracks, keyed by page.
            persistClipTracks()
            selectedLayer = id
            selectedLayers = [id]
            loadClipTracks(pic, layer: activeLayer)
            bypassed = []
            recomposite()
            return
        }
        if clipOnStage, let clip = stageStack {
            persistClipTracks()
            persistTrim()
            selectedLayer = id
            selectedLayers = [id]
            loadClipTracks(clip, layer: activeLayer)
            loadTrim(clip)
            bypassed = []
            refreshStackPlan()
            return
        }
        guard let pic = stagePicture else { return }
        persistAdjustments()
        persistCrop()
        selectedLayer = id
        selectedLayers = [id]
        adjustments = pic.adjustments(layer: id)
        crop = pic.crop(layer: id)
        bypassed = []
    }

    /// A clip layer's tracks into the panels: as recorded, or one key at
    /// the first frame.
    func loadClipTracks(_ clip: AssetDTO, layer: String) {
        let a = clip.adjustKeys(layer: layer)
        clipAdjustKeys = a.isEmpty ? Keyframes(start: ImageAdjustments()) : a
        var cc = clip.cropKeys(layer: layer)
        if cc.place.isEmpty { cc.place = Keyframes(start: CropSpec()) }
        if cc.turn.isEmpty { cc.turn = Keyframes(start: Turn()) }
        clipCropKeys = cc
        refreshClipValues()
    }

    /// A new, empty layer above the selected one, selected -- the next
    /// generation is made on it.
    func addLayer() {
        guard let core, let projectId, let pic = stageStack else { return }
        persistAdjustments()
        persistCrop()
        if keyedStage { persistClipTracks() }
        let target = composedTarget(pic.id)
        var extra: [String: Any] = ["above": activeLayer]
        // A still with pages: on the page shown, alone.
        if pagedOnStage { extra["page"] = stagePage }
        let r = core.layerOp(project: projectId, asset: target, "add",
                             extra: extra)
        guard r.ok, let id = r["layer"] as? String else {
            note("error", r.message)
            return
        }
        layersChanged()
        selectLayer(id)
    }

    /// Up (+1) or down (-1) the stack.
    func moveLayer(_ id: String, by: Int) {
        layerOp("move", id, ["by": by])
    }

    /// A layer DUPLICATED right above it -- a markup's drawing and
    /// objects copied (drawn on apart), anything else shown by both, its
    /// looks and time with it -- "Layer 2 Copy", selected.
    func duplicateLayer(_ id: String) {
        guard let core, let projectId, let pic = stageStack,
              let l = pic.layerStack.first(where: { $0.id == id }) else {
            return
        }
        // What the panels hold of it, recorded first: the copy takes it.
        flushPanels()
        commitSelection()
        let title = l.title
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(pic.id), "duplicate",
                             layer: id,
                             extra: ["name": String(localized: "\(title) Copy")])
        guard r.ok, let made = r["layer"] as? String else {
            flash(r.message)
            return
        }
        layersChanged()
        selectLayer(made)
    }

    func setLayerVisible(_ id: String, _ visible: Bool) {
        layerOp(visible ? "show" : "hide", id)
    }

    func renameLayer(_ id: String, _ name: String) {
        layerOp("rename", id, ["name": name])
    }

    /// Any layer but the last of a picture's -- layer 0 too, with a layer
    /// above it (core Controller::remove_layer) -- and any but a clip's
    /// own.
    func canRemoveLayer(_ id: String) -> Bool {
        guard let stack = stageStack,
              stack.layerStack.contains(where: { $0.id == id }) else {
            return false
        }
        if stack.isComposition && (stageFramed || stack.kind == "audio") {
            return stack.layerStack.count > 1
        }
        if !id.isEmpty { return true }
        return stack.kind == "image" && stack.layerStack.count > 1
    }

    func removeLayer(_ id: String) {
        guard canRemoveLayer(id) else { return }
        // A picture's own layer 0 goes: the layer above takes its place --
        // and keeps the name it showed ("Layer 2"). (The project's goes as
        // any layer does.)
        if id.isEmpty, !stageFramed, let stack = stageStack?.layerStack,
           stack.count > 1, stack[1].name.isEmpty {
            renameLayer(stack[1].id, stack[1].title)
        }
        if selectedLayer == id { selectedLayer = "" }
        layerOp("remove", id)
        if keyedStage, let clip = stageStack {
            loadClipTracks(clip, layer: activeLayer)
            return
        }
        if let pic = stagePicture {
            adjustments = pic.adjustments(layer: activeLayer)
            crop = pic.crop(layer: activeLayer)
        }
        // Back to a single picture: it is its own image again.
        if !stageComposed, let pic = stagePicture,
           let url = pic.url, let img = Self.loadImage(url) {
            stage.a = img
        }
    }

    private func layerOp(_ op: String, _ id: String,
                         _ extra: [String: Any] = [:]) {
        guard let core, let projectId, let pic = stageStack else { return }
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(pic.id), op,
                             layer: id, extra: extra)
        if !r.ok { flash(r.message) }
        layersChanged()
    }

    func layersChanged() {
        reloadAssets()
        if clipOnStage { refreshStackPlan() } else { recomposite() }
    }

    // MARK: Layer folders (DESIGN §6a)

    /// The stack's folders.
    var stageLayerFolders: [LayerFolderDTO] { stageStack?.layerFolders ?? [] }

    func layerFolderName(_ id: String) -> String {
        stageLayerFolders.first { $0.id == id }?.name ?? ""
    }

    /// The selected layers -- several ⌘-clicked, else the one selected --
    /// in a new folder, where the topmost of them is.
    func groupSelectedLayers() {
        guard let core, let projectId, let stack = stageStack else { return }
        let ids = selectedLayers.count > 1 ? Array(selectedLayers)
                                           : [activeLayer]
        let n = String(stageLayerFolders.count + 1)
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(stack.id), "group",
                             extra: ["layers": ids,
                                     "name": String(localized: "Folder \(n)")])
        if !r.ok { flash(r.message) }
        layersChanged()
    }

    func ungroupLayers(_ folder: String) {
        layerOp("ungroup", "", ["folder": folder])
    }

    func renameLayerFolder(_ folder: String, _ name: String) {
        layerOp("folder-rename", "", ["folder": folder, "name": name])
    }

    /// Every layer of the folder shown or hidden.
    func setFolderVisible(_ folder: String, _ visible: Bool) {
        layerOp(visible ? "folder-show" : "folder-hide", "",
                ["folder": folder])
    }

    /// Layers moved to lie right above `above` (nil: at the bottom), in
    /// `folder` ("": in none) -- a drag in the Layers section or the
    /// timeline's headers.
    func placeLayers(_ ids: [String], above: String?, folder: String) {
        var extra: [String: Any] = ["layers": ids, "folder": folder]
        if let above { extra["above"] = above }
        layerOp("place", "", extra)
    }

    /// A folder folded shut (its layers out of sight), or open.
    func isFolderFolded(_ folder: String) -> Bool {
        foldedLayerFolders.contains("\(stageStack?.id ?? "")/\(folder)")
    }

    func toggleFolderFolded(_ folder: String) {
        let k = "\(stageStack?.id ?? "")/\(folder)"
        withAnimation(Self.motion) {
            if foldedLayerFolders.contains(k) {
                foldedLayerFolders.remove(k)
            } else {
                foldedLayerFolders.insert(k)
            }
        }
    }

    /// What a drag of a layer's row carries: this, and its id (a clip's
    /// layer has no file to carry); a still's carries its file, matched
    /// back to it here.
    static let layerPrefix = "valtz-layer:"
    @ObservationIgnored var layerDrag: (id: String, url: URL?)?

    /// The layer a drop carries: by its marker, or its file.
    func draggedLayer(text: String? = nil, url: URL? = nil) -> String? {
        if let t = text, t.hasPrefix(Self.layerPrefix) {
            return String(t.dropFirst(Self.layerPrefix.count))
        }
        if let url, let d = layerDrag, d.url == url { return d.id }
        return nil
    }

    /// A picture or a clip dropped on a layer: what it shows from now on
    /// -- imported first if need be.
    /// An asset of the list dropped on a layer's row: what it shows.
    func setLayerSource(_ id: String, asset: String) {
        guard let core, let projectId, let stack = stageStack,
              asset != stack.id else { return }
        let r = core.layerOp(project: projectId,
                             asset: composedTarget(stack.id), "source",
                             layer: id, extra: ["source": asset])
        if !r.ok { flash(r.message) }
        layersChanged()
    }

    func setLayerSource(_ id: String, from url: URL) {
        guard let core, let projectId, let stack = stageStack else { return }
        importFiles([url])
        let stackId = composedTarget(stack.id)
        Task { @MainActor [weak self] in
            for _ in 0..<600 {
                guard let self else { return }
                if let a = self.asset(forFile: url) {
                    guard a.id != stackId else { return }
                    let r = core.layerOp(project: projectId, asset: stackId,
                                         "source", layer: id,
                                         extra: ["source": a.id])
                    if !r.ok { self.flash(r.message) }
                    self.layersChanged()
                    return
                }
                try? await Task.sleep(for: .milliseconds(100))
            }
        }
    }

    // MARK: Pages (a still's; DESIGN §6a)

    /// The still on the stage has PAGES: it is drawn a page at a time, and
    /// its layers' looks are keyed by page as a clip's are by frame.
    var pagedOnStage: Bool { stagePicture?.isPaged == true }
    var stagePages: Int { stagePicture?.pageCount ?? 1 }

    /// The panels KEY what they change: a clip's tracks at its frame, a
    /// still's with pages at its page.
    var keyedStage: Bool { clipOnStage || pagedOnStage }

    /// Where a key goes: the frame on screen -- or the page -- counted
    /// from the selected layer's own start, as its keys are.
    var keyFrame: Int {
        let at = clipOnStage ? videoFrame : stagePage
        guard let s = stageStack, s.isComposition else { return at }
        return at - s.time(of: activeLayer).offset
    }

    /// The stage at a page of the still (from 0): what the panels show
    /// and key there.
    func goToPage(_ n: Int) {
        guard pagedOnStage else { return }
        let to = min(max(0, n), stagePages - 1)
        guard to != stagePage else { return }
        persistClipTracks()
        stagePage = to
        refreshClipValues()
        recomposite()
    }

    /// A page after the one on the stage, which the stage then shows: what
    /// runs across it continues there (core Controller::add_page).
    func addPage() {
        guard let core, let projectId, let pic = stagePicture else { return }
        flushPanels()
        let was = pic.isPaged
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             "page-add", extra: ["after": stagePage])
        guard r.ok, let made = r["page"] as? Int else {
            flash(r.message)
            return
        }
        pagesChanged(to: made, was: was)
    }

    /// The page on the stage gone, with the layers only it had.
    func removePage() {
        guard let core, let projectId, let pic = stagePicture,
              pic.isPaged else { return }
        flushPanels()
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             "page-remove", extra: ["page": stagePage])
        guard r.ok else {
            flash(r.message)
            return
        }
        pagesChanged(to: min(stagePage, stagePages - 2), was: true)
    }

    /// The stage at `page` after the pages changed: the panels' tracks
    /// read again (the core moved their keys with their pages) -- or, back
    /// to one page, its values.
    private func pagesChanged(to page: Int, was: Bool) {
        reloadAssets()
        guard let pic = stagePicture else { return }
        stagePage = max(0, min(page, pic.pageCount - 1))
        if !pic.layerStack.contains(where: { $0.id == selectedLayer }) {
            selectedLayer = pic.layerStack.last?.id ?? ""
            selectedLayers = [selectedLayer]
        }
        if pic.isPaged {
            loadClipTracks(pic, layer: activeLayer)
        } else if was {
            adjustments = pic.adjustments(layer: activeLayer)
            crop = pic.crop(layer: activeLayer)
        }
        recomposite()
    }

    /// Is the layer on the page the stage shows?
    func isLayerOnPage(_ l: LayerDTO) -> Bool {
        guard let pic = stagePicture else { return true }
        return pic.isOnPage(l, stagePage)
    }

    /// The pages a layer is on, as its row says it: "Page 2", "Pages
    /// 2–4"; nil with no pages.
    func pageSpanText(_ l: LayerDTO) -> String? {
        guard let pic = stagePicture, pic.isPaged else { return nil }
        guard let span = pic.pageSpan(of: l) else {
            return String(localized: "On no page")
        }
        let a = String(span.lowerBound + 1), b = String(span.upperBound + 1)
        return span.count == 1 ? String(localized: "Page \(a)")
            : String(localized: "Pages \(a)–\(b)")
    }

    /// Which pages a layer is on, from the page on the stage.
    enum LayerPages { case thisPage, fromHere, every }

    func setLayerPages(_ id: String, _ which: LayerPages) {
        guard let core, let projectId, let pic = stagePicture,
              pic.isPaged else { return }
        persistClipTracks()
        let (first, count): (Int, Int) = switch which {
        case .thisPage: (stagePage, 1)
        case .fromHere: (stagePage, 0)
        case .every: (0, 0)
        }
        let r = core.layerOp(project: projectId, asset: composedTarget(pic.id),
                             "pages", layer: id,
                             extra: ["first": first, "count": count])
        if !r.ok { flash(r.message) }
        layersChanged()
        if id == activeLayer, let p = stagePicture {
            loadClipTracks(p, layer: activeLayer)
        }
    }

    // MARK: A clip's stack, played

    /// The plan the player draws the clip's stack with: the core's
    /// (media::StackRenderer), made again when a layer, the canvas, the
    /// timeline or the selected layer's tracks change.
    var stackPlayback: StackPlayback?
    @ObservationIgnored private var stackPlanning = false
    @ObservationIgnored private var stackPlanAgain = false
    /// The source frame the player goes to once the plan of the marks
    /// just set lands (holdPlayer(onSource:)).
    @ObservationIgnored private var trimSeekSource: Int?

    /// A new plan for the clip on the stage -- with the selected layer's
    /// tracks as the panels hold them, less what a held Bypass leaves out
    /// -- or none when the clip is just itself. One at a time; changes
    /// meanwhile make one more, the newest. The old plan is let go a
    /// moment later, once frames in flight are drawn.
    func refreshStackPlan() {
        guard let core, let projectId, clipComposed,
              let clip = currentClip else {
            dropStackPlan()
            return
        }
        if stackPlanning {
            stackPlanAgain = true
            return
        }
        stackPlanning = true
        let rate = stageFrameRate
        let look = stageClipLook
        let layer = activeLayer
        let id = clip.id
        let firstPoster = stackPlayback == nil
        // The markup objects being changed are drawn live, over it.
        let hidden = markupOpen && markup.live ? markup.selectedIds : []
        let painted = markup.painted
        Task { @MainActor [weak self] in
            let made = await Task.detached {
                let live = look.map {
                    (layer: layer,
                     adjust: $0.adjust.json(rate: rate) { $0.json } as Any,
                     crop: $0.crop.json(rate: rate) as Any)
                }
                return core.stackPlan(project: projectId, asset: id,
                                      live: live, hidden: hidden)
            }.value
            guard let self else {
                if let made { core.releaseStackPlan(made.plan) }
                return
            }
            self.stackPlanning = false
            if let made, self.currentClip?.id == id, self.clipComposed {
                let old = self.stackPlayback
                let reshaped = old?.shape != made.shape
                self.stackPlayback = made
                if let old { self.releaseLater(old.plan) }
                if firstPoster || reshaped { self.stackPoster(made, id) }
            } else if let made {
                core.releaseStackPlan(made.plan)
            }
            if self.stackPlanAgain {
                self.stackPlanAgain = false
                self.refreshStackPlan()
            } else {
                self.placeOnTrimSource()
                // A stroke painted into it is in the frames now.
                if made != nil { self.markupComposited(painted: painted) }
            }
        }
    }

    /// The stack's first frame, as the clip's poster (the layout, a drag)
    /// -- a composition of sound alone, its mix's waveform.
    private func stackPoster(_ s: StackPlayback, _ id: String) {
        guard let core else { return }
        if s.soundOnly {
            guard let projectId else { return }
            Task { @MainActor [weak self] in
                let mix = await Task.detached {
                    core.renderedPath(project: projectId, asset: id)
                }.value
                guard let mix,
                      let img = await AudioWaveform.image(mix, width: 1600,
                                                          height: 400),
                      let self, self.currentClip?.id == id,
                      self.stackPlayback?.plan == s.plan else { return }
                self.stage.a = img
            }
            return
        }
        Task { @MainActor [weak self] in
            // Nothing on it yet (a timeline just set up): its frame, black,
            // so the stage takes the timeline's shape -- it was square.
            let img = await Task.detached {
                core.stackStill(plan: s.plan, frame: 0)
            }.value ?? Self.blankPoster(width: s.width, height: s.height)
            guard let self, let img, self.currentClip?.id == id,
                  self.stackPlayback?.plan == s.plan else { return }
            self.stage.a = img
        }
    }

    /// A black picture of a frame's shape (its longer edge 256 pixels):
    /// what an empty timeline shows, and the stage's aspect.
    nonisolated static func blankPoster(width: Int, height: Int) -> CGImage? {
        guard width > 0, height > 0 else { return nil }
        let k = 256 / Double(max(width, height))
        let w = max(1, Int((Double(width) * k).rounded()))
        let h = max(1, Int((Double(height) * k).rounded()))
        guard let ctx = CGContext(
            data: nil, width: w, height: h, bitsPerComponent: 8,
            bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else {
            return nil
        }
        ctx.setFillColor(CGColor(gray: 0, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        return ctx.makeImage()
    }

    private func releaseLater(_ plan: UInt64) {
        guard let core else { return }
        Task {
            try? await Task.sleep(for: .seconds(2))
            core.releaseStackPlan(plan)
        }
    }

    /// The clip is itself again (or gone from the stage).
    private func dropStackPlan() {
        guard let old = stackPlayback else { return }
        stackPlayback = nil
        releaseLater(old.plan)
        // Still on the stage: its own first frame is its poster again.
        guard clipOnStage, let url = stage.video else { return }
        Task { [weak self] in
            guard let poster = await Self.posterFrame(url) else { return }
            guard let self, self.stage.video == url,
                  self.stackPlayback == nil else { return }
            self.stage.a = poster
        }
    }

    /// The stage drawn again from the stack -- by the core, as a file is
    /// made of it, with the selected layer's values as the panels hold
    /// them (less what a held Bypass leaves out). One draw at a time;
    /// changes meanwhile make one more, the newest.
    func recomposite() {
        guard let core, let projectId, let pic = stagePicture,
              stageComposed else { return }
        if compositing {
            compositeAgain = true
            return
        }
        compositing = true
        let look = (layer: activeLayer,
                    adjust: bypassed.contains(.adjust)
                        ? ImageAdjustments() : adjustments,
                    crop: shownCrop)
        let id = pic.id
        // The markup objects being changed are drawn live, over it.
        let hidden = markupOpen && markup.live ? markup.selectedIds : []
        let page = pic.isPaged ? stagePage : 0
        let painted = markup.painted
        Task { @MainActor [weak self] in
            // Drawn by the core on the GPU into a surface the canvas shows
            // as it is; read in place where pixels are wanted.
            let made: GPUPicture? = await Task.detached {
                guard let s = core.flattenSurface(
                          project: projectId, asset: id, look: look,
                          hidden: hidden, page: page),
                      let img = GPUPicture.cgImage(
                          over: s,
                          colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)
                else { return nil }
                return GPUPicture(image: img, surface: s)
            }.value
            guard let self else { return }
            self.compositing = false
            if let made, self.stagePicture?.id == id, self.stageComposed {
                self.showCurrent(made.image, surface: made.surface)
                self.markupComposited(painted: painted)
            }
            if self.compositeAgain {
                self.compositeAgain = false
                self.recomposite()
            }
        }
    }

    /// The stack flattened to a file -- what a drag from the stage
    /// carries: the whole picture, as the stage shows it.
    func flattenedFile() -> URL? {
        guard let core, let projectId, let pic = stagePicture else {
            return nil
        }
        persistAdjustments()
        persistCrop()
        try? FileManager.default.createDirectory(
            at: Self.shareRoot, withIntermediateDirectories: true)
        let out = Self.shareRoot.appendingPathComponent(exportFileName)
        return core.flatten(project: projectId, asset: pic.id,
                            to: out.path,
                            page: pic.isPaged ? stagePage : 0).ok ? out : nil
    }

    /// One layer alone, as it shows (its look, on the canvas) -- what a
    /// layer dragged into the prompt carries, to edit by itself.
    func layerFile(_ id: String) -> URL? {
        guard let core, let projectId, let pic = stagePicture,
              let layer = pic.layerStack.first(where: { $0.id == id }),
              !layer.isEmpty else { return nil }
        persistAdjustments()
        persistCrop()
        try? FileManager.default.createDirectory(
            at: Self.shareRoot, withIntermediateDirectories: true)
        let base = (exportFileName as NSString).deletingPathExtension
        let out = Self.shareRoot.appendingPathComponent(
            "\(base) \(layer.title).png")
        return core.flatten(project: projectId, asset: pic.id, to: out.path,
                            only: id,
                            page: pic.isPaged ? stagePage : 0).ok ? out : nil
    }

    // MARK: Favor's Custom

    /// Favor chosen: a preset, or Custom -- which starts from the preset
    /// it replaces the first time a family has none of its own, and opens
    /// its panel.
    func choosePreference(_ p: Preference) {
        guard p != preference else {
            if p == .custom { toggleTuning() }
            return
        }
        if p == .custom {
            customBase = preference
            preference = .custom
            refreshTuning()
            showsTuning = true
        } else {
            preference = p
            showsTuning = false
        }
    }

    /// Custom's panel from what opens it -- Favor's Custom again, or its
    /// summary beside it: shut if it was open as the click began, else
    /// opened.
    func toggleTuning() {
        let wasOpen = tuningOpenAtPress || showsTuning
        tuningOpenAtPress = false
        if wasOpen {
            showsTuning = false
        } else {
            refreshTuning()
            showsTuning = true
        }
    }

    /// Notes, at each press, whether Custom's panel was open (installed
    /// once, by the card that shows Favor).
    func watchTuningPresses() {
        guard tuningPressWatch == nil else { return }
        tuningPressWatch = NSEvent.addLocalMonitorForEvents(
            matching: .leftMouseDown
        ) { [weak self] event in
            MainActor.assumeIsolated {
                if let self { self.tuningOpenAtPress = self.showsTuning }
            }
            return event
        }
    }

    /// The edit is a picture's (the options of an edit, for its model).
    private var tuningIsEdit: Bool { activeModality == .image && willEdit }

    /// The options for the model Start runs, settled with the family's
    /// Custom values (its first time: the preset's, kept as its own; an
    /// older save's Turbo selector folded into the LoRA list).
    func refreshTuning() {
        guard preference == .custom, let core, !runningModel.isEmpty,
              let family = runningModelInfo?.family else {
            tuningInfo = nil
            return
        }
        let info = core.tuning(model: runningModel,
                               preference: customBase.rawValue,
                               edit: tuningIsEdit,
                               overrides: customTunings[family]?.json ?? [:])
        tuningInfo = info
        if let info, customTunings[family] == nil
            || customTunings[family]?.choices.isEmpty == false {
            customTunings[family] = info.tuning
            saveCustomTunings()
        }
    }

    /// One option changed in the panel. The core settles it with the
    /// rest (its rules: HyperFlow takes a LoRA slot, not the Turbo LoRA's,
    /// and its own steps; VDN replaces Sol-Attn), and that is what is
    /// kept. An option that decides the steps lets them follow it.
    func setTuning(_ key: String, flag: Bool? = nil, number: Double? = nil) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        if let flag { t.flags[key] = flag }
        if let number { t.numbers[key] = number }
        let o = info.options.first { $0.key == key }
        settleTuning(t, stepsFollow: o?.fixedSteps != nil)
    }

    /// Custom's values settled by the core, kept as the family's. With
    /// `stepsFollow` the steps are left to the preset's count for what
    /// changed (an adapter's own).
    private func settleTuning(_ t: Tuning, stepsFollow: Bool = false) {
        guard let core else { return }
        var json = t.json
        if stepsFollow { json["steps"] = nil }
        guard let next = core.tuning(model: runningModel,
                                     preference: customBase.rawValue,
                                     edit: tuningIsEdit,
                                     overrides: json) else { return }
        tuningInfo = next
        customTunings[next.family] = next.tuning
        saveCustomTunings()
    }

    // LoRAs and checkpoints (Custom's list; core models/tuning.h).

    /// Weights dropped on the panel, .safetensors files or folders: each
    /// filed by what it is (the core reads it) -- a LoRA in the LoRA list,
    /// a DiT or VAE checkpoint in its part's.
    @discardableResult
    func addWeights(_ urls: [URL]) -> Bool {
        guard let core else { return false }
        var loras: [URL] = []
        var parts: [String: [URL]] = [:]
        for url in urls {
            switch core.checkpointKind(url) {
            case "lora": loras.append(url)
            case "dit": parts["dits", default: []].append(url)
            case "vae": parts["vaes", default: []].append(url)
            default: break
            }
        }
        var added = false
        if !loras.isEmpty { added = addLoRAs(loras) || added }
        if !parts.isEmpty, let info = tuningInfo {
            var t = info.tuning
            for (key, urls) in parts {
                var cs = t.checkpoints[key] ?? []
                for url in urls {
                    let path = url.standardizedFileURL.path
                    guard !cs.contains(where: { $0.path == path }) else {
                        continue
                    }
                    // On, when no other is: dropping one is choosing it.
                    cs.append(TuningCheckpoint(
                        path: path, on: !cs.contains { $0.on }))
                    added = true
                }
                t.checkpoints[key] = cs
            }
            settleTuning(t)
        }
        if !added && !urls.isEmpty {
            flash(String(localized: "Not weights Valtz can use here"))
        }
        return added
    }

    /// A DiT or VAE checkpoint turned on (the others off: one replaces
    /// the model's part) or off (the model's own again).
    func setCheckpoint(_ list: String, _ path: String, on: Bool) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        var cs = t.checkpoints[list] ?? []
        for i in cs.indices {
            cs[i].on = cs[i].path == path ? on : (on ? false : cs[i].on)
        }
        t.checkpoints[list] = cs
        settleTuning(t)
    }

    func removeCheckpoint(_ list: String, _ path: String) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        t.checkpoints[list]?.removeAll { $0.path == path }
        settleTuning(t)
    }

    /// LoRAs dropped on the panel, files or folders (the core loads a
    /// folder's weights) not in the list yet: on while a slot is free.
    @discardableResult
    func addLoRAs(_ urls: [URL]) -> Bool {
        guard let info = tuningInfo else { return false }
        var t = info.tuning
        var added = false
        for url in urls {
            let path = url.standardizedFileURL.path
            guard !t.loras.contains(where: { $0.path == path }) else { continue }
            let on = t.loras.filter(\.on).count < loraSlots(t, info)
            t.loras.append(TuningLoRA(path: path, scale: 1, on: on))
            added = true
        }
        if added { settleTuning(t) }
        return added
    }

    /// How many LoRAs run at once: vpipe's two slots, less HyperFlow's or
    /// TaoMate's adapter.
    private func loraSlots(_ t: Tuning, _ info: TuningInfo) -> Int {
        (info.loraOption?.slots ?? 2)
            - (t.flags["hyperflow"] == true || t.flags["taomate"] == true
                ? 1 : 0)
    }

    /// One LoRA changed: its strength, or on. Turned on with the slots
    /// full, it takes one of another's -- a LoRA of one's own before the
    /// Turbo LoRA. The Turbo LoRA turned on or off takes the steps its
    /// preset runs with it, or without.
    func setLoRA(_ path: String, scale: Double? = nil, on: Bool? = nil) {
        guard let info = tuningInfo,
              let i = info.tuning.loras.firstIndex(where: { $0.path == path })
        else { return }
        var t = info.tuning
        if let scale { t.loras[i].scale = scale }
        if let on {
            let others = t.loras.indices.filter { $0 != i && t.loras[$0].on }
            if on, others.count >= loraSlots(t, info) {
                let own = others.last { t.loras[$0].path != info.turboLoRA }
                if let j = own ?? others.last { t.loras[j].on = false }
            }
            t.loras[i].on = on
        }
        settleTuning(t, stepsFollow: on != nil && path == info.turboLoRA)
    }

    func removeLoRA(_ path: String) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        t.loras.removeAll { $0.path == path }
        settleTuning(t, stepsFollow: path == info.turboLoRA)
    }

    /// Custom back to a preset's values, for this family: its LoRA list
    /// is the preset's (the Turbo LoRA, on, when it runs one), and the
    /// weights dropped before stay listed, off.
    func resetTuning(to p: Preference) {
        guard let core, let family = runningModelInfo?.family else { return }
        let before = tuningInfo?.tuning
        customBase = p
        guard let preset = core.tuning(model: runningModel,
                                       preference: p.rawValue,
                                       edit: tuningIsEdit,
                                       overrides: [:]) else { return }
        var t = preset.tuning
        for var l in before?.loras ?? []
            where !t.loras.contains(where: { $0.path == l.path }) {
            l.on = false
            t.loras.append(l)
        }
        for (key, cs) in before?.checkpoints ?? [:] {
            t.checkpoints[key] = cs.map { var c = $0; c.on = false; return c }
        }
        let info = core.tuning(model: runningModel, preference: p.rawValue,
                               edit: tuningIsEdit, overrides: t.json)
            ?? preset
        tuningInfo = info
        customTunings[family] = info.tuning
        saveCustomTunings()
    }

    /// What a request says: the preset Custom started from, with Custom's
    /// values; or the preset.
    private var sentPreference: Preference {
        preference == .custom ? customBase : preference
    }
    private var sentTuning: [String: Any]? {
        guard preference == .custom,
              let family = runningModelInfo?.family,
              let t = customTunings[family] else { return nil }
        return t.json
    }

    /// "8 steps · HyperFlow · Sol · int8": Custom's values, briefly.
    var customSummary: String {
        guard let info = tuningInfo else { return Preference.custom.hint }
        let t = info.tuning
        var parts: [String] = []
        if let n = t.numbers["steps"] {
            let steps = String(Int(n.rounded()))
            parts.append(String(localized: "\(steps) steps"))
        }
        if t.loras.contains(where: { $0.on && $0.path == info.turboLoRA }) {
            parts.append(String(localized: "Turbo"))
        }
        let names: [(String, String)] = [
            ("hyperflow", String(localized: "HyperFlow")),
            ("taomate", String(localized: "TaoMate")),
            ("taomate_lora", String(localized: "LoRA only")),
            ("vdn", String(localized: "VDN")),
            ("sol_attn", String(localized: "Sol")),
            ("sage_attn", String(localized: "Sage")),
            ("i8_gemm", String(localized: "int8")),
            ("ane_ffn", String(localized: "ANE")),
            ("motion_cache", String(localized: "MotionCache")),
        ]
        for (k, name) in names where t.flags[k] == true { parts.append(name) }
        for l in t.loras where l.on && l.path != info.turboLoRA {
            parts.append(l.name)
        }
        for key in ["dits", "vaes"] {
            if let c = t.checkpoints[key]?.first(where: { $0.on }) {
                parts.append(c.name)
            }
        }
        return parts.joined(separator: " · ")
    }

    private static let customTuningsKey = "favor.custom"

    /// A scripted snapshot run neither reads nor writes them: it would
    /// start from (and leave behind) values of a session that is not its.
    private static var keepsCustomTunings: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
    }

    private static func loadCustomTunings() -> [String: Tuning] {
        guard keepsCustomTunings,
              let d = UserDefaults.standard.data(forKey: customTuningsKey),
              let t = try? JSONDecoder().decode([String: Tuning].self,
                                                from: d) else { return [:] }
        return t
    }

    private func saveCustomTunings() {
        guard Self.keepsCustomTunings else { return }
        if let d = try? JSONEncoder().encode(customTunings) {
            UserDefaults.standard.set(d, forKey: Self.customTuningsKey)
        }
    }

    /// A composer tray opened (or none): Adjust, Crop and Trim work on the
    /// stage, not the words -- the prompt box shrinks to its three rows,
    /// and grows back as it was when the tray goes, unless its own toggle
    /// was used meanwhile. Run from the view (onChange), not a didSet.
    func panelChanged(to p: ComposerPanel?) {
        let stageWork = p == .adjust || p == .crop || p == .trim
        // Trim's ← and → step the clip: the prompt gives up the keyboard
        // (its caret took them; a click in it gets them back).
        if p == .trim, let w = editorWindow, w.firstResponder is NSText {
            w.makeFirstResponder(nil)
        }
        if stageWork {
            if !promptCompact {
                compactForPanel = true
                withAnimation(Self.motion) { promptCompact = true }
            }
        } else if compactForPanel {
            compactForPanel = false
            if promptCompact {
                withAnimation(Self.motion) { promptCompact = false }
            }
        }
    }

    /// The prompt box's own toggle (its corner button): the person's
    /// choice, which a tray closing leaves as it is.
    func togglePromptCompact() {
        compactForPanel = false
        withAnimation(Self.motion) { promptCompact.toggle() }
    }

    /// What the empty prompt box says: how the model Start runs reads a
    /// prompt -- a song's style and lyrics, speech's fields and words, a
    /// clip's action and sound, a picture's description -- by what that
    /// model does, not only the tab.
    var promptHint: String {
        let caps = catalog.first { $0.id == runningModel }?.capabilities
            ?? []
        switch activeModality {
        case .audio:
            if caps.contains("text-to-speech") {
                return String(localized: "Type the words to speak — or drop in a voice to speak in")
            }
            return String(localized: "Describe the music — genre, mood, instruments, voice — then add lyrics under [Verse] and [Chorus]")
        case .video:
            if caps.contains("reference-to-video")
                && !caps.contains("text-to-video") {
                return String(localized: "Drop in pictures, clips and sounds, and name them <Picture 1>, <Video 1>, <Audio 1>")
            }
            return String(localized: "Describe the clip and its sound — or drop in a picture to open on")
        case .image:
            if caps.contains("image-edit") {
                return String(localized: "Describe the picture — or drop in pictures and say what to change")
            }
            return String(localized: "Describe the picture — subject, setting, light, style")
        }
    }

    /// A model's name, from the catalog.
    func modelName(_ id: String) -> String {
        catalog.first { $0.id == id }?.name ?? id
    }

    /// Video and audio are listed (Auto's order is settled), but nothing
    /// runs for them until a model that makes them is installed here.
    var modalityRuns: Bool {
        activeModality == .image || !autoPick("generate").isEmpty
    }

    func capability(_ id: String) -> CapabilityStatus? {
        capabilities.first { $0.capability == id }
    }

    var installedImageModels: [ModelOption] {
        capability("text-to-image")?.options.filter {
            $0.state == "installed" && $0.fits
        } ?? []
    }

    var imageModelName: String {
        imageModel.isEmpty ? String(localized: "No model")
                           : modelName(imageModel)
    }

    var installedEditModels: [ModelOption] {
        guard capability("image-edit")?.availability == "ready" else {
            return []
        }
        return capability("image-edit")?.options.filter {
            $0.state == "installed" && $0.fits
        } ?? []
    }

    var editModelName: String {
        editModel.isEmpty ? String(localized: "No edit model")
                          : modelName(editModel)
    }

    /// Start will EDIT: a picture is attached and an edit model is
    /// installed. The result is the size set in the drawer -- the base's
    /// own unless changed (its custom size).
    var willEdit: Bool {
        activeModality == .image && !editModel.isEmpty && hasEditPictures
    }

    /// Pictures an edit would use are attached to the prompt.
    var hasEditPictures: Bool {
        promptAttachments.contains { $0.kind == "image" }
    }

    var assistantReady: Bool {
        capability("prompt-enhance")?.availability == "ready"
    }

    func download(model: String, token: String = "") {
        guard let core else { return }
        downloadFailures[model] = nil
        let r = core.downloadModel(model, token: token)
        if !r.ok {
            note("error", r.message)
            downloadFailures[model] = r.message
        }
    }

    /// The gated sheet's Download: the token goes to this download only
    /// -- through the core into the fetch, in memory -- and the sheet
    /// closes, taking it with it.
    func downloadGated(_ member: CapabilityTree.Member, token: String) {
        let t = token.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        download(model: member.model, token: t)
        gatedDownload = nil
    }

    /// A quantized variant made here from its source (Settings ›
    /// Capabilities' Quantize): a job, its row's progress.
    func quantize(model: String) {
        guard let core else { return }
        let r = core.quantizeModel(model)
        if !r.ok { flash(r.message) }
    }

    func rescanModels() {
        core?.rescanModels()
        refreshMachine()
    }

    /// A model used from a file or folder where it already is.
    func linkModel(_ id: String, to url: URL) {
        guard let core else { return }
        let r = core.linkModel(id, path: url.path)
        if !r.ok { flash(r.message) }
        refreshMachine()
    }

    func unlinkModel(_ id: String) {
        guard let core else { return }
        let r = core.unlinkModel(id)
        if !r.ok { flash(r.message) }
        refreshMachine()
    }

    /// The agentic helper chosen ("" Auto), kept by the core for later
    /// runs and valtzctl.
    func chooseHelper(_ id: String) {
        guard let core else { return }
        let r = core.chooseAssistant(id)
        if !r.ok { flash(r.message) }
        helpers = DTO.decode(HelperList.self, core.assistantsJSON())
    }

    /// What the helper drafts with: "mtp", or "dflash" at `bits`.
    func setHelperDrafter(_ kind: String, bits: Int) {
        guard let core else { return }
        let r = core.setAssistantDrafter(kind, bits: bits)
        if !r.ok { flash(r.message) }
        helpers = DTO.decode(HelperList.self, core.assistantsJSON())
    }

    /// How long the helper stays loaded after a request, in seconds.
    func setHelperKeepLoaded(_ seconds: Double) {
        guard let core else { return }
        let r = core.setAssistantKeepLoaded(seconds)
        if !r.ok { flash(r.message) }
        helpers = DTO.decode(HelperList.self, core.assistantsJSON())
    }

    /// A video summary's seconds a frame (DESIGN §4i); 0: Auto.
    func setVideoEvery(_ seconds: Double) {
        guard let core else { return }
        let r = core.setVideoEvery(seconds)
        if !r.ok { flash(r.message) }
        helpers = DTO.decode(HelperList.self, core.assistantsJSON())
    }

    // MARK: - Navigation

    func toggleNav() {
        withAnimation(Self.motion) { navOpen.toggle() }
    }

    /// The editor or the log, from the ☰ column.
    func show(_ s: AppScreen) {
        withAnimation(Self.motion) {
            screen = s
            navOpen = false
        }
    }

    // MARK: - Projects

    func openProject(path: String, createIfMissing: Bool = false) {
        guard let core else { return }
        var r = core.openProject(path: path)
        if !r.ok && createIfMissing {
            let name = URL(fileURLWithPath: path)
                .deletingPathExtension().lastPathComponent
            r = core.createProject(path: path, name: name)
        }
        guard r.ok, let id = r["project"] as? String else {
            note("error", String(localized: "Could not open \(path): \(r.message)"))
            flash(String(localized: "Could not open \(path): \(r.message)"))
            return
        }
        // The project before goes (its changes were saved, or let go, by
        // confirmClose).
        if let before = projectId, before != id {
            retireProject(before, path: projectPath, anonymous: isAnonymous)
            withAnimation(Self.motion) { resetSession() }
        }
        projectId = id
        projectPath = path
        isAnonymous = false
        refreshDocument()
        // What the open found.
        if r["recovered"] as? Bool == true {
            flash(String(localized: "Your unsaved changes were restored. Revert to Saved goes back to the saved version."))
        }
        if let missing = r["missing_extensions"] as? [[String: Any]],
           !missing.isEmpty {
            let names = missing.map {
                ($0["name"] as? String) ?? ($0["id"] as? String) ?? ""
            }.joined(separator: ", ")
            flash(String(localized: "This project uses extensions that aren't installed (\(names)): their parts are skipped, and kept for the next save."))
        }
        projectName = r["name"] as? String
            ?? URL(fileURLWithPath: path).deletingPathExtension()
                .lastPathComponent
        restoreWindowSize()
        reloadAssets()
        // Its work, as it was left: the project's composition.
        if let view = projectViews.first { putOnStage(view) }
    }

    /// The PROJECT's composition (DESIGN §6a; core project_composition) --
    /// what the stage shows of the work: one, or none until the first
    /// result or a setup.
    var projectViews: [AssetDTO] {
        assets.filter(\.isProject)
    }

    func isProjectView(_ id: String) -> Bool {
        assets.contains { $0.id == id && $0.isProject }
    }

    /// The project set up from the inspector (Information › Project): a
    /// still or a timeline, `width` x `height` (0 x 0: sound alone) --
    /// the project's composition from now on; the old one stays an asset.
    func newProjectComposition(still: Bool, width: Int, height: Int) {
        guard let core, let projectId else { return }
        let r = core.assetOp(project: projectId, "new-composition", [
            "still": still, "width": width, "height": height,
            "as_project": true, "name": projectName ?? "",
        ])
        guard r.ok, let id = r["asset"] as? String else {
            flash(r.message)
            return
        }
        reloadAssets()
        if let a = assets.first(where: { $0.id == id }) { putOnStage(a) }
    }

    /// That composition the project's.
    func useAsProject(_ a: AssetDTO) {
        guard let core, let projectId, a.isComposition else { return }
        // Resized before (its canvas then made its frame): its layers'
        // places may move, the panels' values written first.
        flushPanels()
        let r = core.assetOp(project: projectId, "project", ["asset": a.id])
        if !r.ok { flash(r.message) }
        reloadAssets()
        if let doc = assets.first(where: { $0.id == a.id }) {
            putOnStage(doc)
        }
    }

    /// A FLAT asset of `a` as it is drawn (a picture, a movie, a sound):
    /// made off the main thread -- a long timeline is encoded.
    func flattenAsset(_ a: AssetDTO) {
        guard let core, let projectId else { return }
        let name = String(localized: "\(a.name), flattened")
        flash(String(localized: "Flattening \(a.name)…"))
        Task { @MainActor [weak self] in
            let r = await Task.detached {
                core.assetOp(project: projectId, "flatten",
                             ["asset": a.id, "name": name])
            }.value
            guard let self else { return }
            if !r.ok { self.flash(r.message) }
            self.reloadAssets()
        }
    }

    /// The layer showing a composition, decomposed: that composition's
    /// layers in its place, its placement and time carried over.
    func decomposeLayer(_ id: String) {
        layerOp("decompose", id)
        if let stack = stageStack, !stack.layerStack.contains(where: {
            $0.id == selectedLayer
        }) {
            selectedLayer = ""
            selectedLayers = []
        }
    }

    /// The layer shows a composition (a still, a timeline): it can be
    /// decomposed.
    func showsComposition(_ layer: LayerDTO) -> Bool {
        guard let s = layer.source else { return false }
        return assets.first { $0.id == s }?.isComposition == true
    }

    /// What a layer shows, as the asset it is.
    func source(of layer: LayerDTO) -> AssetDTO? {
        layer.source.flatMap { s in assets.first { $0.id == s } }
    }

    /// A change to what the stage shows goes to a COMPOSITION (DESIGN
    /// §6a): a flat or generated asset there is first wrapped in its
    /// edited copy -- the same picture, clip or sound -- which the stage
    /// shows from then on; the original never changes.
    func composedTarget(_ id: String) -> String {
        guard let a = assets.first(where: { $0.id == id }), !a.isDrawn,
              a.kind == "image" || a.kind == "video" || a.kind == "audio",
              let core, let projectId else { return id }
        let r = core.assetOp(project: projectId, "modify", [
            "asset": id,
            "name": String(localized: "\(a.name), edited"),
        ])
        guard r.ok, let made = r["asset"] as? String else {
            note("error", r.message)
            return id
        }
        reloadAssets()
        if stageAssetId == id { stageAssetId = made }
        return made
    }

    func createProject(at url: URL) {
        guard let core else { return }
        let r = core.createProject(
            path: url.path,
            name: url.deletingPathExtension().lastPathComponent)
        if r.ok {
            openProject(path: url.path)
        } else {
            note("error", r.message)
        }
    }

    func reloadAssets() {
        guard let core, let projectId else { return }
        let r = core.assets(project: projectId)
        // Never quietly: a list that cannot be read leaves every new
        // result unfindable, and each one looks failed.
        guard r.ok else {
            note("error", String(localized: "Could not read the project's assets: \(r.message)"))
            return
        }
        guard var list = DTO.decode([AssetDTO].self, r["assets"]) else {
            note("error", String(localized: "Could not read the project's assets: \(String(localized: "unexpected data"))"))
            return
        }
        // A layer showing a MARKUP carries its content, as the markup
        // toolbar and the layer rows read it.
        let markups = Dictionary(list.compactMap { a in
            a.isMarkupAsset ? a.markup.map { (a.id, $0) } : nil
        }, uniquingKeysWith: { a, _ in a })
        if !markups.isEmpty {
            for i in list.indices where list[i].isComposition {
                guard var ls = list[i].layers else { continue }
                for j in ls.indices {
                    if let s = ls[j].source, let m = markups[s] {
                        ls[j].markup = m
                    }
                }
                list[i].layers = ls
            }
        }
        assets = list
        assetFolders = DTO.decode([FolderDTO].self, r["folders"]) ?? []
        reloadHistory()
        reloadOutput()
        // A picture to edit adjusted before its import finished: record
        // it now that it is an asset.
        if adjustsBase, let url = stageBaseURL,
           let a = asset(forFile: url), a.adjustments != adjustments {
            persistAdjustments()
        }
        if adjustsBase, let url = stageBaseURL,
           let a = asset(forFile: url), a.crop != crop {
            persistCrop()
        }
    }

    func importFiles(_ urls: [URL]) {
        // A project's own file -- a result dragged from the stage into the
        // prompt -- is that asset already: nothing to import.
        let fresh = urls.filter { asset(forFile: $0) == nil }
        guard let core, let projectId, !fresh.isEmpty else { return }
        let r = core.importFiles(project: projectId,
                                 paths: fresh.map { $0.path })
        if !r.ok { note("error", r.message) }
    }

    /// The imported asset for a dropped file, once its import finished.
    func asset(forFile url: URL) -> AssetDTO? {
        let path = url.standardizedFileURL.path
        func same(_ p: String?) -> Bool {
            guard let p, !p.isEmpty else { return false }
            return URL(fileURLWithPath: p).standardizedFileURL.path == path
        }
        // What was imported from there, or the asset whose media it is
        // (a result, dragged in from the stage).
        return assets.last { same($0.sourcePath) || same($0.path) }
    }

    // MARK: - Prompt

    func promptChanged() {
        intentTask?.cancel()
        let text = prompt
        let atts = promptAttachments.map { ["kind": $0.kind,
                                            "name": $0.url.lastPathComponent] }
        intentTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled, let self, let core = self.core else {
                return
            }
            // Heuristic only while typing; the model is for submit.
            _ = core.detectIntent(["text": text, "attachments": atts,
                                   "use_model": false])
        }
    }

    /// Replace the prompt text (the inline attachments stay).
    func setPrompt(_ text: String) {
        prompt = text
        promptRevision += 1
        promptChanged()
    }

    // MARK: - Generation

    /// The output size: a preset at about size² pixels, both edges
    /// multiples of 64, or the custom size on the model's grid -- the
    /// size the engine will make (it rounds to that grid itself).
    var dimensions: (width: Int, height: Int) {
        if let c = customSize {
            let g = sizeGrid
            func snap(_ v: Int) -> Int {
                max(g, Int((Double(v) / Double(g)).rounded()) * g)
            }
            return (snap(c.width), snap(c.height))
        }
        let edge = sizeClass.edge(activeModality)
        let area = Double(edge * edge)
        let r = orientation == .square ? 1 : aspectRatio.ratio
        let long = (area * r).squareRoot()
        let short = area / long
        // A clip's edges are on the video model's grid (32 for H3), so
        // 16:9 Draft is 832 × 480; a picture's on 64.
        let grid = activeModality == .video ? Double(sizeGrid) : 64
        func snap(_ v: Double) -> Int {
            max(256, Int((v / grid).rounded()) * Int(grid))
        }
        return orientation == .portrait ? (snap(short), snap(long))
                                        : (snap(long), snap(short))
    }

    /// The pixel grid of the model Start will run: sizes are multiples
    /// of it (Qwen-Image 2.1 makes them of 32).
    private var sizeGrid: Int {
        let id = willEdit ? editModel : imageModel
        return max(16, catalog.first { $0.id == id }?.sizeAlign ?? 16)
    }

    /// The orientation shown: chosen, or read off a custom size.
    var shownOrientation: Orientation {
        guard customSize != nil else { return orientation }
        let d = dimensions
        return Orientation(width: d.width, height: d.height)
    }

    /// The aspect ratio has a say: a preset that is not square.
    var showsAspectRatio: Bool {
        customSize == nil && orientation != .square
    }

    /// The size picker's choice; nil while the size is custom. Choosing a
    /// preset leaves the custom size, keeping its shape as near as the
    /// presets go: its orientation, and the closest aspect ratio.
    var sizeChoice: SizeClass? {
        get { customSize == nil ? sizeClass : nil }
        set {
            guard let newValue else { return }
            if customSize != nil { keepCustomShape() }
            sizeClass = newValue
            customSize = nil
        }
    }

    private func keepCustomShape() {
        let d = dimensions
        orientation = Orientation(width: d.width, height: d.height)
        guard orientation != .square else { return }
        let r = Double(max(d.width, d.height)) / Double(min(d.width, d.height))
        aspectRatio = AspectRatio.allCases.min {
            abs(Foundation.log($0.ratio / r))
                < abs(Foundation.log($1.ratio / r))
        } ?? aspectRatio
    }

    /// The orientation picker. With a custom size, portrait and landscape
    /// turn it (width and height trade places); any other choice goes
    /// back to the preset nearest its area.
    func setOrientation(_ o: Orientation) {
        guard let c = customSize else {
            orientation = o
            return
        }
        let now = shownOrientation
        if o == now { return }
        if o != .square && now != .square {
            customSize = PixelSize(width: c.height, height: c.width)
            sizeIsBase = false
            return
        }
        let d = dimensions
        keepCustomShape()
        sizeClass = nearestSizeClass(area: d.width * d.height)
        orientation = o
        customSize = nil
    }

    /// A size typed in the size field. False when it cannot be read.
    /// The size of the preset in use leaves it in use.
    @discardableResult
    func setCustomSize(_ text: String) -> Bool {
        guard let size = PixelSize(parsing: text) else { return false }
        let d = dimensions
        if customSize == nil, size.width == d.width, size.height == d.height {
            return true
        }
        customSize = size
        sizeIsBase = false
        return true
    }

    /// A new base sets the output size to its own -- the picture as it
    /// displays (EXIF orientation applied) -- as a custom size, so an
    /// edit keeps its shape and pixels unless another size is chosen.
    /// A base larger than the largest preset is taken at that area, same
    /// shape: generation time and memory grow with the pixels, and a
    /// 12 MP photo would not fit on most Macs.
    func adoptBaseSize(_ url: URL) {
        guard let size = Self.displaySize(url) else { return }
        var w = Double(size.width), h = Double(size.height)
        if activeModality == .video {
            // A clip opening on a picture takes its shape, at the size
            // chosen here: a clip's time grows with every pixel of every
            // frame, so the picture's own resolution is not the measure.
            let e = Double(sizeClass.edge(.video))
            let k = (e * e / (w * h)).squareRoot()
            w *= k
            h *= k
        }
        let cap = Double(SizeClass.large.rawValue * SizeClass.large.rawValue)
        if w * h > cap {
            let k = (cap / (w * h)).squareRoot()
            w *= k
            h *= k
        }
        let short = Double(PixelSize.edges.lowerBound)
        if min(w, h) < short {
            let k = short / min(w, h)
            w *= k
            h *= k
        }
        customSize = PixelSize(
            width: Int(w.rounded()).clamped(to: PixelSize.edges),
            height: Int(h.rounded()).clamped(to: PixelSize.edges))
        sizeIsBase = true
    }

    /// A still's size as it displays: its pixels, turned by the EXIF
    /// orientation (5 to 8 are a quarter turn).
    nonisolated static func displaySize(_ url: URL) -> PixelSize? {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil)
                as? [CFString: Any],
              let w = props[kCGImagePropertyPixelWidth] as? Int,
              let h = props[kCGImagePropertyPixelHeight] as? Int
        else { return nil }
        let o = (props[kCGImagePropertyOrientation] as? NSNumber)?.intValue
            ?? 1
        return o >= 5 ? PixelSize(width: h, height: w)
                      : PixelSize(width: w, height: h)
    }

    private func nearestSizeClass(area: Int) -> SizeClass {
        let m = activeModality
        return SizeClass.allCases.min {
            abs($0.edge(m) * $0.edge(m) - area)
                < abs($1.edge(m) * $1.edge(m) - area)
        } ?? .standard
    }

    var isGenerating: Bool { generationJob != nil }

    /// The mark's light sweep, once per launch, a moment after the window
    /// has appeared.
    func startupSweep() {
        guard !startupSweepDone else { return }
        startupSweepDone = true
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(450))
            markSweep += 1
        }
    }

    var canStart: Bool {
        !prompt.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            && !(activeModality == .audio ? audioModel : imageModel).isEmpty
            && projectId != nil
    }

    /// Simple mode's Start.
    func start() {
        guard canStart else { return }
        if memoryFailure != nil {
            withAnimation(Self.motion) { memoryFailure = nil }
        }
        // A tag that names nothing: the row is filled in first.
        if let tag = unboundRefTags.first {
            flash(String(localized: "Fill in the reference row first: \(tag) names nothing yet."))
            return
        }
        // The preset runs with a LoRA that is not downloaded: asked for.
        let missing = presetMissingLoRAs
        if !missing.isEmpty {
            askForPresetLoRA(missing)
            return
        }
        // The stage shows the work: a picture clicked in the row goes.
        clearReferenceFocus()
        if activeModality == .video {
            startVideo()
            return
        }
        if activeModality == .audio {
            startAudio()
            return
        }
        // A change still to be written goes to its asset (the modified
        // copy a picture of the prompt is changed through) first.
        persistAdjustments()
        persistCrop()
        var base: String?
        var refs: [String] = []
        for att in promptAttachments where att.kind == "image"
            || att.kind == "video" {
            guard let a = assetId(of: att) else {
                flash(String(localized: "Still importing \(att.url.lastPathComponent)…"))
                return
            }
            if att.isBase && base == nil { base = a } else { refs.append(a) }
        }
        let edits = willEdit && base != nil
        // An edit that stops puts the stage back as it is now -- unless
        // it shows another task, which runs on.
        stageAtStart = edits && generationJob == nil ? StageAtStart(
            stage: stage, visible: stageVisible, original: stageOriginal,
            assetId: stageAssetId, baseURL: stageBaseURL,
            baseOriginal: stageBaseOriginal, beforeBase: stageBeforeBase,
            currentSlot: currentSlot, currentAside: currentAside) : nil
        // What the picture to edit was given in the Adjust panel goes with
        // it; B, when the result lands, is the picture as the model got it.
        let baseAdjust = baseAttachment
            .flatMap { baseAdjustments[Self.fileKey($0.url)] }
            .flatMap { $0.isIdentity ? nil : $0 }
        let baseCrop = baseAttachment
            .flatMap { baseCrops[Self.fileKey($0.url)] }
            .flatMap { $0.isIdentity ? nil : $0 }
        generationBase = baseAttachment.flatMap { att in
            let shown = stageBaseURL.map { Self.fileKey($0) } ==
                Self.fileKey(att.url) ? stage.a : nil
            // As the model gets it: adjusted, then cropped.
            return (shown ?? Self.loadImage(att.url)).map {
                (cropped($0, baseCrop),
                 asset(forFile: att.url)?.name ?? att.url.lastPathComponent,
                 edits)
            }
        }
        // The generation card's size, always (DESIGN §3a): the result lies
        // on whatever it lands on at its own size, centred.
        let dims = dimensions
        // The base's look is its asset's own (its modified copy), rendered
        // for the model by the core -- not asked for again.
        submit(size: dims, base: base, references: refs,
               inline: inlineAssets)
    }

    /// Start, for a clip. It is made from the words -- the pictures in the
    /// prompt are not read -- except that the picture to edit (the base)
    /// becomes the clip's first frame when the model can open on one,
    /// with what the Adjust panel gave it.
    private func startVideo() {
        guard let core, let projectId else { return }
        persistAdjustments()
        let d = dimensions
        var req: [String: Any] = [
            "project": projectId,
            "row": rowAssetIds,
            // With pictures inline, the core drops them with their space.
            "prompt": (promptMentions.isEmpty ? prompt : promptMarked)
                .trimmingCharacters(in: .whitespacesAndNewlines),
            "model": imageModel,
            "preference": sentPreference.rawValue,
            "width": d.width,
            "height": d.height,
            "frames": clipFrames,
        ]
        if let t = sentTuning { req["tuning"] = t }
        if let seed = Int64(seedText) { req["seed"] = seed }
        if let pa = promptAssetId { req["prompt_asset"] = pa }
        if let n = promptGivenName { req["prompt_name"] = n }
        if usesReferences {
            // The row's media as references, in its order (each through
            // its look or its trim); the clip continued, its tail; the
            // mentions named as the model names them.
            for att in promptAttachments where assetId(of: att) == nil
                && ["image", "video", "audio"].contains(att.kind) {
                flash(String(localized: "Still importing \(att.url.lastPathComponent)…"))
                return
            }
            let r = referenceRequest
            req["model"] = videoReferenceModel
            req["references"] = r.refs.compactMap { $0 }
            if let c = r.cont { req["continue"] = c }
            req["reference_sound"] = hasSongReference && keepSongSound
            req["inline"] = inlineAssets
        } else if opensOnPicture, let att = baseAttachment,
                  att.kind == "image" {
            guard let a = assetId(of: att) else {
                flash(String(localized: "Still importing \(att.url.lastPathComponent)…"))
                return
            }
            // As it looks: its modified copy, rendered by the core. Its
            // mentions are <Picture 1> to the model.
            req["first"] = a
            req["inline"] = inlineAssets
        }
        generationBase = nil
        let r = core.generateVideo(req)
        guard r.ok, let job = r.job else {
            flash(r.message)
            return
        }
        began(job: job, kind: .video)
    }

    /// Start, for a song. The prompt is its words -- the style, then the
    /// lyrics under section headers (the core splits them); the pictures
    /// in the prompt are not read. The model plans the score and decides
    /// the length, the Length row at most.
    private func startAudio() {
        guard let core, let projectId else { return }
        var req: [String: Any] = [
            "project": projectId,
            "row": rowAssetIds,
            "prompt": (promptMentions.isEmpty ? prompt : promptMarked)
                .trimmingCharacters(in: .whitespacesAndNewlines),
            "model": audioModel,
            "plan": songPlan.rawValue,
            "preference": sentPreference.rawValue,
        ]
        if speaks {
            // Speech: about how long, in the row's voice.
            if let s = speechSeconds { req["seconds"] = s }
            if let v = speechVoice, let id = assetId(of: v) {
                req["voice"] = id
            }
        } else if let s = songSeconds {
            req["max_seconds"] = s
        }
        if let pa = promptAssetId { req["prompt_asset"] = pa }
        if let n = promptGivenName { req["prompt_name"] = n }
        if let t = sentTuning { req["tuning"] = t }
        if let seed = Int64(seedText) { req["seed"] = seed }
        generationBase = nil
        let r = core.generateAudio(req)
        guard r.ok, let job = r.job else {
            flash(r.message)
            return
        }
        began(job: job, kind: .audio)
    }

    /// A job Start began: the light sweeps once, before its first preview,
    /// and a long prompt shrinks to its three rows (the box's own toggle
    /// grows it again) -- the stage takes the height.
    private func began(job: String, kind: Modality = .image) {
        markSweep += 1
        // It lands in the ACTIVE composition, as a new layer: above its
        // selected layer -- in it, when blank -- at the frame or page
        // shown, when it is the one on the stage; else on top.
        var ctx = TaskContext(kind: kind, base: generationBase,
                              stageAtStart: stageAtStart)
        if let onto = activeComposition {
            ctx.onto = onto.id
            if generationJob == nil, stageStack?.id == onto.id {
                ctx.at = activeLayer
                if clipOnStage { ctx.offset = videoFrame }
                if pagedOnStage { ctx.offset = stagePage }
            }
        }
        taskContexts[job] = ctx
        generationBase = nil
        stageAtStart = nil
        reloadTasks()
        withAnimation(Self.motion) {
            promptCompact = true
            // The stage shows the work.
            focusedReference = nil
        }
        watchTask(job)
    }

    /// The ACTIVE composition (DESIGN §3a): what is being worked on -- a
    /// composition or a still composition, never a flat asset, which is
    /// only viewed (its first change makes its edited copy, and that is
    /// active). The one on the stage; or the one waiting behind a viewed
    /// asset, a picture to edit, a task the stage watches. Nil: nothing
    /// is -- a generation makes a composition of its own.
    var activeComposition: AssetDTO? {
        activeCompositionId.flatMap { id in
            assets.first { $0.id == id && $0.isComposition }
        }
    }

    private var activeCompositionId: String? {
        if generationJob != nil { return watchReturn }
        if viewedAsset != nil { return viewReturn }
        if stageBaseURL != nil { return stageBeforeBase?.assetId }
        return stageAssetId
    }

    /// The active composition while the stage watches a task.
    @ObservationIgnored private var watchReturn: String?

    /// The task the stage watches, as the app keeps it.
    private var watchedTask: TaskContext? {
        generationJob.flatMap { taskContexts[$0] }
    }

    /// The task queue again, from the core: this project's tasks.
    func reloadTasks() {
        guard let core else { return }
        let r = core.tasks()
        let list = (DTO.decode([TaskDTO].self, r["tasks"]) ?? [])
            .filter { $0.project == projectId }
        if list != tasks {
            withAnimation(Self.motion) { tasks = list }
        }
    }

    /// The task making an asset, while it is queued or running.
    func task(of asset: String) -> TaskDTO? {
        tasks.first { $0.asset == asset && !$0.asset.isEmpty }
    }

    /// The stage WATCHES a task: a generation's latest preview (or, before
    /// one, what it was made from), its phase and time; an export's frames
    /// over the stage; an upscale's composition, its Crop panel's bar. A
    /// queued one shows as waiting.
    func watchTask(_ job: String) {
        guard let t = tasks.first(where: { $0.job == job }) else { return }
        if t.kind == "export" {
            withAnimation(Self.motion) {
                exportShow = ExportShow(
                    job: job,
                    file: ((t.destination ?? "") as NSString)
                        .lastPathComponent)
            }
            return
        }
        if t.kind == "upscale" {
            if let src = t.source,
               let a = assets.first(where: { $0.id == src }) {
                if stageAssetId != src { setActive(a) }
                if let l = t.layer { selectLayer(l) }
                upscaleJob = job
                withAnimation(Self.motion) { openPanel = .crop }
            }
            return
        }
        guard let ctx = taskContexts[job] else { return }
        if generationJob == nil {
            // What is active stays so while the stage watches.
            watchReturn = activeCompositionId
            // A preview is shown alone: the compare goes off while its
            // button still can -- watching greys it out.
            endCompare()
        }
        if viewedAsset != nil {
            viewedAsset = nil
            viewReturn = nil
        }
        generationStarted = ctx.started
        generationEnds = ctx.ends
        generationKind = ctx.kind
        liveStep = ctx.step
        liveSteps = ctx.steps
        withAnimation(Self.motion) { generationJob = job }
        if let clip = ctx.clip {
            showLiveClip(clip)
        } else if let img = ctx.preview {
            showLive(img)
        } else if let base = ctx.base {
            // Nothing drawn yet: what it is made from, waiting.
            withAnimation(Self.motion) {
                stage.a = base.image
                stage.video = nil
                stage.clip = nil
                stage.b = nil
                stage.mode = .a
                stage.aLabel = base.label
                stageVisible = true
            }
        } else if stageVisible && t.state == "queued" {
            // A new picture, not begun: the mark and its status.
            withAnimation(Self.motion) { stageVisible = false }
        }
    }

    /// The stage leaves the task it watched, for what was picked; the task
    /// runs on (Assets lists it).
    private func unwatchTask() {
        if exportShow != nil {
            withAnimation(Self.motion) { exportShow = nil }
        }
        guard generationJob != nil else { return }
        generationJob = nil
        generationStarted = nil
        generationEnds = nil
    }

    /// A new estimate of the time left, `left` seconds from now. A small
    /// change is eased in (the recent pace moves a little every second,
    /// and the total beside the elapsed time should not twitch); a large
    /// one -- a new phase, its own estimate -- is taken as it is.
    private func estimateEnd(_ job: String, in left: Double) {
        guard taskContexts[job] != nil else { return }
        let end = Date.now.addingTimeInterval(left)
        if let old = taskContexts[job]?.ends,
           abs(end.timeIntervalSince(old)) < 15 {
            taskContexts[job]?.ends = old.addingTimeInterval(
                end.timeIntervalSince(old) * 0.3)
        } else {
            taskContexts[job]?.ends = end
        }
        if job == generationJob { generationEnds = taskContexts[job]?.ends }
    }

    /// The stack player's frames drawn and skipped since it was put on
    /// (a snapshot aid); nil without one.
    var stackCounts: (drawn: Int, skipped: Int)? {
        stackPlayback == nil ? nil : StackBox.shown?.counts
    }

    /// What the export on the stage is doing, for its bar and caption.
    var exportPhase: JobPhase? {
        exportShow.flatMap { jobs[$0.job]?.phase }
    }

    /// "Exporting · 34%", "Waiting…": the export on the stage's caption.
    var exportCaption: String {
        guard let job = exportShow?.job else { return "" }
        if let p = jobs[job]?.phase { return p.caption(kind: .video) }
        return jobs[job]?.state == "queued"
            ? String(localized: "Waiting…") : String(localized: "Preparing…")
    }

    /// Stop pressed under the export on the stage: it is cancelled (the
    /// file is not written) and the stage is as it was.
    func stopExport() {
        guard let job = exportShow?.job else { return }
        cancel(job: job)
    }

    /// What the generation is doing, for its progress bar and caption.
    var generationPhase: JobPhase? {
        generationJob.flatMap { jobs[$0]?.phase }
    }

    /// "Generating · 47%"; "Queued · T2" while tasks run before it, or
    /// "Waiting…" behind another job.
    var generationCaption: String {
        guard let job = generationJob else { return "" }
        if let p = jobs[job]?.phase { return p.caption(kind: generationKind) }
        guard jobs[job]?.state == "queued" else {
            return String(localized: "Preparing…")
        }
        if let t = tasks.first(where: { $0.job == job }), t.position > 0 {
            return String(localized: "Queued · \(t.badge)")
        }
        return String(localized: "Waiting…")
    }

    /// The asset each mention in the text names, in order; "" for one
    /// not imported yet.
    var inlineAssets: [String] {
        promptMentions.map { id in
            promptAttachments.first { $0.id == id }
                .flatMap { assetId(of: $0) } ?? ""
        }
    }

    /// The asset a prompt item stands for: its own (from the list, or its
    /// modified copy), else its file's once imported.
    func assetId(of item: PromptAttachment) -> String? {
        item.asset ?? asset(forFile: item.url)?.id
    }

    /// The picture each tag names, numbered as the core numbers an
    /// edit's pictures: the base first, then the other pictures in the
    /// prompt's order.
    private var numberedPictures: [PromptAttachment] {
        let pics = promptAttachments.filter { $0.kind == "image" }
        return pics.filter(\.isBase) + pics.filter { !$0.isBase }
    }

    /// What the edit model's prompts call its n-th picture ("<image{n}>"),
    /// or "" when it has no such tags.
    var referenceTag: String {
        guard willEdit else { return "" }
        return catalog.first { $0.id == editModel }?.referenceTag ?? ""
    }

    /// The attachment a tag in the prompt names, by number.
    func picture(numbered n: Int) -> PromptAttachment? {
        let pics = numberedPictures
        return n >= 1 && n <= pics.count ? pics[n - 1] : nil
    }

    /// The picture being edited, if the prompt holds one.
    var baseAttachment: PromptAttachment? {
        promptAttachments.first { $0.isBase }
    }

    /// The file of the result on the stage -- or the copy made to drag or
    /// share it adjusted. Dragged into the prompt, it becomes the base.
    func isStageResultFile(_ url: URL) -> Bool {
        let path = url.standardizedFileURL.path
        if path.hasPrefix(Self.shareRoot.standardizedFileURL.path) {
            return true
        }
        if stageBaseURL?.standardizedFileURL.path == path {
            return true
        }
        guard let own = currentAsset?.url else {
            return false
        }
        return own.standardizedFileURL.path == path
    }

    /// A picture dropped on the stage (or on the mark, before anything is
    /// made) becomes the base: it goes into the prompt as the picture to
    /// edit and shows on the stage. False when it is not a picture.
    @discardableResult
    func setBase(from url: URL) -> Bool {
        guard let type = UTType(filenameExtension: url.pathExtension),
              type.conforms(to: .image) else { return false }
        guard showBase(url) else { return false }
        addReferences([url], forceBase: true)
        return true
    }

    // MARK: The prompt's staged media

    /// The prompt CLEARED: its words, its row of media and their mentions,
    /// the assistant's suggestion -- a row item active on the stage let
    /// go first; the picture to edit gone, the stage's own work is back
    /// (baseChanged, from the view).
    func clearPrompt() {
        clearReferenceFocus()
        promptAssetId = nil
        promptName = nil
        promptAttachments = []
        referenceThumbs = [:]
        promptMentions = []
        promptMarked = ""
        prompt = ""
        dropSuggestion()
        promptCompact = false
        promptRevision += 1
        promptChanged()
    }

    /// Media into the prompt's row, each file once (one staged already
    /// is that one). A picture is the BASE when `forceBase` (dropped on
    /// the stage) or when it is the stage's result; the base leads the
    /// row, and the one before it becomes a reference. They are imported
    /// as they come. Their ids, in order.
    @discardableResult
    func addReferences(_ urls: [URL], forceBase: Bool = false) -> [UUID] {
        var list = promptAttachments
        var ids: [UUID] = []
        var baseTaken = false
        var fresh: [PromptAttachment] = []
        for url in urls where url.isFileURL {
            let kind = PromptTextView.kind(of: url)
            let base = kind == "image" && !baseTaken
                && (forceBase || isStageResultFile(url))
            if base { baseTaken = true }
            let key = Self.fileKey(url)
            var item: PromptAttachment
            if let i = list.firstIndex(where: { Self.fileKey($0.url) == key }) {
                item = list.remove(at: i)
                if !base {
                    list.insert(item, at: i)
                    ids.append(item.id)
                    continue
                }
            } else {
                item = PromptAttachment(id: UUID(), url: url, kind: kind)
                fresh.append(item)
            }
            if base {
                for j in list.indices { list[j].isBase = false }
                item.isBase = true
                list.insert(item, at: 0)
            } else {
                list.append(item)
            }
            ids.append(item.id)
        }
        withAnimation(Self.motion) { promptAttachments = list }
        for item in fresh { loadReferenceThumb(item) }
        importFiles(urls)
        promptChanged()
        return ids
    }

    /// Out of the row -- and out of the text, where it was mentioned
    /// (PromptEditor drops the mentions of what is not staged).
    func removeReference(_ id: UUID) {
        // Active: let go first, the stage as it was.
        if focusedReference == id { endActivation(restore: true) }
        withAnimation(Self.motion) {
            promptAttachments.removeAll { $0.id == id }
        }
        referenceThumbs[id] = nil
        promptChanged()
    }

    /// The picture to edit, or -- on the base -- none: a picture made
    /// the base leads the row.
    func toggleBase(_ id: UUID) {
        guard let i = promptAttachments.firstIndex(where: { $0.id == id }),
              promptAttachments[i].kind == "image" else { return }
        // The suggestion taken (or the base chosen another way): it goes.
        baseHint = nil
        var list = promptAttachments
        if list[i].isBase {
            list[i].isBase = false
        } else {
            for j in list.indices { list[j].isBase = false }
            var item = list.remove(at: i)
            item.isBase = true
            list.insert(item, at: 0)
        }
        withAnimation(Self.motion) { promptAttachments = list }
    }

    /// The n-th picture of the row (0 first) as the base, or not (the
    /// snapshot hooks' badge clicks).
    func toggleBase(picture n: Int) {
        let pics = promptAttachments.filter { $0.kind == "image" }
        if pics.indices.contains(n) { toggleBase(pics[n].id) }
    }

    /// `id` to `index` in the row -- the order the model gets them in.
    /// The base keeps the lead.
    func moveReference(_ id: UUID, to index: Int) {
        guard let from = promptAttachments.firstIndex(where: { $0.id == id }),
              !promptAttachments[from].isBase else { return }
        var list = promptAttachments
        let item = list.remove(at: from)
        let lead = list.first?.isBase == true ? 1 : 0
        list.insert(item, at: min(max(index, lead), list.count))
        guard list != promptAttachments else { return }
        withAnimation(Self.motion) { promptAttachments = list }
    }

    /// A click on a thumbnail (released on it): it becomes the ACTIVE
    /// media -- or, active, it lets go.
    func toggleReferenceFocus(_ id: UUID) {
        if focusedReference == id {
            endActivation(restore: true)
        } else {
            activateReference(id)
        }
    }

    func clearReferenceFocus() {
        if focusedReference != nil { endActivation(restore: true) }
    }

    /// What the stage goes back to when a row item is let go.
    enum StageReturn {
        case asset(String), base, none
    }

    private var currentReturn: StageReturn {
        if adjustsBase, baseAttachment != nil { return .base }
        if let id = stageAssetId { return .asset(id) }
        return .none
    }

    /// A row item made ACTIVE: its own copy on the stage -- made now, the
    /// original left as it was -- and every panel (Information with its
    /// canvas and length, Layers, Adjust, Crop, Trim) works on it, a
    /// picture, a clip or a sound alike. The stage's work comes back when
    /// it is let go.
    private func activateReference(_ id: UUID) {
        guard !isGenerating, let core, let projectId,
              let i = promptAttachments.firstIndex(where: { $0.id == id })
        else { return }
        let item = promptAttachments[i]
        guard let src = assetId(of: item) else {
            flash(String(localized: "Still importing \(item.url.lastPathComponent)…"))
            return
        }
        // Another active one ends; the stage is about to be replaced.
        if focusedReference != nil {
            endActivation(restore: false)
        } else {
            stageReturn = currentReturn
        }
        var target = src
        if !item.ownCopy {
            let name = assets.first { $0.id == src }?.name
                ?? item.url.lastPathComponent
            let r = core.assetOp(project: projectId, "modify", [
                "asset": src, "name": String(localized: "\(name), edited"),
            ])
            guard r.ok, let made = r["asset"] as? String else {
                flash(r.message)
                return
            }
            promptAttachments[i].asset = made
            promptAttachments[i].ownCopy = true
            promptAttachments[i].original = src
            reloadAssets()
            target = made
        }
        guard let a = assets.first(where: { $0.id == target }) else { return }
        let keep = lastStageResult
        withAnimation(Self.motion) { focusedReference = id }
        putOnStage(a)
        lastStageResult = keep
        // Alone, named as the model will call it.
        withAnimation(Self.motion) {
            stage.b = nil
            stage.bLabel = ""
            stage.mode = .a
            stage.aLabel = activeLabel(promptAttachments[i])
        }
    }

    private func activeLabel(_ item: PromptAttachment) -> String {
        if item.isBase { return String(localized: "To edit") }
        if let n = referenceNumber(item) {
            return String(localized: "Picture \(String(n))")
        }
        return ""
    }

    /// The active row item let go: its changes written; its copy gone if
    /// nothing was changed on it (the item the original again); the stage
    /// as it was, when `restore`.
    func endActivation(restore: Bool) {
        guard let id = focusedReference else { return }
        persistAdjustments()
        persistCrop()
        if clipOnStage {
            persistClipTracks()
            persistTrim()
        }
        withAnimation(Self.motion) { focusedReference = nil }
        discardUntouchedCopy(id)
        let back = stageReturn
        stageReturn = nil
        guard restore else { return }
        switch back {
        case .asset(let a)?:
            if let x = assets.first(where: { $0.id == a }) {
                putOnStage(x)
            } else {
                clearStage()
            }
        case .base?:
            if let b = baseAttachment { showBase(b.url, item: b) }
        default:
            clearStage()
        }
    }

    /// A copy made for an activation that nothing was changed on: gone,
    /// the item the original again.
    private func discardUntouchedCopy(_ id: UUID) {
        guard let core, let projectId,
              let i = promptAttachments.firstIndex(where: { $0.id == id }),
              promptAttachments[i].ownCopy,
              let copy = promptAttachments[i].asset,
              let orig = promptAttachments[i].original,
              let c = assets.first(where: { $0.id == copy }),
              let o = assets.first(where: { $0.id == orig }),
              Self.untouched(c, from: o)
        else { return }
        let r = core.assetOp(project: projectId, "remove", ["asset": copy])
        guard r.ok else { return }
        promptAttachments[i].asset = orig
        promptAttachments[i].ownCopy = false
        promptAttachments[i].original = nil
        reloadAssets()
    }

    /// An edited copy as it was made: a composition of one layer showing
    /// the original, nothing changed -- or a copy of a composition with
    /// nothing changed from it.
    static func untouched(_ c: AssetDTO, from o: AssetDTO) -> Bool {
        if o.isComposition || o.isMarkupAsset {
            return (c.modifiers ?? []) == (o.modifiers ?? [])
                && (c.layers ?? []) == (o.layers ?? [])
                && c.canvas == o.canvas && c.timeline == o.timeline
                && c.markup == o.markup
        }
        let ls = c.layers ?? []
        return (c.modifiers ?? []).isEmpty && ls.count == 1
            && ls[0].source == o.id && ls[0].visible && !ls[0].mask
            && (ls[0].time ?? LayerTimeDTO()) == LayerTimeDTO()
            && c.canvas?.resized != true && c.timeline == nil
    }

    /// Nothing on the stage: the mark again.
    func clearStage() {
        dropStackPlan()
        withAnimation(Self.motion) {
            stage = ViewerState()
            stageVisible = false
            compareEngaged = false
        }
        parkedB = nil
        stageOriginal = nil
        stageAssetId = nil
        currentSlot = .a
        currentAside = nil
        adjustments = ImageAdjustments()
        crop = CropSpec()
    }

    /// A flat asset's EDITED COPY -- a composition of one layer showing it
    /// -- made and set active: only a composition is edited (DESIGN §3a).
    func editCopy(_ a: AssetDTO) {
        let id = composedTarget(a.id)
        if let c = assets.first(where: { $0.id == id }) { setActive(c) }
    }

    /// An asset of the list SET ACTIVE -- a capture too, the project's
    /// composition too: on the stage, what every panel works on, changed
    /// in place.
    func setActive(_ a: AssetDTO) {
        if focusedReference != nil { endActivation(restore: false) }
        stageReturn = nil
        putOnStage(a)
    }

    /// What the model calls a staged picture, by number -- the base 1,
    /// then the other pictures in the row's order; none for a clip.
    func referenceNumber(_ item: PromptAttachment) -> Int? {
        // A clip's reference: its number within its kind ("<Video 1>").
        if usesReferences {
            return referenceBadge(item).flatMap {
                Int($0.filter(\.isNumber))
            }
        }
        return numberedPictures.firstIndex { $0.id == item.id }
            .map { $0 + 1 }
    }

    /// The file's thumbnail, made here (ImageIO, or a clip's first frame)
    /// -- not QuickLook, whose cache would keep a copy on disk, which the
    /// anonymous session must not leave behind.
    private func loadReferenceThumb(_ item: PromptAttachment) {
        let url = item.url
        let kind = item.kind
        let id = item.id
        Task { [weak self] in
            let cg: CGImage? = kind == "video"
                ? await PromptTextView.videoFrame(url, maxPixels: 240)
                : kind == "audio"
                ? await AudioWaveform.image(url, width: 240, height: 120)
                : await Task.detached(priority: .userInitiated) {
                    PromptTextView.imageThumbnail(url, maxPixels: 240)
                }.value
            guard let self, let cg,
                  self.promptAttachments.contains(where: { $0.id == id })
            else { return }
            self.referenceThumbs[id] = cg
        }
    }

    /// The prompt's base changed (dropped, toggled on its badge, removed):
    /// it takes the output size, and the stage shows it. With none, the
    /// stage goes back to what it showed before -- the last result, or
    /// the mark when nothing has been made. A result dragged in as the
    /// base is on the stage already, and stays as it is; so does a
    /// generation in flight.
    ///
    /// Run from the view (ComposerStack's onChange), not a didSet.
    func baseChanged(to url: URL?) {
        if let url { adoptBaseSize(url) }
        guard !isGenerating else { return }
        if let url {
            let path = url.standardizedFileURL.path
            if stageBaseURL?.standardizedFileURL.path == path { return }
            if stageBaseURL == nil && isStageResultFile(url) { return }
            showBase(url)
        } else if stageBaseURL != nil {
            restoreStage()
        }
    }

    /// What the stage was when an EDIT started: put back when it stops or
    /// fails. Its live frames take the stage (showLive) -- and with it the
    /// stage's hold on the picture being edited -- so without this a
    /// stopped edit left a preview frame on the stage that no file or
    /// asset is, and the next Start edited from "the picture on the
    /// stage": a picture that was never made. Put back, the stage shows
    /// the original again, the picture to edit.
    /// A task Start began, as the app keeps it: what its result is
    /// compared with (an edit's base) and where it goes (the layer chosen
    /// at Start), the stage as it was at Start (an edit stopped puts it
    /// back), its clock, and its latest preview -- shown at once when
    /// the stage watches it again.
    private struct TaskContext {
        var kind: Modality
        var base: (image: CGImage, label: String, fitted: Bool)?
        /// Where its result lands: the composition active at Start, its
        /// selected layer when it was on the stage, and the frame (or
        /// page) shown there; nil: a composition of its own.
        var onto: String?
        var at: String?
        var offset = 0
        var stageAtStart: StageAtStart?
        /// When it began to run (nil while queued).
        var started: Date?
        var ends: Date?
        var step = 0
        var steps = 0
        var preview: CGImage?
        var clip: PreviewClip?
    }

    private struct StageAtStart {
        var stage: ViewerState
        var visible: Bool
        var original: CGImage?
        var assetId: String?
        var baseURL: URL?
        var baseOriginal: CGImage?
        var beforeBase: StageBeforeBase?
        var currentSlot: StageSlot?
        var currentAside: (image: CGImage, fit: CanvasFit, label: String,
                           surface: IOSurface?)?
    }
    @ObservationIgnored private var stageAtStart: StageAtStart?

    /// What the stage showed before a base took it: put back when the
    /// prompt has no base again.
    private struct StageBeforeBase {
        var stage: ViewerState
        var visible: Bool
        var original: CGImage?
        var assetId: String?
        var currentSlot: StageSlot?
        var currentAside: (image: CGImage, fit: CanvasFit, label: String,
                           surface: IOSurface?)?
        var adjustments: ImageAdjustments
        var crop: CropSpec
    }
    private var stageBeforeBase: StageBeforeBase?
    /// The row item the stage shows to adjust (`stagedItem`).
    private var stagedItemId: UUID?

    /// The results the stage holds -- shown, or set aside while a picture
    /// of the prompt (or a running edit) has it, to come back after.
    var stageAssetsHeld: [String] {
        ([stageAssetId, stageBeforeBase?.assetId, stageAtStart?.assetId]
            + taskContexts.values.map { $0.stageAtStart?.assetId })
            .compactMap { $0 }
    }

    /// The picture to edit, on the stage, captioned "To edit". False when
    /// it cannot be read.
    @discardableResult
    private func showBase(_ url: URL, item: PromptAttachment? = nil,
                          label: String? = nil) -> Bool {
        guard let img = Self.loadImage(url) else { return false }
        persistAdjustments()  // what was on the stage keeps its values
        persistCrop()
        if stageBaseURL == nil {
            stageBeforeBase = StageBeforeBase(
                stage: stage, visible: stageVisible, original: stageOriginal,
                assetId: stageAssetId, currentSlot: currentSlot,
                currentAside: currentAside,
                adjustments: adjustments, crop: crop)
        }
        stageBaseURL = url
        withAnimation(Self.motion) {
            stage.a = img
            stage.video = nil
            stage.clip = nil
            // A clip opens on it rather than editing it.
            stage.aLabel = label ?? (opensOnPicture
                ? String(localized: "First frame")
                : String(localized: "To edit"))
            stage.b = nil
            stage.bLabel = ""
            stage.mode = .a
            stageVisible = true
        }
        stage.fitRequest += 1
        // Not a result: nothing to share or inspect until one lands. The
        // picture itself can be adjusted before it is edited, and comes
        // back with what it had.
        stageOriginal = nil
        stageAssetId = nil
        currentSlot = .a
        currentAside = nil
        adjustTask?.cancel()
        stageBaseOriginal = img
        stagedItemId = item?.id
            ?? promptAttachments.first { Self.fileKey($0.url) == Self.fileKey(url) }?.id
        // What its asset carries (the item's modified copy's look), else
        // what it was given here before its import finished.
        let shown = stagedItem.flatMap { assetId(of: $0) }
            .flatMap { id in assets.first { $0.id == id } }
            ?? asset(forFile: url)
        adjustments = shown?.adjustments
            ?? baseAdjustments[Self.fileKey(url)] ?? ImageAdjustments()
        crop = shown?.crop
            ?? baseCrops[Self.fileKey(url)] ?? CropSpec()
        if !adjustments.isIdentity { renderAdjusted() }
        return true
    }

    /// One file's key, whichever way its URL was spelled.
    private static func fileKey(_ url: URL) -> String {
        url.standardizedFileURL.path
    }

    /// The stage as it was before the base took it.
    private func restoreStage() {
        persistAdjustments()  // the picture to edit keeps its values
        persistCrop()
        stageBaseURL = nil
        stageBaseOriginal = nil
        stagedItemId = nil
        let before = stageBeforeBase
        stageBeforeBase = nil
        adjustTask?.cancel()
        withAnimation(Self.motion) {
            if let before {
                stage = before.stage
                stageVisible = before.visible
            } else {
                stage = ViewerState()
                stageVisible = false
            }
        }
        stage.fitRequest += 1
        stageOriginal = before?.original
        stageAssetId = before?.assetId
        currentSlot = before.map { $0.currentSlot } ?? .a
        currentAside = before?.currentAside
        adjustments = before?.adjustments ?? ImageAdjustments()
        crop = before?.crop ?? CropSpec()
    }

    /// `size`: the output's; nil lets an edit follow its base's shape.
    /// `inline`: the asset at each U+FFFC of `promptMarked` (simple
    /// mode); empty sends the plain prompt.
    private func submit(size: (width: Int, height: Int)?,
                        base: String? = nil, references refs: [String],
                        inline: [String] = [],
                        baseAdjust: ImageAdjustments? = nil,
                        baseCrop: CropSpec? = nil) {
        guard let core, let projectId else { return }
        var req: [String: Any] = [
            "project": projectId,
            "prompt": (inline.isEmpty ? prompt : promptMarked)
                .trimmingCharacters(in: .whitespacesAndNewlines),
            "preference": sentPreference.rawValue,
        ]
        if let t = sentTuning { req["tuning"] = t }
        if !inline.isEmpty { req["inline"] = inline }
        // Its tags' row, and the prompt it came from (DESIGN §10c).
        req["row"] = rowAssetIds
        if let pa = promptAssetId { req["prompt_asset"] = pa }
        if let n = promptGivenName { req["prompt_name"] = n }
        if willEdit {
            req["model"] = editModel
            req["mode"] = "edit"
            // The base fills the size asked for (centre-cropped to it);
            // pictures without one are composed into a picture of it.
            if let base {
                req["base"] = base
                if let baseAdjust { req["base_adjust"] = baseAdjust.json }
                if let baseCrop { req["base_crop"] = baseCrop.json }
            }
        } else {
            req["model"] = imageModel
        }
        if let size {
            req["width"] = size.width
            req["height"] = size.height
        }
        if !negative.isEmpty { req["negative"] = negative }
        if let seed = Int64(seedText) { req["seed"] = seed }
        if !refs.isEmpty { req["references"] = refs }
        let r = core.generateImage(req)
        guard r.ok, let job = r.job else {
            flash(r.message)
            return
        }
        began(job: job)
    }

    func stop() {
        if let job = generationJob { cancel(job: job) }
    }

    func cancel(job: String) {
        _ = core?.cancel(job: job)
    }

    /// The assistant rewrites the prompt for the model Start would run.
    /// A model whose makers publish a rewriter (Qwen-Image 2.1) gets
    /// theirs: the assistant is shown an edit's pictures and names them
    /// as the model does. A shape the user chose is part of the request.
    func enhance() {
        guard let core, !prompt.isEmpty, enhanceJob == nil else { return }
        var req: [String: Any] = [
            // What Start makes, for the model it runs -- whose language
            // the suggestion is written in (English for a model that
            // reads only English). A song's words are its style, then its
            // lyrics.
            "target": activeModality.rawValue,
            "model": runningModel,
            "mode": willEdit ? "edit" : "generate",
        ]
        req["prompt"] = promptMarked
        req["inline"] = inlineAssets
        req["row"] = rowAssetIds
        if let pid = projectId { req["project"] = pid }
        if let b = baseAttachment, let a = assetId(of: b) {
            req["base"] = a
        }
        req["references"] = promptAttachments
            .filter { !$0.isBase && $0.kind == "image" }
            .compactMap { assetId(of: $0) }
        // A clip as Start would make it -- its length, its frame, the
        // picture it opens on (the base) or its references as Start sends
        // them: a video model's makers' guide is written to them.
        if activeModality == .video {
            req["frames"] = clipFrames
            let d = dimensions
            req["size"] = "\(d.width)x\(d.height)"
            if usesReferences {
                let r = referenceRequest
                req["references"] = r.refs.compactMap { $0 }
                if let c = r.cont { req["continue"] = c }
                req["reference_sound"] = hasSongReference && keepSongSound
                req.removeValue(forKey: "base")
            }
        }
        // A shape the user chose -- not the default square, not the
        // base's own size -- is part of the request.
        if activeModality == .image,
           customSize != nil ? !sizeIsBase : orientation != .square {
            let d = dimensions
            req["size"] = "\(d.width)x\(d.height)"
        }
        // A song's Length: its lyrics are written to fit (Auto: none);
        // speech's, its words.
        if activeModality == .audio,
           let s = speaks ? speechSeconds : songSeconds {
            req["seconds"] = s
        }
        let r = core.enhancePrompt(req)
        guard r.ok, let job = r.job else {
            flash(r.message)
            return
        }
        withAnimation(Self.motion) {
            enhanceJob = job
            enhanceDraft = ""
            enhanceTab = currentTabId
            enhanced = nil
        }
    }

    /// The model whose prompt outline an empty box starts from: the one
    /// Start would run, when it has one (catalog `prompting.outline`).
    var outlineModel: CatalogModel? {
        runningModelInfo.flatMap { $0.hasOutline == true ? $0 : nil }
    }

    /// The box empty, it starts from the model's prompt template -- its
    /// sections as it was trained on them, each with a hint, the row's
    /// media already named where they belong (core
    /// assist::prompt_outline). The assistant is not asked: there is
    /// nothing to enhance yet.
    func insertOutline() {
        guard let core, prompt.isEmpty, let m = outlineModel else { return }
        let row = promptAttachments.map(\.kind)
            .filter { Self.refKind($0) != nil }
        let text = core.promptOutline(model: m.id, row: row)
        guard !text.isEmpty else { return }
        promptMentions = []
        promptMarked = text
        setPrompt(text)
        dropSuggestion()
    }

    /// The assistant's button: words are enhanced; an empty box starts
    /// from the model's prompt template.
    func assistantPressed() {
        if prompt.isEmpty { insertOutline() } else { enhance() }
    }

    var assistantEnabled: Bool {
        guard enhanceJob == nil else { return false }
        return prompt.isEmpty ? outlineModel != nil : assistantReady
    }

    var assistantHelp: String {
        if prompt.isEmpty, let m = outlineModel {
            return String(localized: "Start from \(m.name)'s prompt template")
        }
        return assistantReady
            ? String(localized: "Enhance the prompt with the on-device assistant")
            : String(localized: "Install the assistant model in Settings › Models")
    }

    /// The open prompt's tab (nil before the Prompt Editor made any).
    var currentTabId: UUID? {
        promptTabs.indices.contains(activePromptTab)
            ? promptTabs[activePromptTab].id : nil
    }

    /// The running suggestion is for the prompt open now.
    var enhanceHere: Bool { enhanceTab == nil || enhanceTab == currentTabId }

    /// The assistant is writing a suggestion for the prompt open now.
    var suggestionWriting: Bool {
        enhanced == nil && enhanceJob != nil && enhanceHere
    }

    /// A suggestion is offered, or being written, for the prompt open now.
    var suggesting: Bool { enhanced != nil || suggestionWriting }

    /// The suggestion -- written, or being written -- let go: a running
    /// assistant stopped.
    func dropSuggestion() {
        if let job = enhanceJob {
            cancel(job: job)
            enhanceJob = nil
        }
        enhanceDraft = ""
        enhanceTab = nil
        enhanced = nil
    }

    /// The suggestion replaces the prompt: its tags ("<image2>") become
    /// the pictures they name, in place (PromptEditor), and the shape it
    /// was written for becomes the output's.
    func acceptEnhanced() {
        guard let e = enhanced else { return }
        negative = e.negative
        withAnimation(Self.motion) {
            enhanced = nil
            applySuggestedShape(aspect: e.aspect, follow: e.follow)
        }
        setPrompt(e.prompt)
    }

    /// `follow` ("<image1>"): the size of that picture; else `aspect`
    /// ("3:2"): a preset when one has that shape, else a custom size of
    /// that shape at the present area.
    private func applySuggestedShape(aspect: String, follow: String) {
        if !follow.isEmpty {
            let digits = follow.filter(\.isNumber)
            if let n = Int(digits), let pic = picture(numbered: n) {
                adoptBaseSize(pic.url)
            }
            return
        }
        let parts = aspect.split(separator: ":").compactMap { Double($0) }
        guard parts.count == 2, parts[0] > 0, parts[1] > 0 else { return }
        let r = parts[0] / parts[1]
        if abs(r - 1) < 0.02 {
            customSize = nil
            orientation = .square
            return
        }
        let long = max(r, 1 / r)
        if let preset = AspectRatio.allCases.first(where: {
            abs(Foundation.log($0.ratio / long)) < 0.03
        }) {
            customSize = nil
            orientation = r > 1 ? .landscape : .portrait
            aspectRatio = preset
            return
        }
        let d = dimensions
        let area = Double(d.width * d.height)
        let w = (area * r).squareRoot()
        customSize = PixelSize(
            width: Int(w.rounded()).clamped(to: PixelSize.edges),
            height: Int((area / w).rounded()).clamped(to: PixelSize.edges))
        sizeIsBase = false
    }

    /// Reject: the suggestion goes -- stopped, if it is still being
    /// written.
    func dismissEnhanced() {
        withAnimation(Self.motion) { dropSuggestion() }
    }

    // MARK: - Viewers

    /// The picture an edit result was made from (its recipe's base).
    func baseAsset(of asset: AssetDTO) -> AssetDTO? {
        guard let r = asset.recipe, r.op == "edit-image",
              let id = r.inputs?.first(where: { $0.role == "base" })?.asset
        else { return nil }
        return assets.first { $0.id == id }
    }

    // MARK: Layer looks

    /// What a layer's look changes, for its row in the Layers list: its
    /// adjustments, where it lies (crop, scale, offset), its turn -- and,
    /// on a clip, whether any of them is keyed (changes over time).
    struct LayerLook: Equatable {
        var adjust = false
        var place = false
        var turn = false
        var keyed = false
    }

    /// The selected layer's as the panels hold them now; the others' as
    /// recorded.
    func layerLook(_ id: String) -> LayerLook {
        if keyedStage, let clip = stageStack {
            let live = id == activeLayer
            let a = live ? clipAdjustKeys : clip.adjustKeys(layer: id)
            let c = live ? clipCropKeys : clip.cropKeys(layer: id)
            return LayerLook(adjust: !a.isEmpty && !a.isIdentity,
                             place: !c.place.isEmpty && !c.place.isIdentity,
                             turn: !c.turn.isEmpty && !c.turn.isIdentity,
                             keyed: a.keys.count > 1
                                || c.place.keys.count > 1
                                || c.turn.keys.count > 1)
        }
        guard let pic = stagePicture else { return LayerLook() }
        let live = id == activeLayer
        let adj = live ? adjustments : pic.adjustments(layer: id)
        let c = live ? crop : pic.crop(layer: id)
        var placed = c
        placed.rotate = 0
        placed.pad = CropSpec().pad
        return LayerLook(adjust: !adj.isIdentity,
                         place: !placed.isIdentity,
                         turn: abs(c.rotate) > 0.0001)
    }

    // MARK: Canvas and timeline

    /// The stack's own frame: its bottom layer through its crop -- what a
    /// canvas is resized from (core Controller::own_frame).
    func ownFrame(of a: AssetDTO) -> (width: Int, height: Int) {
        if let f = a.ownFrame { return (f.w, f.h) }
        let crop = a.kind == "video"
            ? a.cropKeys.place.value(at: 0) ?? CropSpec()
            : a.crop(layer: "")
        if !crop.isIdentity, crop.canvasWidth > 0, crop.canvasHeight > 0 {
            return (crop.canvasWidth, crop.canvasHeight)
        }
        if a.id == stageAssetId, a.kind == "image", let img = stageOriginal {
            return (img.width, img.height)
        }
        let f = a.info?.frame
        return (f?.w ?? 0, f?.h ?? 0)
    }

    /// Canvas Size, anchored (0, 0.5, 1 across and down).
    func setCanvas(_ a: AssetDTO, width: Int, height: Int,
                   anchorX: Double, anchorY: Double) {
        guard let core, let projectId else { return }
        // The panels' values first: the resize moves the layers' places.
        flushPanels()
        let r = core.setCanvas(project: projectId, asset: composedTarget(a.id), width: width,
                               height: height, anchorX: anchorX,
                               anchorY: anchorY)
        canvasChanged(r)
    }

    func resetCanvas(_ a: AssetDTO) {
        guard let core, let projectId else { return }
        flushPanels()
        canvasChanged(core.resetCanvas(project: projectId,
                                       asset: composedTarget(a.id)))
    }

    private func canvasChanged(_ r: CoreReply) {
        guard r.ok else {
            flash(r.message)
            return
        }
        reloadAssets()
        // A composition that keeps a frame of its own is resized as that
        // frame (core fold_canvas_): its layers' places changed with it,
        // and the panels take them again -- held, they would be written
        // back over it.
        if let s = stageStack, s.canvas?.framed == true {
            if keyedStage {
                loadClipTracks(s, layer: activeLayer)
            } else {
                crop = s.crop(layer: activeLayer)
            }
        }
        if clipOnStage {
            refreshStackPlan()
            return
        }
        if stageComposed {
            recomposite()
        } else if let pic = stagePicture, let url = pic.url,
                  let img = Self.loadImage(url) {
            // Its own frame again: the picture as read, adjusted.
            stageOriginal = img
            stage.a = img
            redrawAdjusted()
        }
        stage.fitRequest += 1
    }

    /// A clip's timeline length, in its frames; 0 is its own.
    func setTimeline(_ a: AssetDTO, frames: Int) {
        guard let core, let projectId else { return }
        let r = core.setTimeline(project: projectId,
                                 asset: composedTarget(a.id),
                                 frames: frames)
        if !r.ok { flash(r.message) }
        reloadAssets()
        if clipOnStage { refreshStackPlan() }
    }

    // MARK: History on the stage

    /// The current state's picture as shown: in its side of the compare,
    /// or kept aside while history states hold both.
    private var currentImage: CGImage? {
        switch currentSlot {
        case .a: stage.a
        case .b: stage.b
        case nil: currentAside?.image
        }
    }

    /// The current state drawn again (adjusted, composed): where it shows,
    /// or aside -- with the GPU surface it was drawn into, which the
    /// canvas shows as it is.
    private func showCurrent(_ img: CGImage, surface: IOSurface? = nil) {
        // What the stage shows is what the next result is compared with:
        // the project as composed, not the file of its layer 0.
        if !isGenerating {
            lastStageResult = (img, String(localized: "Previous"))
        }
        switch currentSlot {
        case .a:
            stage.a = img
            stage.aSurface = surface
        case .b:
            stage.b = img
            stage.bSurface = surface
        case nil:
            currentAside?.image = img
            currentAside?.surface = surface
        }
    }

    /// The history again, from the core: after a result lands, and when
    /// a project opens.
    func reloadHistory() {
        guard let core, let projectId else {
            history = []
            return
        }
        let r = core.history(project: projectId)
        guard r.ok, let list = DTO.decode([HistoryEntryDTO].self,
                                          r["entries"]) else { return }
        if list != history { history = list }
    }

    /// The history state at `url` (its picture's file), if it is one.
    func historyEntry(at url: URL) -> HistoryEntryDTO? {
        let path = url.standardizedFileURL.path
        return history.last { $0.url.standardizedFileURL.path == path }
    }

    /// The picture an asset was made from, as the model got it (its
    /// generation's base in the history): an edit's, or a clip's opening
    /// picture.
    func baseEntry(of a: AssetDTO) -> HistoryEntryDTO? {
        history.last { $0.isBase && $0.isPicture && $0.generation == a.id }
    }

    /// The stage can take a picture to compare: it shows a still, and the
    /// project has another picture -- an asset, or a base as the model
    /// got it.
    var stageTakesCompare: Bool {
        guard stageVisible && !stage.showsMotion && stage.a != nil
              && !isGenerating else { return false }
        return assets.contains {
            $0.kind == "image" && $0.head > 0 && $0.op != "project"
                && $0.id != stageAssetId
        } || history.contains { $0.isBase && $0.isPicture }
    }

    /// A picture of the asset list takes one side of the compare --
    /// dropped on its A or B button, or its menu's Compare -- as it looks:
    /// its file, or with layers or a look of its own, the core's
    /// flattening of it.
    func compareAsset(_ a: AssetDTO, in slot: StageSlot) {
        guard stageTakesCompare, a.kind == "image",
              let img = compareImage(a) else { return }
        let name = a.op == "project" ? (projectName ?? a.name) : a.name
        compare(img, label: name, id: a.id, in: slot)
    }

    /// What was dropped on A or B: a picture of the asset list (a row's
    /// "valtz-asset:<id>"), or the one an asset was made from (its file).
    /// A clip or a song goes on the stage itself. True when it took the
    /// side.
    func dropOnCompare(_ url: URL, in slot: StageSlot) -> Bool {
        if let id = Self.draggedAssets([url]).first {
            guard stageTakesCompare,
                  let a = assets.first(where: { $0.id == id }),
                  a.kind == "image" else { return false }
            compareAsset(a, in: slot)
            return true
        }
        guard stageTakesCompare, let e = historyEntry(at: url),
              e.isPicture else { return false }
        compare(e, in: slot)
        return true
    }

    /// A base as the model got it takes one side of the compare.
    func compare(_ entry: HistoryEntryDTO, in slot: StageSlot) {
        guard stageTakesCompare, entry.isPicture,
              let img = Self.loadImage(entry.url) else { return }
        compare(img, label: entry.caption, id: entry.id, in: slot)
    }

    private func compareImage(_ a: AssetDTO) -> CGImage? {
        if (a.layers ?? []).isEmpty, (a.modifiers ?? []).isEmpty,
           let url = a.url {
            return Self.loadImage(url)
        }
        guard let core, let projectId else { return nil }
        try? FileManager.default.createDirectory(
            at: Self.shareRoot, withIntermediateDirectories: true)
        let out = Self.shareRoot.appendingPathComponent(
            "compare-\(a.id)-\(a.modified).png")
        let r = core.flatten(project: projectId, asset: a.id, to: out.path)
        guard r.ok else {
            flash(r.message)
            return nil
        }
        return Self.loadImage(out)
    }

    /// A picture takes one side of the compare. It is fitted onto the
    /// canvas, the current state's pixel grid, as an edit's base is
    /// (CanvasFit.centreCrop), so the two line up. A side that showed the
    /// current state no longer does: the state is kept aside, and Return
    /// to Current brings it back.
    private func compare(_ img: CGImage, label: String, id: String,
                         in slot: StageSlot) {
        let canvas = canvasSize()
        stageCanvas = canvas
        let placed = canvas.map { CanvasFit.centreCrop(img, onto: $0) }
            ?? (img, .identity)
        if currentSlot == slot, let shown = currentImage {
            currentAside = (shown, slot == .a ? stage.aFit : stage.bFit,
                            slot == .a ? stage.aLabel : stage.bLabel,
                            slot == .a ? stage.aSurface : stage.bSurface)
            currentSlot = nil
        }
        let hadB = stage.b != nil
        parkedB = nil
        withAnimation(Self.motion) {
            compareEngaged = true
            switch slot {
            case .a:
                stage.a = placed.0
                stage.aFit = placed.1
                stage.aLabel = label
                stage.aCompared = id
            case .b:
                stage.b = placed.0
                stage.bFit = placed.1
                stage.bLabel = label
                stage.bCompared = id
            }
            // A second picture where there was none: shown against A.
            if slot == .b && !hadB && stage.mode == .a {
                stage.mode = .wipe
            }
            // Beside a compared picture, the current one says what it is.
            if currentSlot == .a && stage.aLabel.isEmpty {
                stage.aLabel = String(localized: "Current")
            } else if currentSlot == .b && stage.bLabel.isEmpty {
                stage.bLabel = String(localized: "Current")
            }
        }
        stage.fitRequest += 1
    }

    /// A shows the current state again; B stays as it is.
    func returnToCurrent() {
        guard currentSlot == nil, let aside = currentAside else { return }
        withAnimation(Self.motion) {
            stage.a = aside.image
            stage.aSurface = aside.surface
            stage.aFit = aside.fit
            stage.aLabel = aside.label.isEmpty && stage.b != nil
                ? String(localized: "Current") : aside.label
        }
        currentSlot = .a
        currentAside = nil
        stage.fitRequest += 1
    }

    /// The pixel grid the compare lines up on: a side that is not fitted
    /// onto another is one; failing that, the last one found.
    private func canvasSize() -> CGSize? {
        if let a = stage.a, stage.aFit == .identity {
            return CGSize(width: a.width, height: a.height)
        }
        if let b = stage.b, stage.bFit == .identity {
            return CGSize(width: b.width, height: b.height)
        }
        return stageCanvas
    }

    /// The title bar's compare button: on, or off -- the current state
    /// back in A, B set aside for next time.
    func toggleCompare() {
        guard canToggleCompare else { return }
        if compareOn {
            endCompare()
            return
        }
        withAnimation(Self.motion) {
            compareEngaged = true
            if stage.b == nil, let p = parkedB {
                stage.b = p.image
                stage.bFit = p.fit
                stage.bLabel = p.label
                stage.bSurface = p.surface
                stage.bCompared = p.compared
                stage.aLabel = p.aLabel
                if lined(p.image) { stage.mode = .wipe }
            }
            parkedB = nil
        }
        stage.fitRequest += 1
    }

    /// The compare off, as its button turns it off: the current state
    /// back in A, B set aside for next time.
    private func endCompare() {
        guard compareOn else { return }
        if currentSlot == nil { returnToCurrent() }
        if currentSlot == .b { swapStage() }
        withAnimation(Self.motion) { parkB() }
    }

    /// The compare can be turned on or off: a still on the stage, outside
    /// the Prompt Editor (which offers no comparing).
    var canToggleCompare: Bool {
        stageVisible && !stage.showsMotion && stage.a != nil
            && !promptImmersive && !isGenerating
    }

    /// B set aside, the compare off: A alone.
    private func parkB() {
        if let b = stage.b {
            parkedB = ParkedSide(image: b, fit: stage.bFit,
                                 label: stage.bLabel,
                                 surface: stage.bSurface,
                                 compared: stage.bCompared,
                                 aLabel: stage.aLabel)
        }
        stage.b = nil
        stage.bLabel = ""
        stage.mode = .a
        if stage.aCompared == nil { stage.aLabel = "" }
        compareEngaged = false
    }

    /// B lines up with A: fitted onto it, or the same size.
    private func lined(_ b: CGImage) -> Bool {
        guard let a = stage.a else { return false }
        return stage.bFit != .identity
            || (b.width == a.width && b.height == a.height)
    }

    func swapStage() {
        var v = stage
        v.swapAB()
        withAnimation(Self.motion) { stage = v }
        currentSlot = currentSlot?.other
    }

    // MARK: - Crop and trim

    /// The Crop / Trim tab: once the stage has a picture of its own (as
    /// the Adjust tab), or a finished clip.
    var showsCrop: Bool { showsAdjust }

    /// The Trim tab: a finished clip on the stage.
    var showsTrim: Bool { clipOnStage }

    /// A finished clip is on the simple stage (not a preview).
    var clipOnStage: Bool {
        stage.video != nil && stage.clip == nil
            && stageAssetId != nil
    }

    // MARK: Keyframes (a clip)

    /// The frames a track has keys at.
    func keyFrames(_ track: KeyTrack) -> [Int] {
        switch track {
        case .adjust: clipAdjustKeys.keys.map(\.frame)
        case .place: clipCropKeys.place.keys.map(\.frame)
        case .turn: clipCropKeys.turn.keys.map(\.frame)
        }
    }

    /// The player to the track's previous (or next) key.
    func goToKey(_ track: KeyTrack, forward: Bool) {
        let f = keyFrame
        let keys = keyFrames(track)
        let to = forward ? keys.first { $0 > f } : keys.last { $0 < f }
        guard let to else { return }
        // Keys count from the layer's start; the player and the pages
        // from the composition's.
        let start = f - (clipOnStage ? videoFrame : stagePage)
        if pagedOnStage {
            goToPage(to - start)
        } else {
            seekVideo(to: to - start)
        }
    }

    /// A key here, with the values the clip shows here -- or, on a key,
    /// none (the last one stays).
    func toggleKey(_ track: KeyTrack) {
        // None out of the layer's range: it does not show there.
        guard layerOffStage == nil else { return }
        func toggle<T>(_ k: inout Keyframes<T>) {
            let f = keyFrame
            if k.index(at: f) != nil {
                if k.keys.count > 1 { k.remove(at: f) }
            } else if let v = k.value(at: f) {
                k.set(v, at: f)
            }
        }
        switch track {
        case .adjust: toggle(&clipAdjustKeys)
        case .place: toggle(&clipCropKeys.place)
        case .turn: toggle(&clipCropKeys.turn)
        }
        clipTracksChanged()
    }

    /// The player shows another frame: the panels show its values.
    func videoFrameChanged(_ f: Int) {
        videoFrame = f
        refreshClipValues()
    }

    /// `adjustments` and `crop` as the clip has them at the frame on
    /// screen.
    private func refreshClipValues() {
        guard keyedStage else { return }
        let a = clipAdjustKeys.value(at: keyFrame) ?? ImageAdjustments()
        let c = clipCropKeys.value(at: keyFrame)
        if adjustments != a { adjustments = a }
        if crop != c { crop = c }
    }

    /// A track changed: the panels and the player follow, and it is
    /// recorded on the clip a moment later.
    private func clipTracksChanged() {
        refreshClipValues()
        if clipComposed { refreshStackPlan() }
        if pagedOnStage { recomposite() }
        clipKeysPersistTask?.cancel()
        clipKeysPersistTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            self?.persistClipTracks()
        }
    }

    /// Record the clip's tracks (its "adjust" and "crop" modifiers), now.
    func persistClipTracks() {
        clipKeysPersistTask?.cancel()
        clipKeysPersistTask = nil
        guard let core, let projectId,
              let clip = pagedOnStage ? stagePicture
                  : clipOnStage ? currentClip : nil else { return }
        let rate = stageFrameRate
        let layer = activeLayer
        func same<T: Equatable>(_ a: Keyframes<T>, _ b: Keyframes<T>,
                                _ none: T) -> Bool {
            a == b || (a.isEmpty && b == Keyframes(start: none))
        }
        var changed = false
        // A flat clip is changed through its edited copy.
        var target: String?
        func into() -> String {
            if let target { return target }
            let t = composedTarget(clip.id)
            target = t
            return t
        }
        if !same(clip.adjustKeys(layer: layer), clipAdjustKeys,
                 ImageAdjustments()) {
            let r = core.setKeys(project: projectId, asset: into(),
                                 kind: "adjust", layer: layer,
                                 clipAdjustKeys.json(rate: rate) { $0.json })
            if !r.ok { note("error", r.message) }
            changed = true
        }
        let cc = clip.cropKeys(layer: layer)
        if cc != clipCropKeys
            && !(cc.isEmpty && clipCropKeys.isIdentity
                 && clipCropKeys.pad == ClipCrop.start.pad) {
            let r = core.setKeys(project: projectId, asset: into(),
                                 kind: "crop", layer: layer,
                                 clipCropKeys.json(rate: rate))
            if !r.ok { note("error", r.message) }
            changed = true
        }
        if changed { reloadAssets() }
    }

    /// The clip on the stage's look, for its player: less what a held
    /// Bypass leaves out.
    var stageClipLook: ClipLook? {
        // A sound has no picture to look any way.
        guard clipOnStage, !stageIsAudio else { return nil }
        var crop = clipCropKeys
        if bypassed.contains(.place) { crop.place = Keyframes(start: CropSpec()) }
        if bypassed.contains(.turn) { crop.turn = Keyframes(start: Turn()) }
        return ClipLook(adjust: bypassed.contains(.adjust)
                            ? Keyframes(start: ImageAdjustments())
                            : clipAdjustKeys,
                        crop: crop, rate: stageFrameRate)
    }

    /// Bypass, held (`on`) or let go: the stage shows the picture without
    /// that part of its look, or with it again.
    func setBypass(_ part: KeyTrack, _ on: Bool) {
        guard bypassed.contains(part) != on else { return }
        if on { bypassed.insert(part) } else { bypassed.remove(part) }
        // A still's adjustments are drawn into the stage's picture; its
        // crop and a clip's look follow on their own.
        if part == .adjust && !keyedStage { redrawAdjusted() }
        else if clipComposed { refreshStackPlan() }
        else if stageComposed { recomposite() }
    }

    /// There is something of `part` to leave out.
    func canBypass(_ part: KeyTrack) -> Bool {
        // The layer not on the stage: nothing of it to see without.
        guard layerOffStage == nil else { return false }
        switch part {
        case .adjust:
            return keyedStage ? !clipAdjustKeys.isIdentity
                : !adjustments.isIdentity
        case .place:
            if keyedStage { return !clipCropKeys.place.isIdentity }
            return crop.offsetX != 0 || crop.offsetY != 0
                || crop.scaleX != 1 || crop.scaleY != 1
        case .turn:
            return keyedStage ? !clipCropKeys.turn.isIdentity
                : crop.rotate != 0
        }
    }

    /// The crop as the stage shows it: less what a held Bypass leaves out.
    private var shownCrop: CropSpec {
        var c = crop
        if bypassed.contains(.place) {
            c.offsetX = 0
            c.offsetY = 0
            c.scaleX = 1
            c.scaleY = 1
        }
        if bypassed.contains(.turn) { c.rotate = 0 }
        return c
    }

    /// One adjustment, from the panel: on a still, its value; on a clip,
    /// the value at the frame on screen (Keyframes.edit).
    func setAdjustment(_ key: ImageAdjustments.Key, _ v: Double) {
        guard keyedStage else {
            adjustments[key] = v
            return
        }
        // Keys only where the layer shows.
        guard layerOffStage == nil else { return }
        clipAdjustKeys.edit(at: keyFrame) { $0[key] = v }
        clipTracksChanged()
    }

    /// `img` -- a picture, `spec` its crop -- on its canvas; `img` itself
    /// for no crop (or one that cannot be drawn).
    func cropped(_ img: CGImage, _ spec: CropSpec?) -> CGImage {
        guard let spec, !spec.isIdentity, let core,
              let p = core.cropPlacement(spec, width: img.width,
                                         height: img.height)
        else { return img }
        return p.render(img) ?? img
    }

    /// The picture the Crop panel works on, as read: its pixels are the
    /// content's.
    private var cropSubject: CGImage? {
        clipOnStage ? stage.a : adjustsBase ? stageBaseOriginal : stageOriginal
    }

    /// The size of what the selected layer's crop places: its picture or
    /// clip (a markup layer's, the stack's own frame); the bottom one's,
    /// the picture or clip on the stage.
    private var cropContent: (Int, Int)? {
        if let stack = stageStack, isPlacedLayer(activeLayer),
           let l = stack.layerStack.first(where: { $0.id == activeLayer }) {
            if let s = l.source, let a = assets.first(where: { $0.id == s }),
               let f = a.info?.frame, f.w > 0, f.h > 0 {
                return (f.w, f.h)
            }
            let o = ownFrame(of: stack)
            return (o.width, o.height)
        }
        if clipComposed, let f = currentClip?.info?.frame, f.w > 0 {
            return (f.w, f.h)
        }
        return cropSubject.map { ($0.width, $0.height) }
    }

    /// The canvas a crop places its content on, in pixels: its own, or --
    /// an upper layer's -- the stack's own frame (core compose_images).
    func cropCanvas(_ c: CropSpec) -> CGSize {
        if c.canvasWidth == 0, isPlacedLayer(activeLayer),
           let s = stageStack {
            let o = ownFrame(of: s)
            return CGSize(width: o.width, height: o.height)
        }
        return c.canvasSize
    }

    /// The stage shows the picture the Crop panel works on (and no
    /// generation's preview): what the guides go over.
    var stageCropSubjectShown: Bool {
        !isGenerating && !stage.showsMotion
            && (adjustsBase || (stageOriginal != nil && currentSlot == .a))
    }

    /// What the stage's canvas lays out for the crop: where the core puts
    /// the picture on its canvas. None with no crop to show -- and while a
    /// generation's preview has the stage.
    var stageCropPlacement: CropPlacement? {
        // A stack's crops are drawn into its composite.
        guard stageCropSubjectShown, !stageComposed,
              crop != CropSpec(),
              let a = stage.a, let core else { return nil }
        var spec = shownCrop
        if spec.contentWidth == 0 {
            spec.contentWidth = a.width
            spec.contentHeight = a.height
        }
        return core.cropPlacement(spec, width: a.width, height: a.height)
    }

    /// The crop's placement (offsets and scales) changes from the panel
    /// and the stage go through here: the first change takes the
    /// picture's size as the content's. On a clip, the placement track's
    /// value at the frame on screen (Keyframes.edit).
    private func changeCrop(_ change: (inout CropSpec) -> Void) {
        let content = cropContent
        func apply(_ c: inout CropSpec) {
            if c.contentWidth == 0, let (w, h) = content {
                c.contentWidth = w
                c.contentHeight = h
            }
            change(&c)
            let r = CropSpec.scaleRange
            c.scaleX = min(r.upperBound, max(r.lowerBound, c.scaleX))
            c.scaleY = min(r.upperBound, max(r.lowerBound, c.scaleY))
        }
        if keyedStage {
            guard layerOffStage == nil else { return }
            clipCropKeys.place.edit(at: keyFrame, apply)
            clipTracksChanged()
            return
        }
        var c = crop
        apply(&c)
        crop = c
    }

    /// The placement's text boxes: one value, relative to the frame. A
    /// zoom with X and Y locked takes the other along, their ratio kept.
    func setCropPlacement(_ field: CropField, _ v: Double) {
        changeCrop { c in
            switch field {
            case .offsetX: c.offsetX = v
            case .offsetY: c.offsetY = v
            case .scaleX:
                if cropZoomLocked, c.scaleX > 0 { c.scaleY *= v / c.scaleX }
                c.scaleX = v
            case .scaleY:
                if cropZoomLocked, c.scaleY > 0 { c.scaleX *= v / c.scaleY }
                c.scaleY = v
            }
        }
    }

    /// A jog on the Crop card: an offset moved by `d` frame widths (or
    /// heights); a zoom by a factor of e^`d` -- both, locked.
    func nudgeCropPlacement(_ field: CropField, by d: Double) {
        changeCrop { c in
            switch field {
            case .offsetX: c.offsetX = min(10, max(-10, c.offsetX + d))
            case .offsetY: c.offsetY = min(10, max(-10, c.offsetY + d))
            case .scaleX, .scaleY:
                let k = exp(d)
                if cropZoomLocked || field == .scaleX { c.scaleX *= k }
                if cropZoomLocked || field == .scaleY { c.scaleY *= k }
            }
        }
    }

    /// The rotation: a still's; on a clip, the turn track's value at the
    /// frame on screen.
    func setCropRotate(_ degrees: Double) {
        if keyedStage {
            guard layerOffStage == nil else { return }
            clipCropKeys.turn.edit(at: keyFrame) { $0.degrees = degrees }
            clipTracksChanged()
            return
        }
        changeCrop { $0.rotate = degrees }
    }

    /// The background, where the picture does not reach: a still's; a
    /// clip's, one colour for the whole clip.
    func setCropPad(_ color: CGColor) {
        let srgb = color.converted(
            to: CGColorSpace(name: CGColorSpace.sRGB)!,
            intent: .defaultIntent, options: nil) ?? color
        let c = srgb.components ?? [0, 0, 0, 1]
        let rgba = (c.count >= 4 ? Array(c.prefix(4))
            : [c[0], c[0], c[0], c.last ?? 1]).map { Double($0) }
        if keyedStage {
            clipCropKeys.pad = rgba
            clipTracksChanged()
            return
        }
        changeCrop { $0.pad = rgba }
    }

    /// The placement's reset: offsets 0, scales 1 -- a clip's whole
    /// placement track, back to one key at its first frame.
    func resetCropPlacement() {
        if keyedStage {
            clipCropKeys.place = Keyframes(start: CropSpec())
            clipTracksChanged()
            return
        }
        changeCrop { c in
            c.offsetX = 0
            c.offsetY = 0
            c.scaleX = 1
            c.scaleY = 1
        }
    }

    /// The rotation's reset: 0 -- a clip's whole turn track, back to one
    /// key at its first frame.
    func resetCropRotate() {
        if keyedStage {
            clipCropKeys.turn = Keyframes(start: Turn())
            clipTracksChanged()
            return
        }
        changeCrop { $0.rotate = 0 }
    }

    /// The placement is as the picture is (offsets 0, scales 1): on a
    /// clip, its whole track.
    var cropPlacementIsReset: Bool {
        if keyedStage {
            return clipCropKeys.place == Keyframes(start: CropSpec())
                || clipCropKeys.place.isIdentity
                    && clipCropKeys.place.keys.count == 1
        }
        return crop.offsetX == 0 && crop.offsetY == 0 && crop.scaleX == 1
            && crop.scaleY == 1
    }

    var cropRotateIsReset: Bool {
        if keyedStage {
            return clipCropKeys.turn == Keyframes(start: Turn())
        }
        return crop.rotate == 0
    }

    /// Reset: a still's crop; a clip's crop tracks, back to one key each.
    func resetCrop() {
        if keyedStage {
            clipCropKeys = ClipCrop.start
            clipTracksChanged()
            return
        }
        crop = CropSpec()
    }

    /// A picture to edit keeps its crop by file (Start sends it); the crop
    /// is recorded on the asset a moment after the last change.
    private func cropChanged() {
        if adjustsBase, let url = stageBaseURL {
            baseCrops[Self.fileKey(url)] = crop
        }
        if stageComposed && !adjustsBase { recomposite() }
        cropPersistTask?.cancel()
        cropPersistTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            self?.persistCrop()
        }
    }

    /// Record the crop on the picture (its modifier), now.
    func persistCrop() {
        cropPersistTask?.cancel()
        cropPersistTask = nil
        // A still with pages keys its crop (persistClipTracks).
        if pagedOnStage && !adjustsBase { return }
        let layer = adjustsBase ? "" : activeLayer
        guard let core, let projectId, let shown = adjustedAssetId,
              assets.first(where: { $0.id == shown })?.crop(layer: layer)
                != crop
        else { return }
        let id = writeTarget(shown)
        let r = core.setCrop(project: projectId, asset: id, layer: layer, crop)
        if !r.ok { note("error", r.message) }
        if adjustsBase { refreshStagedThumb() }
    }

    /// The clip on the stage's frame rate, exactly: a composition's own
    /// timeline's.
    var stageFrameRate: FrameRate {
        // A still's pages: one a "second" (its keys count pages).
        if pagedOnStage && !clipOnStage { return FrameRate(num: 1, den: 1) }
        // A sound is marked in milliseconds.
        if stageIsAudio { return .milliseconds }
        if let r = currentClip?.compositionRate { return r }
        return currentClip?.info?.frameRate ?? .fallback
    }

    /// What the marks count in: the selected layer's source's own frames
    /// (a sound's milliseconds; a clip's own rate).
    var trimRate: FrameRate {
        guard let c = currentClip, c.isComposition else {
            return stageFrameRate
        }
        if let r = c.time(of: activeLayer).rate { return r }
        // A composition of sound marks everything in milliseconds -- a
        // clip there is its sound.
        if c.kind == "audio" { return .milliseconds }
        guard let src = c.layers?.first(where: { $0.id == activeLayer })
                .flatMap({ source(of: $0) }) else { return stageFrameRate }
        if src.kind == "audio" { return .milliseconds }
        if let r = src.compositionRate { return r }
        return src.info?.frameRate ?? stageFrameRate
    }

    /// The selected layer's time into the panel: its marks and start, its
    /// speed and sound.
    func loadTrim(_ clip: AssetDTO) {
        let layer = activeLayer
        let t = clip.isComposition ? clip.time(of: layer) : LayerTimeDTO()
        trim = t.marks
        trimOffset = t.offset
        let speed = clip.isComposition ? clip.speedKeys(layer: layer)
                                       : Keyframes()
        layerSpeed = speed.isEmpty ? Keyframes(start: LayerSpeed()) : speed
        let sound = clip.isComposition ? clip.soundKeys(layer: layer)
                                       : Keyframes()
        layerSound = sound.isEmpty ? Keyframes(start: LayerSound()) : sound
        pitchFollowsSpeed = clip.isComposition
            && clip.pitchFollowsSpeed(layer: layer)
    }

    /// A sound plays on the stage (no picture to adjust or crop).
    var stageIsAudio: Bool { currentClip?.kind == "audio" }

    /// The clip on the stage, as an asset.
    var currentClip: AssetDTO? {
        guard stage.video != nil, let id = stageAssetId else { return nil }
        return assets.first { $0.id == id }
    }

    /// Set the mark-in (or the mark-out) where the player is -- held to
    /// the clip's frames. A mark past the other one clears that one.
    func setMark(in isIn: Bool) {
        // The player outside the clip: no frame of it to mark.
        guard layerOffStage == nil else { return }
        var at = sourceFrameAtPlayhead
        if let len = sourceLength { at = min(at, max(0, len - 1)) }
        var t = trim
        if isIn {
            t.markIn = at
            if let o = t.markOut, o < at { t.markOut = nil }
        } else {
            t.markOut = at
            if let i = t.markIn, i > at { t.markIn = nil }
        }
        trim = t
        holdPlayer(onSource: at)
    }

    /// A mark just set: the player put ON it -- the mark-in where the
    /// kept clip now begins (0:00 on a layer that starts the timeline),
    /// the mark-out where it ends -- so play goes on from that point of
    /// the source. The marks move the clip on the timeline, and the
    /// player kept its timeline time: after a mark-in it stood that much
    /// further into the source, and played from there. Kept at once (the
    /// player plays what is kept), the player put there once the stack's
    /// new plan lands -- a player made again for it resumes at its old
    /// time.
    private func holdPlayer(onSource n: Int) {
        trimSeekSource = n
        persistTrim()
        if !stackPlanning { placeOnTrimSource() }
    }

    private func placeOnTrimSource() {
        guard let n = trimSeekSource else { return }
        trimSeekSource = nil
        let last = stageClipLength.map { $0 - 1 }
        let at = timelineFrame(ofSource: n)
        sendVideo(.place(max(0, min(at, last ?? at))))
    }

    /// The selected layer starts where the player is.
    func setStartAtPlayhead() {
        trimOffset = videoFrame
    }

    /// Its start typed as a time on the COMPOSITION's timeline -- a
    /// timecode ("00:00:01:12", as the field shows it), a frame ("#36"),
    /// or seconds ("1:23.456", "83.456"): false when it cannot be read.
    @discardableResult
    func setStart(typed text: String) -> Bool {
        guard let n = stageFrameRate.parseFrame(text) else { return false }
        trimOffset = n
        return true
    }

    private func sendVideo(_ kind: VideoCommand.Kind) {
        videoCommand = VideoCommand(kind: kind,
                                    token: (videoCommand?.token ?? 0) + 1)
    }

    /// The clip (timeline, sound) on the stage at its first frame or its
    /// last: the editor's small stage's |< and >|.
    func seekStage(end: Bool) {
        seekVideo(to: end ? max(0, (stageClipLength ?? 1) - 1) : 0)
    }

    /// The small stage has its own transport under it: something that
    /// plays, or a still's pages to turn.
    var stageHasTransport: Bool {
        clipOnStage || (pagedOnStage && stagePages > 1)
    }

    /// The selected layer's clip at its first frame on the timeline, or
    /// its last (the Trim panel's |< and >|): where it lies, its marks
    /// and speed taken -- a picture's span too, the way back onto a layer
    /// the playhead has left; a clip alone, its own first and last.
    func seekClipEdge(end: Bool) {
        if let c = currentClip, c.isComposition, let k = layerClock {
            let r = stageFrameRate
            let first = max(0, Int((k.start * r.fps - 1e-6).rounded(.up)))
            // A picture running on to the timeline's end: its end.
            let last = k.end.isFinite
                ? max(first, Int((k.end * r.fps - 1e-6).rounded(.up)) - 1)
                : max(first, (stageClipLength ?? 1) - 1)
            seekVideo(to: end ? last : first)
        } else {
            seekVideo(to: end ? max(0, (stageClipLength ?? 1) - 1) : 0)
        }
    }

    /// ← and → step the clip in the Trim panel (⇧ ten steps; ⌥ a sound's
    /// millisecond), its sound heard at each: handled -- true -- or left
    /// to go on (text, a list, another window keep their arrows).
    func trimKey(_ e: NSEvent) -> Bool {
        // The timeline steps too: a clip's frames, a still's pages.
        let timeline = timelineOpen && (clipOnStage || pagedOnStage)
        guard (openPanel == .trim && clipOnStage) || timeline,
              let w = e.window,
              !(w is NSPanel), editorWindow == nil || w === editorWindow
        else { return false }
        let fr = w.firstResponder
        if fr is NSText || fr is NSTableView || fr is NSOutlineView
            || fr is NSCollectionView { return false }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
            .subtracting([.numericPad, .function, .capsLock])
        guard mods.subtracting([.shift, .option]).isEmpty,
              e.keyCode == 123 || e.keyCode == 124 else { return false }
        let n = mods.contains(.shift) ? 10 : 1
        timelineStep(e.keyCode == 123 ? -n : n)
        return true
    }

    /// SPACE plays or pauses what plays on the stage -- a clip, a
    /// timeline, a sound -- as in any player: from the start again when
    /// it stands at the end. Wherever the keyboard is in the window but
    /// text (the prompt's space is a space), a list or a control (a
    /// focused button's space presses it); held, once. Before the
    /// player's own (AVKit takes Space when it has the keyboard), so the
    /// two never both toggle.
    func playerKey(_ e: NSEvent) -> Bool {
        guard e.keyCode == 49, clipOnStage, fleetServing == nil,
              let w = e.window, !(w is NSPanel),
              editorWindow == nil || w === editorWindow else { return false }
        let mods = e.modifierFlags.intersection(.deviceIndependentFlagsMask)
            .subtracting([.numericPad, .function, .capsLock])
        guard mods.isEmpty else { return false }
        let fr = w.firstResponder
        if fr is NSText || fr is NSTableView || fr is NSOutlineView
            || fr is NSCollectionView || fr is NSControl { return false }
        if !e.isARepeat {
            playVideo(rate: videoRate == 0 ? 1 : 0)
        }
        return true
    }

    /// Show frame `n` of the clip on the stage, exactly (paused): one it
    /// has.
    func seekVideo(to n: Int) {
        let last = stageClipLength.map { $0 - 1 }
        sendVideo(.seek(max(0, min(n, last ?? n))))
    }

    /// The frames the clip on the stage runs: its timeline's (Information
    /// › Canvas › Length), else its own. (Its own alone held the player
    /// off the frames a longer timeline adds.)
    private var stageClipLength: Int? {
        guard let c = currentClip else { return nil }
        // A composition's: its timeline's, in its frames.
        if c.isComposition, let n = c.length { return c.timeline ?? n }
        // A sound's, in milliseconds: the units its marks count in.
        if stageIsAudio {
            return c.info?.seconds.map {
                Int(($0 * stageFrameRate.fps).rounded())
            }
        }
        return c.timeline ?? c.info?.frames
    }

    /// The selected layer's source's length, in what its marks count.
    var sourceLength: Int? {
        guard let c = currentClip else { return nil }
        guard c.isComposition,
              let src = c.layers?.first(where: { $0.id == activeLayer })
                  .flatMap({ source(of: $0) }) else { return stageClipLength }
        if let n = src.length, src.isComposition { return n }
        return src.info?.seconds.map { Int(($0 * trimRate.fps).rounded()) }
    }

    /// The Trim panel's transport: play at 1× forward or backward, pause,
    /// step a frame, and fast-forward / fast-rewind -- 2×, then 4× and 8×
    /// on each press.
    func playVideo(rate: Float) {
        sendVideo(.play(rate))
    }

    /// A frame on or back; a sound steps 10 ms, and 1 ms with ⌥ held --
    /// its marks are milliseconds.
    func stepVideo(_ frames: Int) {
        guard stageIsAudio else {
            sendVideo(.step(frames))
            return
        }
        let fine = NSEvent.modifierFlags.contains(.option)
        sendVideo(.step(frames * (fine ? 1 : 10)))
    }

    /// A mark typed as a time ("1:23.456", "83.456", "00:01:23.456"):
    /// set where it says -- a sound's marks are milliseconds. False when
    /// it cannot be read.
    @discardableResult
    func setMark(in isIn: Bool, typed text: String) -> Bool {
        guard let seconds = FrameRate.parseTime(text) else { return false }
        var n = trimRate.frame(at: seconds)
        if let len = sourceLength { n = min(n, max(0, len - 1)) }
        var t = trim
        if isIn {
            t.markIn = n
            if let o = t.markOut, o < n { t.markOut = nil }
        } else {
            t.markOut = n
            if let i = t.markIn, i > n { t.markIn = nil }
        }
        trim = t
        holdPlayer(onSource: n)
        return true
    }

    func fastForward() {
        playVideo(rate: videoRate >= 2 ? min(videoRate * 2, 8) : 2)
    }

    func fastRewind() {
        playVideo(rate: videoRate <= -2 ? max(videoRate * 2, -8) : -2)
    }

    func resetTrim() {
        trim = TrimSpec()
        trimOffset = 0
    }

    /// The frames the marks keep, when either is set.
    var trimmedFrames: Int? {
        guard !trim.isIdentity else { return nil }
        let last = sourceLength.map { $0 - 1 } ?? Int.max
        let n = min(trim.markOut ?? last, last) - (trim.markIn ?? 0) + 1
        return n > 0 && n < Int.max / 2 ? n : nil
    }

    /// "48 frames · 2.0 s"; a sound's "12.345 s".
    func trimLengthText(_ n: Int) -> String {
        let r = trimRate
        let secs = String(format: r.isMilliseconds ? "%.3f" : "%.1f",
                          Double(n) / r.fps) + " s"
        if r.isMilliseconds { return secs }
        let frames = String(n)
        return String(localized: "\(frames) frames · \(secs)")
    }

    private func scheduleTrimPersist() {
        trimPersistTask?.cancel()
        trimPersistTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(300))
            guard !Task.isCancelled else { return }
            self?.persistTrim()
        }
    }

    /// Record the selected layer's time -- its marks and its start -- on
    /// the composition on the stage (a flat clip or sound there is first
    /// wrapped in its edited copy), now.
    func persistTrim() {
        trimPersistTask?.cancel()
        trimPersistTask = nil
        guard let core, let projectId, let clip = currentClip else { return }
        let layer = activeLayer
        let comp = clip.isComposition
        let was = comp ? clip.time(of: layer) : LayerTimeDTO()
        let timeChanged = was.marks != trim || was.offset != trimOffset
        func same<T: Equatable>(_ a: Keyframes<T>, _ b: Keyframes<T>,
                                _ none: T) -> Bool {
            a == b || (a.isEmpty && b == Keyframes(start: none))
        }
        let speedChanged = !same(comp ? clip.speedKeys(layer: layer)
                                      : Keyframes(),
                                 layerSpeed, LayerSpeed())
        let soundChanged = !same(comp ? clip.soundKeys(layer: layer)
                                      : Keyframes(),
                                 layerSound, LayerSound())
            || (comp && clip.pitchFollowsSpeed(layer: layer))
                != pitchFollowsSpeed
        guard timeChanged || speedChanged || soundChanged else { return }
        let id = composedTarget(clip.id)
        if timeChanged {
            let rate = trimRate
            var t = was
            t.in = trim.markIn ?? -1
            t.out = trim.markOut ?? -1
            t.rateNum = trim.isIdentity ? 0 : rate.num
            t.rateDen = trim.isIdentity ? 1 : rate.den
            t.offset = trimOffset
            let r = core.setLayerTime(project: projectId, asset: id,
                                      layer: layer, t.json)
            if !r.ok { flash(r.message) }
        }
        // Keyed in the composition's frames, from the layer's start.
        let rate = stageFrameRate
        if speedChanged {
            let keys = layerSpeed.json(rate: rate) { ["rate": $0.rate] }
            let r = core.layerOp(project: projectId, asset: id, "speed",
                                 layer: layer, extra: ["keys": keys])
            if !r.ok { flash(r.message) }
        }
        if soundChanged {
            let keys = layerSound.json(rate: rate) {
                ["volume": $0.volume, "pitch": $0.pitch]
            }
            let r = core.layerOp(project: projectId, asset: id, "sound",
                                 layer: layer,
                                 extra: ["keys": keys,
                                         "follow_speed": pitchFollowsSpeed])
            if !r.ok { flash(r.message) }
        }
        reloadAssets()
        refreshStackPlan()
    }

    // MARK: - Adjustments

    /// There is a picture on the stage to adjust: a finished result, or
    /// the picture about to be edited.
    var canAdjust: Bool {
        (stageOriginal != nil || adjustsBase || clipOnStage) && !isGenerating
    }

    /// The Adjust panel works on the picture to edit, before the model
    /// sees it (rather than on a result).
    var adjustsBase: Bool { stageBaseURL != nil && stageBaseOriginal != nil }

    /// The Adjust tab is there once the stage has a picture of its own:
    /// a result, or the picture to edit -- and stays (locked) while a
    /// generation that came from one runs.
    var showsAdjust: Bool {
        stageOriginal != nil || adjustsBase
            || (clipOnStage && !stageIsAudio)
            || (isGenerating && watchedTask?.base != nil)
    }

    /// What the stage shows can be dragged out: a finished result on
    /// file, or the picture dropped there to edit.
    var canDragStage: Bool {
        stageVisible && !isGenerating
            && (currentAsset?.url != nil || stageBaseURL != nil)
            // The whole picture: with a layer hidden, there is none.
            && !(stageLayered && stageHasHiddenLayer)
    }

    /// The file a drag from the stage carries: the result (adjusted, a
    /// PNG of what is shown; a stack flattened), else the picture dropped
    /// there.
    func stageDragFile() -> URL? {
        let url = stageComposed && !adjustsBase ? flattenedFile()
            : shownFile() ?? stageBaseURL
        lastStageDrag = url?.standardizedFileURL
        return url
    }

    /// The file a drag from the stage carried last: dropped in the prompt
    /// -- on its text or its row -- the stage is CAPTURED instead
    /// (addStageToPrompt), its edits and what it shows an asset the
    /// generation records, never a loose copy of its pixels.
    func isStageDrag(_ url: URL) -> Bool {
        lastStageDrag != nil && url.standardizedFileURL == lastStageDrag
    }

    /// Reset: a still's adjustments; a clip's whole track, back to one
    /// key at its first frame.
    func resetAdjustments() {
        if keyedStage {
            clipAdjustKeys = Keyframes(start: ImageAdjustments())
            clipTracksChanged()
            return
        }
        adjustments = ImageAdjustments()
    }

    /// The asset the Adjust panel is working on: the picture to edit, or
    /// the result on the stage.
    private var adjustedAssetId: String? {
        if adjustsBase, let url = stageBaseURL {
            if let item = stagedItem { return assetId(of: item) }
            return asset(forFile: url)?.id
        }
        return stageOriginal != nil ? stageAssetId : nil
    }

    /// The row item the stage shows to adjust: the picture to edit, or a
    /// picture clicked in the row.
    var stagedItem: PromptAttachment? {
        stagedItemId.flatMap { id in promptAttachments.first { $0.id == id } }
    }

    /// Where a change to the staged row item is written: its own MODIFIED
    /// copy (core derive_modified) -- made the first time it is changed,
    /// the original left as it was; changed in place after. nil: written
    /// to `fallback` (no row item, or not imported yet).
    private func writeTarget(_ fallback: String) -> String {
        guard adjustsBase, let id = stagedItemId,
              let i = promptAttachments.firstIndex(where: { $0.id == id })
        else { return composedTarget(fallback) }
        let item = promptAttachments[i]
        if item.ownCopy, let a = item.asset { return a }
        guard let core, let projectId, let src = assetId(of: item),
              let source = assets.first(where: { $0.id == src }) else {
            return fallback
        }
        let r = core.assetOp(project: projectId, "modify", [
            "asset": src,
            "name": String(localized: "\(source.name), edited"),
        ])
        guard r.ok, let made = r["asset"] as? String else {
            note("error", r.message)
            return fallback
        }
        promptAttachments[i].asset = made
        promptAttachments[i].ownCopy = true
        reloadAssets()
        return made
    }

    /// The staged row item's thumbnail, as it now looks (its copy's look).
    private func refreshStagedThumb() {
        guard let core, let projectId, let item = stagedItem,
              item.ownCopy, let a = item.asset else { return }
        let id = item.id
        Task { [weak self] in
            let cg = await Task.detached(priority: .utility) {
                core.thumbnail(project: projectId, asset: a, maxPixels: 240)
                    .flatMap { AppModel.loadImage($0) }
            }.value
            guard let self, let cg else { return }
            self.referenceThumbs[id] = cg
        }
    }

    /// Record the panel's values on the image (its modifier), now. A
    /// picture to edit still importing is written when it lands
    /// (reloadAssets).
    func persistAdjustments() {
        persistTask?.cancel()
        persistTask = nil
        // A still with pages keys its look (persistClipTracks).
        if pagedOnStage && !adjustsBase { return }
        let layer = adjustsBase ? "" : activeLayer
        guard let core, let projectId, let shown = adjustedAssetId,
              assets.first(where: { $0.id == shown })?.adjustments(layer: layer)
                != adjustments else { return }
        // A picture of the prompt: through its own modified copy.
        let id = writeTarget(shown)
        let r = core.setAdjustments(project: projectId, asset: id,
                                    layer: layer, adjustments)
        if !r.ok { note("error", r.message) }
        if adjustsBase { refreshStagedThumb() }
    }

    /// Re-render what the stage shows -- the result, or the picture to
    /// edit -- with the current adjustments, off the main thread; a newer
    /// change cancels an older render. The picture to edit keeps its own.
    private func renderAdjusted() {
        // What is adjusted is the current state: shown again if history
        // states held both sides.
        if currentSlot == nil { returnToCurrent() }
        if adjustsBase, let url = stageBaseURL {
            baseAdjustments[Self.fileKey(url)] = adjustments
        }
        persistTask?.cancel()
        persistTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            self?.persistAdjustments()
        }
        redrawAdjusted()
    }

    /// The stage's picture drawn again with the adjustments it shows --
    /// none while Bypass is held.
    private func redrawAdjusted() {
        adjustTask?.cancel()
        // A picture with a stack is drawn whole: every layer's look.
        if stageComposed && !adjustsBase {
            recomposite()
            return
        }
        let onBase = adjustsBase
        guard let original = onBase ? stageBaseOriginal : stageOriginal,
              let core else { return }
        let chain = core.adjustmentChain(bypassed.contains(.adjust)
                                         ? ImageAdjustments() : adjustments)
        // Nothing to draw: the picture as read.
        if chain.isEmpty {
            showCurrent(original)
            return
        }
        adjustTask = Task { [weak self] in
            // Drawn on the GPU into a surface the canvas shows as it is.
            let out = await Task.detached(priority: .userInitiated) {
                ImageAdjustments.render(chain, to: original)
            }.value
            guard !Task.isCancelled, let self, let out else { return }
            if onBase {
                guard self.stageBaseOriginal === original else { return }
            } else {
                guard self.stageOriginal === original else { return }
            }
            self.showCurrent(out.image, surface: out.surface)
        }
    }

    func setStageMode(_ m: CompareMode) {
        withAnimation(Self.motion) { stage.mode = m }
        stage.fitRequest += 1
    }

    func toggleStageWipeLine() {
        withAnimation(Self.motion) { stage.wipeLine.toggle() }
    }

    // MARK: - The project as a document (DESIGN §5b)

    /// What the core says of the project as a document: unsaved changes,
    /// whether it has a file yet, what Undo and Redo would do (a command's
    /// kind).
    struct DocumentState: Equatable {
        var dirty = false
        var untitled = false
        var undo: String?
        var redo: String?
    }
    var document = DocumentState()
    /// "Don't Save" chosen for the project going: its working copy, with
    /// its unsaved changes, goes with it.
    @ObservationIgnored private var discardOnClose = false

    /// project.changed: the state the core keeps.
    func documentChanged(_ p: [String: Any]) {
        guard let id = p["project"] as? String, id == projectId else { return }
        let next = DocumentState(
            dirty: p["dirty"] as? Bool ?? false,
            untitled: p["untitled"] as? Bool ?? false,
            undo: (p["undo"] as? [String: Any])?["kind"] as? String,
            redo: (p["redo"] as? [String: Any])?["kind"] as? String)
        if next != document { document = next }
    }

    /// The state asked for (after an open, a save).
    func refreshDocument() {
        guard let core, let projectId else {
            document = DocumentState()
            return
        }
        let r = core.projectState(projectId)
        if r.ok, let st = r["state"] as? [String: Any] {
            documentChanged(st)
        }
    }

    /// What a panel has still to write: written, so it is part of what is
    /// saved -- or undone.
    func flushPanels() {
        persistAdjustments()
        persistCrop()
        if clipOnStage {
            persistClipTracks()
            persistTrim()
        } else if pagedOnStage {
            persistClipTracks()
        }
    }

    /// The project's history, a step back (or forward).
    func undoProject() { stepHistory(forward: false) }
    func redoProject() { stepHistory(forward: true) }

    private func stepHistory(forward: Bool) {
        guard let core, let projectId else { return }
        guard !historyBusy else {
            flash(String(localized: "Wait for the work in progress to finish."))
            return
        }
        flushPanels()
        let r = forward ? core.redo(project: projectId)
                        : core.undo(project: projectId)
        guard r.ok else {
            flash(r.message)
            return
        }
        syncAfterHistory()
    }

    /// The stage and the panels as the project now is -- after an undo, a
    /// redo, a revert, or a job that changed it (an upscale's swap).
    /// Nothing the panels held before is written back, now or by a save
    /// still waiting.
    func syncAfterHistory() {
        persistTask?.cancel()
        persistTask = nil
        cropPersistTask?.cancel()
        cropPersistTask = nil
        clipKeysPersistTask?.cancel()
        clipKeysPersistTask = nil
        trimPersistTask?.cancel()
        trimPersistTask = nil
        let before = stageAssetId.flatMap { id in
            assets.first { $0.id == id }
        }
        reloadAssets()
        refreshDocument()
        guard let id = stageAssetId,
              let a = assets.first(where: { $0.id == id }) else {
            // What was on the stage is gone: the project's work, or
            // nothing.
            if let view = projectViews.first {
                putOnStage(view)
            } else {
                clearStage()
            }
            return
        }
        // Its file is another (a new take undone): shown again whole.
        if a.url != before?.url || a.kind != before?.kind {
            putOnStage(a)
            return
        }
        // A clip's -- or a composition of sound's -- layers, tracks and
        // times as they now are: the panels hold them again (a mark or a
        // start held from before would be written back over the undo),
        // the player its new plan.
        if clipOnStage, a.kind == "video" || a.isTimeline {
            loadClipTracks(a, layer: activeLayer)
            if a.isComposition { loadTrim(a) }
            refreshStackPlan()
            return
        }
        if a.isPaged || before?.isPaged == true {
            // Its pages as they now are: the page shown kept if it is
            // still there, the tracks (or, back to one page, the values).
            stagePage = min(stagePage, a.pageCount - 1)
            if a.isPaged {
                loadClipTracks(a, layer: activeLayer)
            } else {
                adjustments = a.adjustments(layer: activeLayer)
                crop = a.crop(layer: activeLayer)
            }
            recomposite()
            return
        }
        withAnimation(Self.motion) {
            adjustments = a.adjustments(layer: activeLayer)
            crop = a.crop(layer: activeLayer)
        }
        if stageComposed {
            recomposite()
        } else {
            redrawAdjusted()
        }
    }

    /// Save (⌘S): to the project's file -- an untitled one is named first.
    func saveDocument() {
        guard let core, let projectId else { return }
        if document.untitled || isAnonymous {
            saveDocumentAs()
            return
        }
        flushPanels()
        noteWindowSize()
        let r = core.saveProject(projectId)
        if !r.ok { flash(r.message) }
        refreshDocument()
    }

    /// Save As… (⇧⌘S): a new file, the project's from now on.
    func saveDocumentAs() {
        let panel = NSSavePanel()
        panel.title = String(localized: "Save Project")
        panel.nameFieldStringValue = windowTitle + ".valtz"
        panel.allowedContentTypes = [.valtzProject]
        // Where projects were last opened or saved.
        panel.directoryURL = panelFolder(.project)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        rememberPanel(.project, chose: url)
        _ = save(as: url)
    }

    /// Saved at `url` (also the snapshot hook's way).
    @discardableResult
    func save(as url: URL) -> Bool {
        guard let core, let projectId else { return false }
        flushPanels()
        noteWindowSize()
        let r = core.saveProjectAs(projectId, path: url.path)
        guard r.ok else {
            flash(r.message)
            return false
        }
        projectPath = url.path
        projectName = url.deletingPathExtension().lastPathComponent
        isAnonymous = false
        refreshDocument()
        return true
    }

    /// Revert to Saved: the project as last saved; what came since, and
    /// its history, gone.
    func revertDocument(confirm: Bool = true) {
        guard let core, let projectId, !document.untitled, !isAnonymous
        else { return }
        if confirm {
            let alert = NSAlert()
            alert.messageText = String(localized: "Revert to the saved version of “\(projectName)”?")
            alert.informativeText = String(localized: "Your changes since it was saved will be lost, and can't be undone.")
            alert.addButton(withTitle: String(localized: "Revert"))
            alert.addButton(withTitle: String(localized: "Cancel"))
            guard alert.runModal() == .alertFirstButtonReturn else { return }
        }
        guard !isBusy else {
            flash(String(localized: "Wait for the work in progress to finish."))
            return
        }
        persistTask?.cancel()
        cropPersistTask?.cancel()
        let r = core.revertProject(projectId)
        guard r.ok else {
            flash(r.message)
            return
        }
        syncAfterHistory()
    }

    /// Before the project goes (another opened, a new one, a quit): its
    /// unsaved changes saved, or thrown away -- or the person cancels
    /// (false). The ANONYMOUS session is deleted when it goes, so one
    /// holding anything -- an asset, a change -- asks too: saved (Save
    /// As: named, it is kept), deleted, or kept open. It went without a
    /// word before, and a session's work with it.
    func confirmClose() -> Bool {
        discardOnClose = false
        guard projectId != nil else { return true }
        if isAnonymous {
            return confirmCloseAnonymous(
                String(localized: "Your work in it will be lost: an anonymous project is deleted when it closes, with everything in it — its results, imports and history — unless you save it."))
        }
        guard document.dirty else { return true }
        // A scripted run keeps what it did, as before files were saved.
        if ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] != nil {
            saveDocument()
            return true
        }
        let alert = NSAlert()
        alert.messageText = String(localized: "Do you want to save the changes made to “\(projectName)”?")
        alert.informativeText = String(localized: "Your changes will be lost if you don't save them.")
        alert.addButton(withTitle: String(localized: "Save"))
        alert.addButton(withTitle: String(localized: "Don't Save"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            saveDocument()
            return !document.dirty
        case .alertSecondButtonReturn:
            discardOnClose = true
            return true
        default:
            return false
        }
    }

    /// The anonymous session asked about before it goes, when it holds
    /// anything: Save… (Save As; cancelled there, nothing closes), Don't
    /// Save (deleted), Cancel. `info` says what goes. A scripted run takes
    /// VALTZ_SNAPSHOT_CLOSE's answer -- "save:<path>", "discard",
    /// "cancel" -- and without one lets it go, as before.
    func confirmCloseAnonymous(_ info: String) -> Bool {
        guard isAnonymous, !assets.isEmpty || document.dirty else {
            return true
        }
        let title = String(localized: "Do you want to save the anonymous project “\(windowTitle)”?")
        let env = ProcessInfo.processInfo.environment
        if env["VALTZ_SNAPSHOT"] != nil {
            let answer = snapshotCloseAnswer
                ?? env["VALTZ_SNAPSHOT_CLOSE"] ?? "discard"
            FileHandle.standardError.write(Data(
                "snapshot: close-alert title=\(title) answer=\(answer) assets=\(assets.count) dirty=\(document.dirty)\n".utf8))
            if answer.hasPrefix("save:") {
                return save(as: URL(fileURLWithPath:
                    String(answer.dropFirst(5))))
            }
            return answer != "cancel"
        }
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = info
        alert.addButton(withTitle: String(localized: "Save…"))
        let dont = alert.addButton(withTitle: String(localized: "Don't Save"))
        dont.hasDestructiveAction = true
        alert.addButton(withTitle: String(localized: "Cancel"))
        switch alert.runModal() {
        case .alertFirstButtonReturn:
            saveDocumentAs()
            // Named now: it is kept. The save panel cancelled: still
            // anonymous, and nothing closes.
            return !isAnonymous
        case .alertSecondButtonReturn:
            return true
        default:
            return false
        }
    }

    /// The project closed in the core: the anonymous session discarded
    /// and deleted; a named one closed -- its working copy kept when it
    /// has changes not saved (resumed at the next open), unless "Don't
    /// Save".
    private func retireProject(_ id: String, path: String,
                               anonymous: Bool) {
        guard let core else { return }
        if !anonymous && !discardOnClose { keepWindowSize() }
        if anonymous {
            _ = core.discardProject(id)
            try? FileManager.default.removeItem(atPath: path)
        } else if discardOnClose {
            _ = core.discardProject(id)
        } else {
            _ = core.closeProject(id)
        }
        discardOnClose = false
    }

    // MARK: The window's size, per project

    /// The editor's window (RootView's WindowReader): a project opened
    /// before it was there is sized once it is.
    @ObservationIgnored weak var editorWindow: NSWindow? {
        didSet {
            guard let w = editorWindow else { return }
            editorWasShown = true
            // Its close asks about the project while it is still there.
            if closeGuard?.window !== w {
                closeGuard = WindowCloseGuard(window: w) { [weak self] in
                    self?.confirmWindowClose() ?? true
                }
            }
            // Auto-update starts once the editor is there: its prompt over
            // the editor, never before it.
            updater.start()
            if let size = pendingWindowSize {
                pendingWindowSize = nil
                applyWindowSize(size)
            }
        }
    }
    /// The editor's window has been there: closing the last window quits
    /// Valtz from now on (AppDelegate).
    @ObservationIgnored private(set) var editorWasShown = false
    @ObservationIgnored private var closeGuard: WindowCloseGuard?
    /// The project already asked about as the editor's window closed:
    /// the quit that follows does not ask again.
    @ObservationIgnored private(set) var closeConfirmed = false
    /// A scripted run's answer to the next close alert, over
    /// VALTZ_SNAPSHOT_CLOSE (VALTZ_SNAPSHOT_CLOSE_WINDOW's).
    @ObservationIgnored var snapshotCloseAnswer: String?

    /// The editor's window asked to close -- its close button, ⌘W: the
    /// project asked about FIRST, while the window is still there, so
    /// Cancel keeps it (before, the window went, and Cancel at the quit
    /// that followed left Valtz running with no window, asking again).
    /// Agreed, what a quit does is done; Valtz then quits as it goes.
    func confirmWindowClose() -> Bool {
        guard confirmClose() else { return false }
        closeForQuit()
        closeConfirmed = true
        return true
    }
    @ObservationIgnored private var pendingWindowSize: CGSize?

    /// The window's size into the project -- saved with it: Save, Save
    /// As, or alone when it goes clean (keepWindowSize).
    private func noteWindowSize() {
        guard let core, let projectId, let w = editorWindow else { return }
        _ = core.setViewState(projectId, ["window": [
            "width": Int(w.frame.width.rounded()),
            "height": Int(w.frame.height.rounded()),
        ]])
    }

    /// A named project going -- closed, the app quitting -- with nothing
    /// to save: its window's size kept, written and saved alone when it
    /// changed. (With changes, Save wrote it; let go, it goes with them.)
    private func keepWindowSize() {
        guard let core, let projectId, !isAnonymous, !document.untitled,
              !document.dirty else { return }
        noteWindowSize()
        refreshDocument()
        if document.dirty {
            _ = core.saveProject(projectId)
            refreshDocument()
        }
    }

    /// The window at the size the project was last looked at.
    private func restoreWindowSize() {
        guard let core, let projectId else { return }
        let r = core.viewState(projectId)
        guard let win = (r["view"] as? [String: Any])?["window"]
                as? [String: Any],
              let w = (win["width"] as? NSNumber)?.doubleValue,
              let h = (win["height"] as? NSNumber)?.doubleValue,
              w > 0, h > 0 else { return }
        let size = CGSize(width: w, height: h)
        if editorWindow == nil {
            pendingWindowSize = size
        } else {
            applyWindowSize(size)
        }
    }

    /// Its top left where it is; within the screen, and no smaller than
    /// the window may be.
    private func applyWindowSize(_ size: CGSize) {
        guard let win = editorWindow else { return }
        var f = win.frame
        let room = win.screen?.visibleFrame ?? f
        let w = min(max(size.width, win.minSize.width), room.width)
        let h = min(max(size.height, win.minSize.height), room.height)
        guard abs(w - f.width) > 0.5 || abs(h - f.height) > 0.5 else {
            return
        }
        f.origin.y += f.height - h
        f.size = CGSize(width: w, height: h)
        // Kept on the screen.
        f.origin.x = min(max(f.origin.x, room.minX), room.maxX - w)
        f.origin.y = min(max(f.origin.y, room.minY), room.maxY - h)
        win.setFrame(f, display: true, animate: win.isVisible)
    }

    /// Before the app quits: "Don't Save" carried out.
    func closeForQuit() {
        guard let projectId else { return }
        if !discardOnClose { keepWindowSize() }
        if discardOnClose && !isAnonymous {
            _ = core?.discardProject(projectId)
        }
    }

    /// The text being edited, when one has the focus: ⌘Z undoes its typing
    /// first; with none left (or no text focused), the project's history.
    struct TextUndo: Equatable {
        var canUndo = false
        var canRedo = false
        var undoTitle = ""
        var redoTitle = ""
    }
    var textUndo: TextUndo?

    @ObservationIgnored private var textWatch: [Any] = []

    /// Watches the focus for the Edit menu: the text being edited -- its
    /// undo, after every event -- and a click away from it, which ends its
    /// editing as in any Mac app (⌘Z is the project's again).
    func watchTextFocus() {
        guard textWatch.isEmpty else { return }
        textWatch.append(NotificationCenter.default.addObserver(
            forName: NSWindow.didUpdateNotification, object: nil,
            queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated { self?.refreshTextUndo() }
        })
        if let m = NSEvent.addLocalMonitorForEvents(
            matching: .leftMouseDown, handler: { [weak self] event in
                MainActor.assumeIsolated {
                    Self.endTextEditing(for: event)
                    self?.endViewingIfOutside(event)
                }
                return event
            }) {
            textWatch.append(m)
        }
        // The brush's [ ] and ⇧[ ⇧], wherever the keyboard is in the
        // window but text; the Trim panel's ← →; Space, play / pause.
        if let m = NSEvent.addLocalMonitorForEvents(
            matching: .keyDown, handler: { [weak self] event in
                let handled = MainActor.assumeIsolated {
                    guard let self else { return false }
                    return self.brushKey(event) || self.trimKey(event)
                        || self.playerKey(event)
                }
                return handled ? nil : event
            }) {
            textWatch.append(m)
        }
    }

    /// The click is not on text: the text being edited lets go of the
    /// focus.
    static func endTextEditing(for event: NSEvent) {
        guard let w = event.window, w.firstResponder is NSTextView,
              let root = w.contentView?.superview,
              let hit = root.hitTest(event.locationInWindow) else { return }
        var v: NSView? = hit
        while let x = v {
            if x is NSText || x is NSTextField { return }
            if let clip = x as? NSClipView, clip.documentView is NSText {
                return
            }
            if let sv = x as? NSScrollView, sv.documentView is NSText {
                return
            }
            v = x.superview
        }
        w.makeFirstResponder(nil)
    }

    func refreshTextUndo() {
        let window = NSApp.keyWindow ?? NSApp.mainWindow
            ?? NSApp.windows.first { $0.isVisible }
        let tv = window?.firstResponder as? NSTextView
        let next = tv.map { t -> TextUndo in
            let u = t.undoManager
            return TextUndo(canUndo: u?.canUndo ?? false,
                            canRedo: u?.canRedo ?? false,
                            undoTitle: u?.undoMenuItemTitle ?? "",
                            redoTitle: u?.redoMenuItemTitle ?? "")
        }
        if next != textUndo { textUndo = next }
    }

    // MARK: - Anonymous session

    /// Where anonymous sessions live, and the files made to share or drag
    /// an adjusted image: both removed at exit (and at the next launch,
    /// after a crash). Sessions are kept in Valtz's own support folder,
    /// NOT the temporary directory: macOS deletes what is there and has
    /// not been read for some days, and a session's media nobody looks at
    /// -- a stale generation -- went missing under a session left open.
    nonisolated static let sessionRoot = FileManager.default
        .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent(Bundle.main.bundleIdentifier
                                    ?? "com.tgous.valtz", isDirectory: true)
        .appendingPathComponent("Anonymous", isDirectory: true)
    /// This process's own: another Valtz running beside it -- a test run,
    /// a second copy -- has its own, and neither clears the other's.
    nonisolated static let shareRoot = FileManager.default
        .temporaryDirectory.appendingPathComponent("Valtz Share",
                                                   isDirectory: true)
        .appendingPathComponent(String(getpid()), isDirectory: true)

    /// A session's OWNER LOCK, beside its folder: held (flock) by the Valtz
    /// that runs it, for as long as it runs. Clearing what a crash left
    /// takes only sessions whose lock it can get -- whose Valtz is gone.
    /// (Every launch and quit cleared the whole folder before 2026-10-05:
    /// a Valtz started beside another deleted the other's open session,
    /// its media going missing under it.)
    nonisolated static func lockFile(_ dir: URL) -> URL {
        dir.appendingPathExtension("lock")
    }

    /// Sessions -- and share folders -- that no running Valtz holds: a
    /// crash's, a relaunch's predecessor's. Another running Valtz's are
    /// left alone.
    nonisolated static func removeOrphanSessions() {
        let fm = FileManager.default
        let items = (try? fm.contentsOfDirectory(
            at: sessionRoot, includingPropertiesForKeys: nil)) ?? []
        for url in items where url.pathExtension != "lock" {
            let lock = lockFile(url)
            let fd = open(lock.path, O_RDWR)
            guard fd >= 0 else {
                // No owner ever took it (a lock comes first): a crash's.
                try? fm.removeItem(at: url)
                continue
            }
            if flock(fd, LOCK_EX | LOCK_NB) == 0 {
                try? fm.removeItem(at: url)
                try? fm.removeItem(at: lock)
            }
            close(fd)
        }
        // Share folders of processes no longer there.
        let shares = shareRoot.deletingLastPathComponent()
        for url in (try? fm.contentsOfDirectory(
            at: shares, includingPropertiesForKeys: nil)) ?? [] {
            guard let pid = Int32(url.lastPathComponent),
                  pid != getpid() else { continue }
            if kill(pid, 0) != 0 && errno == ESRCH {
                try? fm.removeItem(at: url)
            }
        }
    }

    /// This process's own session folder, its lock, its share folder --
    /// at quit and Start Over.
    private func removeOwnSession() {
        let fm = FileManager.default
        if isAnonymous, !projectPath.isEmpty {
            let dir = URL(fileURLWithPath: projectPath)
            try? fm.removeItem(at: dir)
            try? fm.removeItem(at: Self.lockFile(dir))
        }
        if sessionLock >= 0 {
            close(sessionLock)
            sessionLock = -1
        }
        try? fm.removeItem(at: Self.shareRoot)
    }

    nonisolated static func randomAnonymousName() -> String {
        "valtz_anonymous_" + (animals.randomElement() ?? "otter")
    }

    /// A fresh anonymous project: a package in the temporary directory,
    /// ephemeral in the core (its thumbnails stay inside it too).
    private func startAnonymousSession() {
        guard let core else { return }
        let fm = FileManager.default
        try? fm.createDirectory(at: Self.sessionRoot,
                                withIntermediateDirectories: true)
        // An untitled working copy (DESIGN §5b): nothing of it outside the
        // temporary folder until Save As names it.
        let dir = Self.sessionRoot.appendingPathComponent(UUID().uuidString)
        // Its owner lock first, held while this Valtz runs it.
        let fd = open(Self.lockFile(dir).path, O_CREAT | O_RDWR, 0o644)
        if fd >= 0 {
            if flock(fd, LOCK_EX | LOCK_NB) == 0 {
                sessionLock = fd
            } else {
                close(fd)
            }
        }
        let r = core.createUntitled(dir: dir.path, name: anonymousName)
        guard r.ok, let id = r["project"] as? String else {
            note("error", r.message)
            return
        }
        _ = core.setEphemeral(project: id, true)
        projectId = id
        projectPath = dir.path
        projectName = anonymousName
        isAnonymous = true
        reloadAssets()
        refreshDocument()
    }

    /// Something is running (a generation, the assistant, an import):
    /// the session cannot be discarded under it.
    var isBusy: Bool {
        generationJob != nil || enhanceJob != nil
            || jobs.values.contains { $0.state == "queued" || $0.state == "running" }
    }

    /// Undo and redo wait for the project's own short jobs -- an import,
    /// links refreshed. The TASKS run on through them (DESIGN §3a): a
    /// task's request undone withdraws it.
    var historyBusy: Bool {
        jobs.values.contains {
            ($0.state == "queued" || $0.state == "running")
                && ($0.purpose == "import" || $0.purpose == "refresh-links")
        }
    }

    /// The session holds something Start Over would delete.
    var sessionHasWork: Bool {
        !assets.isEmpty || !prompt.isEmpty || !promptAttachments.isEmpty
    }

    /// Start Over: the anonymous session is deleted from disk -- its
    /// package (results, imports, thumbnails) and any shared or dragged
    /// copies -- and a new one begins with a new name, an empty prompt,
    /// no result, and the generation settings and adjustments at their
    /// defaults.
    func startOver() {
        guard isAnonymous, !isBusy, let core else { return }
        if let id = projectId { _ = core.discardProject(id) }
        removeOwnSession()
        projectId = nil
        withAnimation(Self.motion) { resetSession() }
        anonymousName = Self.randomAnonymousName()
        startAnonymousSession()
    }

    /// File › New Anonymous Project: a new ANONYMOUS session -- private,
    /// untitled, deleted when it goes unless Save As names it (DESIGN
    /// §10a). A named project closes first, its changes saved or let go
    /// (confirmClose); an anonymous one is deleted, as Start Over deletes
    /// it -- asked first when it holds work.
    func newAnonymousProject() {
        guard core != nil else { return }
        guard !isBusy else {
            flash(String(localized: "Wait for the work in progress to finish."))
            return
        }
        if isAnonymous {
            guard confirmCloseAnonymous(String(localized: "A new one starts in its place, and this one is deleted with everything in it — its results, imports and history — unless you save it.")) else { return }
            // Saved: it is a named project now, and closes as one.
            if isAnonymous {
                startOver()
                return
            }
        }
        guard confirmClose() else { return }
        if let before = projectId {
            retireProject(before, path: projectPath, anonymous: false)
        }
        projectId = nil
        withAnimation(Self.motion) { resetSession() }
        anonymousName = Self.randomAnonymousName()
        startAnonymousSession()
    }

    private func resetSession() {
        // An anonymous session's attachment and export folders go with it.
        sessionPanelFolders = [:]
        // The prompt, with its inline media, and the prompt asset it was.
        prompt = ""
        promptAssetId = nil
        promptName = nil
        promptAttachments = []
        referenceThumbs = [:]
        promptMentions = []
        focusedReference = nil
        stageBaseURL = nil
        stageBaseOriginal = nil
        baseAdjustments = [:]
        stageBeforeBase = nil
        generationBase = nil
        promptRevision += 1
        negative = ""
        intent = nil
        dropSuggestion()
        promptCompact = false
        // Immersive editing's prompts go with the session's.
        promptTabs = []
        activePromptTab = 0
        promptImmersive = false
        // The image region.
        stage = ViewerState()
        stageVisible = false
        lastStageResult = nil
        stageOriginal = nil
        stageAssetId = nil
        currentSlot = .a
        currentAside = nil
        stageDragReady = false
        adjustTask?.cancel()
        adjustments = ImageAdjustments()
        crop = CropSpec()
        baseCrops = [:]
        trim = TrimSpec()
        videoFrame = 0
        stagePage = 0
        videoCommand = nil
        videoRate = 0
        assets = []
        // The generation settings, at their defaults.
        modality = .image
        modelChoice = ""
        orientation = .square
        aspectRatio = .threeTwo
        sizeClass = .standard
        customSize = nil
        sizeIsBase = false
        otherShapes = [:]
        clipSeconds = Self.defaultClipSeconds
        clipCustomFrames = nil
        preference = .balanced
        showsTuning = false
        seedText = ""
        openPanel = nil
        searchText = ""
        banner = nil
        refreshMachine()  // picks the default models again
    }

    // MARK: - Title bar

    /// Rename the anonymous session (the title bar's name). Blank keeps
    /// the old name. The session is temporary, and so is its name.
    func renameSession(_ name: String) {
        let n = name.trimmingCharacters(in: .whitespacesAndNewlines)
        if !n.isEmpty { anonymousName = n }
        renamingTitle = false
    }

    /// Simple mode's anonymous session, or the open project.
    var windowTitle: String {
        if isAnonymous { return anonymousName }
        return projectName.isEmpty ? "Valtz" : projectName
    }

    /// The Edit menu's Undo: the text's typing while it has some, else the
    /// project's last command ("Undo Add Layer").
    var undoMenuTitle: String {
        if let t = textUndo, t.canUndo { return t.undoTitle }
        guard let kind = document.undo else {
            return String(localized: "Undo")
        }
        return String(localized: "Undo \(Self.commandName(kind))")
    }

    var redoMenuTitle: String {
        if let t = textUndo, t.canRedo { return t.redoTitle }
        guard let kind = document.redo else {
            return String(localized: "Redo")
        }
        return String(localized: "Redo \(Self.commandName(kind))")
    }

    var canUndoAny: Bool {
        textUndo?.canUndo == true || (document.undo != nil && !historyBusy)
    }

    var canRedoAny: Bool {
        textUndo?.canRedo == true || (document.redo != nil && !historyBusy)
    }

    /// ⌘Z: the text's typing, else the project's history.
    func undoAny() {
        if textUndo?.canUndo == true {
            textView()?.undoManager?.undo()
        } else {
            undoProject()
        }
        refreshTextUndo()
    }

    func redoAny() {
        if textUndo?.canRedo == true {
            textView()?.undoManager?.redo()
        } else {
            redoProject()
        }
        refreshTextUndo()
    }

    /// The text view with the focus, if one has it.
    private func textView() -> NSTextView? {
        let window = NSApp.keyWindow ?? NSApp.mainWindow
            ?? NSApp.windows.first { $0.isVisible }
        return window?.firstResponder as? NSTextView
    }

    /// What a command of the project's history is called, by its kind
    /// (core: Controller::command_).
    static func commandName(_ kind: String) -> String {
        switch kind {
        case "import": String(localized: "Import")
        case "generate.image": String(localized: "Generate Picture")
        case "edit.image": String(localized: "Edit Picture")
        case "generate.video": String(localized: "Generate Clip")
        case "generate.audio": String(localized: "Make Song")
        case "build": String(localized: "Build")
        case "adjust": String(localized: "Adjust")
        case "crop": String(localized: "Crop")
        case "trim": String(localized: "Trim")
        case "keys": String(localized: "Keyframes")
        case "canvas": String(localized: "Canvas Size")
        case "timeline": String(localized: "Length")
        case "page.add": String(localized: "Add Page")
        case "page.remove": String(localized: "Remove Page")
        case "layer.add": String(localized: "Add Layer")
        case "layer.duplicate": String(localized: "Duplicate Layer")
        case "layer.remove": String(localized: "Remove Layer")
        case "layer.move": String(localized: "Move Layer")
        case "layer.show": String(localized: "Show Layer")
        case "layer.hide": String(localized: "Hide Layer")
        case "layer.rename": String(localized: "Rename Layer")
        case "layer.source": String(localized: "Layer Content")
        case "layer.merge": String(localized: "Merge Layers")
        case "layer.mask": String(localized: "Use as Mask")
        case "layer.unmask": String(localized: "Release Mask")
        case "layer.split": String(localized: "Cut Clip")
        case "layer.slide": String(localized: "Move Clip")
        case "layer.stretch": String(localized: "Stretch")
        case "transcribe": String(localized: "Transcribe")
        case "summarize": String(localized: "Summarize Video")
        case "layer.group": String(localized: "Group Layers")
        case "layer.ungroup": String(localized: "Ungroup Layers")
        case "layer.folder-rename": String(localized: "Rename Layer Folder")
        case "layer.place": String(localized: "Move Layer")
        case "markup.layer": String(localized: "Markup Layer")
        case "markup.paint": String(localized: "Paint")
        case "markup.objects": String(localized: "Markup")
        case "markup.pixels": String(localized: "Make Pixels")
        case "markup.paste": String(localized: "Paste Drawing")
        case "markup.clear": String(localized: "Delete Drawing")
        case "folder.add": String(localized: "New Folder")
        case "folder.rename": String(localized: "Rename Folder")
        case "folder.remove": String(localized: "Delete Folder")
        case "asset.move": String(localized: "Move to Folder")
        case "capture.camera": String(localized: "Camera")
        case "asset.rename": String(localized: "Rename Asset")
        case "asset.remove": String(localized: "Remove Asset")
        case "asset.modify": String(localized: "Modified Copy")
        case "asset.capture": String(localized: "Capture")
        case "asset.place": String(localized: "Place")
        case "asset.instantiate": String(localized: "Place on Stage")
        case "prompt.text", "prompt.capture": String(localized: "Prompt")
        case "composition.new": String(localized: "New Composition")
        case "capture.record": String(localized: "Recording")
        case "generate.speech": String(localized: "Make Speech")
        default: String(localized: "Change")
        }
    }

    /// The viewer on screen: the stage.
    var currentViewer: ViewerState { stage }

    /// The fit button: pressed in, it fits now and keeps fitting as the
    /// window, the panels or the image change; released, the zoom stays.
    func toggleFit() {
        if currentViewer.fitting && currentViewer.zoom != nil {
            stage.unfitRequest += 1
        } else {
            zoom(.fit)
        }
    }

    /// There is an image on screen to zoom.
    var canZoom: Bool {
        // The Prompt Editor's stage is a small view, fitted.
        screen == .editor && stageVisible && stage.a != nil
            && !stage.showsMotion && !promptImmersive
    }

    /// Zoom the stage.
    func zoom(_ step: ZoomStep) {
        func apply(_ v: inout ViewerState) {
            switch step {
            case .zoomOut: v.zoomOutRequest += 1
            case .actualSize: v.actualSizeRequest += 1
            case .fit: v.fitRequest += 1
            case .zoomIn: v.zoomInRequest += 1
            }
        }
        apply(&stage)
    }

    /// The light bulb: the whole app light or dark, whatever the
    /// system's setting -- to proof an image against a light or a dark
    /// ground. `animated` (the bulb itself): the windows fade from one to
    /// the other over two seconds, the ground darkening or lightening
    /// gradually -- a crossfade Core Animation draws of everything they
    /// show (the window's frame view's layer: title bar, glass and stage
    /// too), the new look live under it.
    func setAppearance(dark: Bool, animated: Bool = false) {
        if animated {
            for w in NSApp.windows where w.isVisible {
                guard let layer = w.contentView?.superview?.layer
                    ?? w.contentView?.layer else { continue }
                let fade = CATransition()
                fade.type = .fade
                fade.duration = 2 * Self.animationScale
                fade.timingFunction = CAMediaTimingFunction(
                    name: .easeInEaseOut)
                layer.add(fade, forKey: kCATransition)
            }
        }
        NSApp.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
    }

    /// The app shows dark now.
    var appearsDark: Bool {
        NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua])
            == .darkAqua
    }

    /// What share and the inspector act on: the result on the stage.
    var currentAsset: AssetDTO? {
        stageAssetId.flatMap { id in assets.first { $0.id == id } }
    }

    /// Simple mode's result as shown, when it is adjusted: sharing and
    /// saving take the adjustments with them.
    private var adjustedResult: CGImage? {
        guard !adjustments.isIdentity || !crop.isIdentity,
              stageOriginal != nil,
              let shown = currentImage else { return nil }
        return cropped(shown, crop)
    }

    /// A file name for the image on screen: the asset's name (a prompt,
    /// for a result), made safe, with the extension it will be saved as.
    var exportFileName: String {
        guard let asset = currentAsset else { return "Valtz.png" }
        let ext = currentAdjusted && asset.kind == "image" ? "png"
            : (asset.url?.pathExtension.isEmpty == false
               ? asset.url!.pathExtension : "png")
        var base = asset.name
            .components(separatedBy: CharacterSet(charactersIn: "/:\\"))
            .joined(separator: "-")
            .trimmingCharacters(in: .whitespacesAndNewlines)
        if base.count > 80 { base = String(base.prefix(80)) }
        return (base.isEmpty ? "Valtz" : base) + "." + ext
    }

    /// The file a drag from the stage carries: the asset's own, or --
    /// adjusted -- a PNG of what is shown, in the temporary directory.
    /// Its own file is the asset's, so a drag into the prompt finds it.
    func shownFile() -> URL? {
        guard let url = currentAsset?.url else { return nil }
        guard let img = adjustedResult else { return url }
        let dir = Self.shareRoot
        try? FileManager.default.createDirectory(
            at: dir, withIntermediateDirectories: true)
        let out = dir.appendingPathComponent(exportFileName)
        return Self.writePNG(img, to: out) ? out : nil
    }

    /// A file to hand the share sheet: what is shown -- a stack
    /// flattened, an adjusted picture as a PNG -- named as Save… names
    /// it. Not the blob itself: its name is its hash. A linked original
    /// has its own name, and may be large: it goes as it is.
    func shareableFile() -> URL? {
        guard let a = currentAsset, let url = a.url else { return nil }
        if stageComposed { return flattenedFile() }
        if adjustedResult != nil || a.linked { return shownFile() }
        let dir = Self.shareRoot
        try? FileManager.default.createDirectory(
            at: dir, withIntermediateDirectories: true)
        let out = dir.appendingPathComponent(exportFileName)
        try? FileManager.default.removeItem(at: out)
        // A clone on APFS: no bytes copied.
        return (try? FileManager.default.copyItem(at: url, to: out)) != nil
            ? out : url
    }

    /// The picture Save… would write, as shown -- for the save panel's
    /// estimate of a JPEG's size.
    var savedPicture: CGImage? {
        guard currentAsset?.kind == "image" else { return nil }
        if let img = adjustedResult { return img }
        if stageOriginal != nil, let shown = currentImage {
            return shown
        }
        guard let url = currentAsset?.url,
              let src = CGImageSourceCreateWithURL(url as CFURL, nil)
        else { return nil }
        return CGImageSourceCreateImageAtIndex(src, 0, nil)
    }

    /// Save the image on screen (adjusted, if it is) to `dest`.
    /// The picture on screen carries adjustments (its modifier, or the
    /// panel's values on the stage): a file made from it applies them.
    var currentAdjusted: Bool {
        guard let a = currentAsset else { return false }
        // A trimmed sound: its own file is all of it.
        if a.kind == "audio" { return !a.trim.isIdentity }
        // A trimmed clip, or one with a look: its own file would be the
        // whole of it, as made.
        if a.kind == "video" {
            return !a.trim.isIdentity || !a.adjustKeys.isIdentity
                || !a.cropKeys.isIdentity
        }
        guard a.kind == "image" else { return false }
        // A stack's own file is only its bottom layer.
        if !(a.layers ?? []).isEmpty { return true }
        if stageOriginal != nil, a.id == stageAssetId {
            return !adjustments.isIdentity || !crop.isIdentity
        }
        return !a.adjustments.isIdentity || !a.crop.isIdentity
    }

    /// How long the clip or sound on the stage runs, as exported -- a
    /// timeline's length, else the file's (for a bitrate's file size).
    var exportSeconds: Double? {
        guard let a = currentAsset else { return nil }
        if a.isTimeline, let n = a.timeline ?? a.length,
           let r = a.compositionRate {
            return Double(n) / r.fps
        }
        return a.info?.seconds
    }

    /// What Save… offers for the image or video on screen: its own file
    /// and the exports for its kind. An adjusted picture's own file would
    /// drop its adjustments, so it offers the exports only -- they apply
    /// them as the file is made.
    var exportChoices: [ExportChoice] {
        guard let a = currentAsset,
              ["image", "video", "audio"].contains(a.kind) else { return [] }
        let exports = ExportChoice.allCases.filter {
            $0 != .original && $0.kind == a.kind
        }
        return currentAdjusted ? exports : [.original] + exports
    }

    /// Save…'s name for what is on the stage: "Save Audio…", "Save
    /// Video…" or "Save Image…" -- and its panel's title.
    /// The File menu's export of what is on the stage (Save is the
    /// project's).
    var saveItemTitle: String {
        switch currentAsset?.kind {
        case "audio": String(localized: "Export Audio…")
        case "video": String(localized: "Export Video…")
        default: String(localized: "Export Image…")
        }
    }

    var savePanelTitle: String {
        switch currentAsset?.kind {
        case "audio": String(localized: "Save Audio")
        case "video": String(localized: "Save Video")
        default: String(localized: "Save Image")
        }
    }

    /// Save the image or video on screen to `dest`: its own file (or what
    /// is shown, adjusted), or exported as `choice` -- a job; the banner
    /// says when it is saved. `quality`: a JPEG's, 1...100.
    func save(to dest: URL, as choice: ExportChoice, quality: Int? = nil,
              video: [String: Any]? = nil) {
        guard choice != .original, let core, let projectId,
              let asset = currentAsset else {
            saveCurrent(to: dest)
            return
        }
        // The export reads the modifiers from the project: the panels'
        // latest values first -- a clip's keyed look and marks too (a crop
        // changed just before was exported without it).
        flushPanels()
        var request: [String: Any] = [
            "project": projectId, "asset": asset.id,
            "format": choice.rawValue, "path": dest.path,
        ]
        if choice == .jpeg, let quality { request["quality"] = quality }
        if choice.video, let video { request["video"] = video }
        let r = core.exportAsset(request)
        guard r.ok, let job = r.job else {
            flash(r.message)
            return
        }
        exportJobs[job] = dest
        reloadTasks()
        // A clip's export is long: it shows on the stage as it goes --
        // when it runs now; queued behind another task, it waits in
        // Assets (a click there shows it).
        if asset.kind == "video",
           tasks.first?.job == job || tasks.isEmpty {
            withAnimation(Self.motion) {
                exportShow = ExportShow(job: job,
                                        file: dest.lastPathComponent)
            }
        }
    }

    func saveCurrent(to dest: URL) {
        guard let src = currentAsset?.url else { return }
        let fm = FileManager.default
        try? fm.removeItem(at: dest)
        let ok = (try? fm.copyItem(at: src, to: dest)) != nil
        if !ok {
            flash(String(localized: "Could not save \(dest.lastPathComponent)"))
        }
    }

    nonisolated static func writePNG(_ img: CGImage, to url: URL) -> Bool {
        guard let dst = CGImageDestinationCreateWithURL(
            url as CFURL, "public.png" as CFString, 1, nil) else {
            return false
        }
        CGImageDestinationAddImage(dst, img, nil)
        return CGImageDestinationFinalize(dst)
    }

    /// Search, in simple mode: the matching rows of the composer's cards
    /// are marked, and the card with the most of them opens (the one
    /// that was open comes back when the search is cleared). The prompt
    /// marks its own matches (PromptEditor).
    func searchChanged() {
        let q = searchText.trimmingCharacters(in: .whitespaces)
        let hits = !q.isEmpty ? SettingsRow.matching(q, in: self) : []
        searchHits = Set(hits.map(\.id))
        // The card with the most matches; the generation card on a tie.
        // A row always in view (the modality tab) opens none.
        let want: ComposerPanel? = !hits.contains { $0.panel != nil } ? nil
            : [ComposerPanel.generate, .adjust, .crop, .trim].max { a, b in
                hits.filter { $0.panel == a }.count
                    < hits.filter { $0.panel == b }.count
            }
        withAnimation(Self.motion) {
            if let want {
                if !searchOpenedPanel {
                    panelBeforeSearch = openPanel
                    searchOpenedPanel = true
                }
                openPanel = want
            } else if searchOpenedPanel {
                openPanel = panelBeforeSearch
                searchOpenedPanel = false
            }
        }
    }

    nonisolated private static let animals = [
        "axolotl", "badger", "capybara", "dormouse", "egret", "fennec",
        "gecko", "heron", "ibis", "jackal", "kestrel", "lemur", "lynx",
        "manatee", "marten", "narwhal", "ocelot", "okapi", "otter",
        "pangolin", "puffin", "quokka", "quail", "raccoon", "salamander",
        "tapir", "toucan", "urchin", "vole", "walrus", "wombat", "yak",
        "zebra", "meerkat", "koala", "bison", "crane", "dingo", "gibbon",
        "hedgehog",
    ]

    /// Decode with ImageIO, keeping the file's color space (so wide-gamut
    /// and HDR stills display correctly) rather than forcing sRGB.
    /// A still as it DISPLAYS: turned by its EXIF orientation, as the core
    /// turns a picture it feeds a model (media/model-input.mm) -- or a
    /// turned photo would not line up with what was made from it.
    /// A small copy of a picture file, its long edge `maxPixels`.
    nonisolated static func thumbnailImage(_ url: URL,
                                           maxPixels: Int) -> CGImage? {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return nil
        }
        return CGImageSourceCreateThumbnailAtIndex(src, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixels,
        ] as CFDictionary)
    }

    nonisolated static func loadImage(_ url: URL) -> CGImage? {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return nil
        }
        let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil)
            as? [CFString: Any]
        let orientation = (props?[kCGImagePropertyOrientation] as? NSNumber)?
            .intValue ?? 1
        if orientation != 1,
           let w = props?[kCGImagePropertyPixelWidth] as? Int,
           let h = props?[kCGImagePropertyPixelHeight] as? Int {
            // The thumbnail path turns it; at the full size it is the
            // whole picture.
            let opts: [CFString: Any] = [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: max(w, h),
                kCGImageSourceShouldCacheImmediately: true,
                kCGImageSourceShouldAllowFloat: true,
            ]
            if let img = CGImageSourceCreateThumbnailAtIndex(
                src, 0, opts as CFDictionary) {
                return img
            }
        }
        let opts: [CFString: Any] = [
            kCGImageSourceShouldCacheImmediately: true,
            // OpenEXR and other float stills stay float -- values above
            // 1 included -- for the canvas's extended dynamic range,
            // instead of being tone-mapped into 8 or 16 bits.
            kCGImageSourceShouldAllowFloat: true,
        ]
        return CGImageSourceCreateImageAtIndex(src, 0, opts as CFDictionary)
    }

    // MARK: - Events

    func handle(_ ev: CoreEvent) {
        let p = ev.payload
        if ev.kind.hasPrefix("fleet.") || ev.kind == "job.runner" {
            handleFleet(ev)
            return
        }
        switch ev.kind {
        case "log":
            note(p["level"] as? String ?? "info",
                 "\(p["category"] as? String ?? ""): "
                     + "\(p["message"] as? String ?? "")")
        case "assets.changed":
            reloadAssets()
        case "project.changed":
            documentChanged(p)
        case "project.renamed":
            if p["project"] as? String == projectId {
                projectName = p["name"] as? String ?? projectName
                projectPath = p["path"] as? String ?? projectPath
            }
        case "history.changed":
            reloadHistory()
        case "models.changed":
            refreshMachine()
        case "job.queued":
            if let job = ev.job {
                jobs[job] = JobInfo(title: p["title"] as? String ?? "",
                                    purpose: p["purpose"] as? String ?? "",
                                    state: "queued", progress: 0,
                                    model: p["model"] as? String ?? "")
            }
            if p["purpose"] as? String == "build"
                || p["purpose"] as? String == "export" {
                reloadTasks()
            }
            // A generation's prompt, captured: the box's words are it now
            // (the next Start keeps it for the same words).
            if ev.job == generationJob, let id = p["asset"] as? String {
                reloadAssets()
                if let pa = assets.first(where: { $0.id == id })?
                    .promptInput {
                    promptAssetId = pa
                }
            }
        case "job.started":
            if let job = ev.job {
                jobs[job]?.state = "running"
                // A task's clock starts as it leaves the queue.
                if taskContexts[job] != nil {
                    taskContexts[job]?.started = .now
                    if job == generationJob { generationStarted = .now }
                }
                if !tasks.isEmpty { reloadTasks() }
            }
        case "job.progress":
            guard let job = ev.job else { break }
            if let prog = p["progress"] as? Double, prog >= 0 {
                jobs[job]?.progress = prog
            }
            if let phase = JobPhase(p), jobs[job]?.phase != phase {
                jobs[job]?.phase = phase
                if let left = phase.left { estimateEnd(job, in: left) }
                if job == exportShow?.job, let left = phase.left {
                    exportShow?.ends = Date.now.addingTimeInterval(left)
                }
            }
        case "job.preview":
            guard let job = ev.job else { break }
            if let prog = p["progress"] as? Double, prog >= 0 {
                jobs[job]?.progress = prog
            }
            if job == exportShow?.job, let img = ev.image {
                exportShow?.preview = img
            }
            // A task's latest frame, kept for when it is watched (the
            // last only: a clip's is the whole clip, every step).
            if taskContexts[job] != nil, let img = ev.image {
                if let a = tasks.first(where: { $0.job == job })?.asset,
                   !a.isEmpty {
                    taskThumbs[a] = img
                }
                let step = p["step"] as? Int ?? 0
                taskContexts[job]?.step = step
                taskContexts[job]?.steps = p["steps"] as? Int ?? 0
                if ev.clip.isEmpty {
                    taskContexts[job]?.preview = img
                    taskContexts[job]?.clip = nil
                } else {
                    taskContexts[job]?.clip = PreviewClip(
                        frames: ev.clip, fps: p["fps"] as? Double ?? 24,
                        step: step)
                }
            }
            if job == generationJob, let ctx = taskContexts[job] {
                liveStep = ctx.step
                liveSteps = ctx.steps
                if let clip = ctx.clip {
                    showLiveClip(clip)
                } else if let img = ctx.preview {
                    showLive(img)
                }
            }
        case "assist.partial":
            if ev.job == enhanceJob {
                enhanceDraft = p["prompt"] as? String ?? enhanceDraft
            }
        case "assist.enhanced":
            if ev.job == enhanceJob {
                let e = EnhancedPrompt(
                    prompt: p["prompt"] as? String ?? "",
                    negative: p["negative"] as? String ?? "",
                    title: p["title"] as? String ?? "",
                    aspect: p["aspect"] as? String ?? "",
                    follow: p["follow"] as? String ?? "")
                if enhanceHere {
                    withAnimation(Self.motion) { enhanced = e }
                } else if let i = promptTabs.firstIndex(where: {
                    $0.id == enhanceTab }) {
                    // Written for a tab not open now: it waits there.
                    promptTabs[i].enhanced = e
                }
            }
        case "assist.intent":
            intent = IntentInfo(
                intent: p["intent"] as? String ?? "unknown",
                confidence: p["confidence"] as? Double ?? 0,
                fromModel: p["from_model"] as? Bool ?? false)
        case "job.finished", "job.failed", "job.cancelled":
            finish(ev)
        default:
            break
        }
    }

    private func showLive(_ img: CGImage) {
        // The live frame takes the stage from the picture to edit (and
        // from any render of its adjustments still on its way).
        adjustTask?.cancel()
        stage.a = img
        stage.video = nil
        stage.clip = nil
        stage.aLabel = String(localized: "Step \(liveStep) of \(liveSteps)")
        stageBaseURL = nil
        stageBaseOriginal = nil
        stageBeforeBase = nil
        if !stageVisible {
            // The first frame: the icon gives way to the result card.
            stage.fitRequest += 1
            withAnimation(Self.motion) { stageVisible = true }
        }
    }

    /// A video's live preview: the whole clip as the model has it at this
    /// step, looped on the stage; the next step replaces it. After the
    /// last step the real decoders run (picture and sound): the progress
    /// caption says so.
    private func showLiveClip(_ clip: PreviewClip) {
        adjustTask?.cancel()
        stage.a = clip.frames.first   // the layout's shape, and a poster
        stage.clip = clip
        stage.video = nil
        stage.b = nil
        stage.bLabel = ""
        stage.mode = .a
        stage.aLabel = String(localized: "Step \(liveStep) of \(liveSteps)")
        stageBaseURL = nil
        stageBaseOriginal = nil
        stageBeforeBase = nil
        stageOriginal = nil
        if !stageVisible {
            stage.fitRequest += 1
            withAnimation(Self.motion) { stageVisible = true }
        }
    }

    private func finish(_ ev: CoreEvent) {
        guard let job = ev.job else { return }
        let state = String(ev.kind.dropFirst(4))
        jobs[job]?.state = state
        jobs[job]?.phase = nil
        if !tasks.isEmpty || taskContexts[job] != nil { reloadTasks() }
        if state == "finished" { jobs[job]?.progress = 1 }
        if ev.kind == "job.failed" {
            let msg = L10n.coreMessage(ev.payload)
            note("error", msg)
            // A download's failure stays under its row in Capabilities.
            if let j = jobs[job], j.purpose == "download", !j.model.isEmpty {
                downloadFailures[j.model] = msg
            }
            // Refused for memory: its numbers and what to change stay over
            // the prompt, rather than a line that goes in six seconds.
            let task = taskContexts[job] != nil
            if job == generationJob || job == upscaleJob,
               let f = MemoryFailure(ev.payload, message: msg) {
                withAnimation(Self.motion) { memoryFailure = f }
            } else if task && job != generationJob {
                // A task not watched: whose it was, and why.
                let title = jobs[job]?.title ?? ""
                flash(title.isEmpty ? msg
                      : String(localized: "\(title): \(msg)"))
            } else if job == generationJob || job == enhanceJob
                || job == upscaleJob || exportJobs[job] != nil {
                flash(msg)
            }
        }
        // An upscale ended: its layer shows the result (the core swapped
        // it), its scale divided by as much.
        if job == upscaleJob {
            withAnimation(Self.motion) { upscaleJob = nil }
            if ev.kind == "job.finished" {
                // The core swapped the layer's source and divided its
                // scale: the panels take the layer as it now is, writing
                // nothing back. Saved first (selectLayer did), the panel's
                // old scale went back over the new clip -- shown right on
                // the stage, exported twice its size from a corner.
                syncAfterHistory()
                flash(String(localized: "Upscaled: the layer shows the result at its own pixels"))
            } else if ev.kind == "job.cancelled" {
                flash(String(localized: "Upscale stopped"))
            }
        }
        if let dest = exportJobs.removeValue(forKey: job),
           ev.kind == "job.finished" {
            // A still's pages: a file each, numbered beside the name.
            if let paths = ev.payload["paths"] as? [String],
               paths.count > 1 {
                let n = String(paths.count)
                let first = (paths.first as NSString?)?.lastPathComponent
                    ?? dest.lastPathComponent
                flash(String(localized: "Saved \(n) pages, from \(first)"))
            } else {
                flash(String(localized: "Saved \(dest.lastPathComponent)"))
            }
        }
        if job == exportShow?.job {
            withAnimation(Self.motion) { exportShow = nil }
            if ev.kind == "job.cancelled" {
                flash(String(localized: "Export stopped"))
            }
        }
        if job == enhanceJob {
            withAnimation(Self.motion) {
                enhanceJob = nil
                enhanceTab = nil
                enhanceDraft = ""
            }
        }
        // A task Start began: its result placed, watched or not.
        guard let ctx = taskContexts.removeValue(forKey: job) else { return }
        if let a = ev.payload["asset"] as? String { taskThumbs[a] = nil }
        let watched = job == generationJob
        if watched {
            withAnimation(Self.motion) { generationJob = nil }
        }
        reloadAssets()
        let asset = (ev.payload["asset"] as? String).flatMap { id in
            assets.first { $0.id == id }
        }
        // A generation is an asset, raw; it LANDS as a new layer of the
        // composition active at Start -- else in a composition of its own
        // (the project's when there is none) -- one command with it, and
        // that composition is what the stage shows.
        var placed: (doc: AssetDTO, layer: String)?
        if state == "finished", let asset, let core, let projectId,
           asset.kind == "image" || asset.kind == "video"
               || asset.kind == "audio" {
            var req: [String: Any] = ["asset": asset.id, "join": job]
            if let onto = ctx.onto {
                req["onto"] = onto
                if let at = ctx.at { req["at"] = at }
                req["offset"] = ctx.offset
            }
            let r = core.assetOp(project: projectId, "place-result", req)
            if r.ok, let view = r["asset"] as? String {
                reloadAssets()
                if let doc = assets.first(where: { $0.id == view }) {
                    placed = (doc, r["layer"] as? String ?? "")
                }
            } else if !r.ok {
                note("error", r.message)
            }
        }
        guard watched else {
            // Not watched: the stage stays with what is being done there
            // -- drawn again when that is what the result went into.
            if let placed, placed.doc.id == stageAssetId,
               generationJob == nil {
                layersChanged()
            }
            return
        }
        if let placed {
            finishStage(state: "finished", asset: placed.doc, result: asset,
                        layer: placed.layer, task: ctx)
            selectLayer(placed.layer)
            return
        }
        finishStage(state: state, asset: asset, task: ctx)
    }

    /// What is on the stage now, as it is: `asset`, showing `result` --
    /// the generation -- on `layer` (nil: it is the result itself). The
    /// compare opens by itself for an image EDIT only: A the new picture,
    /// B what it was edited from, as the model got it and fitted as the
    /// engine fitted it, so a wipe lines up (editB). Any other result is
    /// shown alone, what was there before set aside for the title bar's
    /// compare button.
    private func finishStage(state: String, asset: AssetDTO?,
                             result: AssetDTO? = nil,
                             layer: String? = nil,
                             task: TaskContext? = nil) {
        unwatchTask()
        if state == "finished", let asset,
           asset.kind == "video" || asset.kind == "audio",
           let url = clipURL(asset) {
            finishClip(asset, url)
            return
        }
        let atStart = task?.stageAtStart
        guard state == "finished", let asset,
              let img = stageImage(asset) else {
            if let atStart, state != "finished" {
                // A stopped or failed edit: the stage as it was at Start,
                // the original the picture to edit again; the caption
                // says what happened.
                generationBase = nil
                stageBaseURL = atStart.baseURL
                stageBaseOriginal = atStart.baseOriginal
                stageBeforeBase = atStart.beforeBase
                stageOriginal = atStart.original
                stageAssetId = atStart.assetId
                currentSlot = atStart.currentSlot
                currentAside = atStart.currentAside
                withAnimation(Self.motion) {
                    stage = atStart.stage
                    stageVisible = atStart.visible
                }
                flash(Self.endLabel(state))
                return
            }
            if stageVisible {
                // Keep the last frame; say what happened.
                stage.aLabel = Self.endLabel(state)
            }
            if state == "finished" { reportUnshown() }
            return
        }
        let base = task?.base
        stageBaseURL = nil
        stageBaseOriginal = nil
        stageBeforeBase = nil
        parkedB = nil
        let edited = base.flatMap { b in
            b.fitted ? editB(b, result: result ?? asset, shown: asset,
                             layer: layer,
                             canvas: CGSize(width: img.width,
                                            height: img.height))
                     : nil
        }
        withAnimation(Self.motion) {
            // B: what an edit was made from, lined up with the result --
            // else, set aside, a picture given to a model that does not
            // edit, or the previous result.
            if let edited {
                stage.b = edited.image
                stage.bFit = edited.fit
                stage.bLabel = edited.label
            } else if let base {
                stage.b = base.image
                stage.bLabel = base.label
            } else if let last = lastStageResult {
                stage.b = last.image
                stage.bLabel = last.label
            } else {
                stage.b = nil
                stage.bLabel = ""
            }
            stage.a = img
            stage.video = nil
            stage.clip = nil
            // A finished single image needs no caption; a comparison does.
            stage.aLabel = stage.b == nil ? "" : String(localized: "New")
            stageVisible = true
            // Compared only after an edit: else B is set aside, and the
            // compare is off.
            if edited == nil {
                parkB()
            }
        }
        // Its look, as recorded (a new take has none): shown, and what the
        // panels change from -- never reset to nothing and written back.
        stageOriginal = img
        // Another still: from its first page.
        if stageAssetId != asset.id { stagePage = 0 }
        stagePage = min(stagePage, asset.pageCount - 1)
        stageAssetId = asset.id
        currentSlot = .a
        currentAside = nil
        adjustTask?.cancel()
        adjustments = asset.adjustments(layer: "")
        crop = asset.crop(layer: "")
        if !adjustments.isIdentity && (asset.layers ?? []).isEmpty
            && asset.canvas == nil {
            redrawAdjusted()
        }
        // A picture with a stack (or on a canvas of its own) shows it,
        // layer 0 selected (or the bottom one, when layer 0 was removed).
        if !(asset.layers ?? []).isEmpty || asset.canvas != nil {
            let l0 = asset.layerStack.contains { $0.id.isEmpty }
                ? "" : asset.layerStack.first?.id ?? ""
            selectedLayer = l0
            adjustments = asset.adjustments(layer: l0)
            crop = asset.crop(layer: l0)
            // Pages: its tracks, at the page shown.
            if asset.isPaged { loadClipTracks(asset, layer: l0) }
            recomposite()
        }
        // The size the stage shows: a stack's canvas (drawn just above),
        // else the picture's.
        let shown: (Int, Int) = {
            guard !(asset.layers ?? []).isEmpty || asset.canvas != nil
            else { return (img.width, img.height) }
            if let c = asset.canvas, c.resized { return (c.w, c.h) }
            let o = ownFrame(of: asset)
            return (o.width, o.height)
        }()
        if stage.mode == .a, let b = stage.b, stage.bFit != .identity
            || (b.width == shown.0 && b.height == shown.1) {
            // Lined up: the new take lands in a wipe against the last, or
            // against what it was edited from.
            setStageMode(.wipe)
        }
        lastStageResult = (img, String(localized: "Previous"))
    }

    /// An edit's B: its base AS THE MODEL GOT IT -- the history's base as
    /// sent (adjusted, cropped, developed), else the picture at Start --
    /// fitted as the engine fitted it for the VAE (filled and
    /// centre-cropped to the result's size: vpipe's image-resample
    /// "crop"), then placed where the result lies on the stage's canvas:
    /// centred at its own size, as a new take or a layer without a crop
    /// lies. Nil where that place is not known (a layer placed by its
    /// crop) or the result's size is not.
    private func editB(_ base: (image: CGImage, label: String, fitted: Bool),
                       result: AssetDTO, shown: AssetDTO, layer: String?,
                       canvas: CGSize)
        -> (image: CGImage, fit: CanvasFit, label: String)? {
        if let layer, !shown.crop(layer: layer).isIdentity { return nil }
        let size: CGSize? = {
            if let f = result.info?.frame, f.w > 0, f.h > 0 {
                return CGSize(width: f.w, height: f.h)
            }
            // The result is what the stage shows.
            return layer == nil ? canvas : nil
        }()
        guard let size else { return nil }
        let sent = baseEntry(of: result).flatMap { Self.loadImage($0.url) }
            ?? base.image
        var (image, fit) = CanvasFit.centreCrop(sent, onto: size)
        fit.origin.x += (canvas.width - size.width) / 2
        fit.origin.y += (canvas.height - size.height) / 2
        return (image, fit, base.label)
    }

    /// A finished clip takes the stage from its preview: the player, its
    /// sound on. There is nothing to compare it with or adjust; its
    /// poster frame (for the layout and a drag) is the preview's until
    /// the clip's own first frame is read.
    private func finishClip(_ asset: AssetDTO, _ url: URL) {
        unwatchTask()
        generationBase = nil
        stageBaseURL = nil
        stageBaseOriginal = nil
        stageBeforeBase = nil
        withAnimation(Self.motion) {
            stage.video = url
            stage.clip = nil
            stage.b = nil
            stage.bLabel = ""
            stage.mode = .a
            stage.aLabel = ""
            stageVisible = true
        }
        stageOriginal = nil
        stageAssetId = asset.id
        currentSlot = .a
        currentAside = nil
        adjustTask?.cancel()
        adjustments = ImageAdjustments()
        crop = CropSpec()
        videoFrame = 0
        // Its look: the tracks it carries, or one key at its first frame.
        if !asset.layerStack.contains(where: { $0.id == selectedLayer }) {
            selectedLayer = ""
            selectedLayers = []
        }
        loadClipTracks(asset, layer: activeLayer)
        // Its selected layer's marks and start.
        loadTrim(asset)
        dropStackPlan()
        refreshStackPlan()
        // The next picture does not compare with a clip.
        lastStageResult = nil
        parkedB = nil
        compareEngaged = false
        let sound = asset.kind == "audio"
        Task { [weak self] in
            // A sound's is its waveform.
            let made = sound
                ? await AudioWaveform.image(url, width: 1600, height: 400)
                : await Self.posterFrame(url)
            guard let poster = made else { return }
            // A stack's poster is its own first frame (stackPoster).
            guard let self, self.stage.video == url,
                  self.stackPlayback == nil else { return }
            self.stage.a = poster
        }
    }

    /// A clip's first frame, upright, as a still.
    nonisolated static func posterFrame(_ url: URL) async -> CGImage? {
        let gen = AVAssetImageGenerator(asset: AVURLAsset(url: url))
        gen.appliesPreferredTrackTransform = true
        return try? await gen.image(at: .zero).image
    }

    /// Dev and snapshot aid: `asset` on the stage, as if it had just been
    /// made (a clip in the player, a picture on the canvas).
    /// A generated clip -- or song -- on the stage as it was made (Show on
    /// Stage, in the asset list): the compare takes pictures only.
    func showMadeOnStage(_ a: AssetDTO) {
        guard a.kind == "video" || a.kind == "audio", let url = a.url else {
            return
        }
        finishClip(a, url)
    }

    func putOnStage(_ asset: AssetDTO) {
        // Picked: the stage leaves the task it watched, which runs on.
        unwatchTask()
        if asset.kind == "video" || asset.kind == "audio",
           let url = clipURL(asset) {
            finishClip(asset, url)
        } else {
            finishStage(state: "finished", asset: asset)
        }
    }

    /// What the player plays a clip or a sound as: its file -- a
    /// composition's stand-in -- or, a timeline with none, a name of its
    /// own (its frames and its sound come from its plan).
    func clipURL(_ a: AssetDTO) -> URL? {
        if let u = a.url { return u }
        guard a.isTimeline else { return nil }
        return URL(string: "valtz-composition:\(a.id)")
    }

    /// A picture-like asset as the stage first shows it: its file (a
    /// composition's stand-in) -- or, a composition with none, a clear
    /// frame of its size, which the core then draws over.
    func stageImage(_ a: AssetDTO) -> CGImage? {
        if let u = a.url, let img = Self.loadImage(u) { return img }
        guard a.isComposition || a.isMarkupAsset else { return nil }
        let size = a.isComposition ? ownFrame(of: a)
            : (width: a.markup?.w ?? 0, height: a.markup?.h ?? 0)
        let w = size.width, h = size.height
        guard w > 0, h > 0, let ctx = CGContext(
            data: nil, width: w, height: h, bitsPerComponent: 8,
            bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        return ctx.makeImage()
    }

    /// What the stage shows when it is not the project's composition,
    /// named over its top left ("Viewing <name>"); nil while it shows the
    /// project, nothing, or a generation in flight.
    var stageViewingName: String? {
        guard stageVisible, !isGenerating else { return nil }
        if adjustsBase, let url = stageBaseURL {
            return asset(forFile: url)?.name ?? url.lastPathComponent
        }
        guard let id = stageAssetId, !isProjectView(id),
              let a = assets.first(where: { $0.id == id }) else {
            return nil
        }
        return a.name
    }

    /// Dev and snapshot aid: a key of a clip's track at `frame`, with
    /// values by the core's names (adjustment keys; "offset_x",
    /// "offset_y", "scale_x", "scale_y" -- or "scale", both -- for the
    /// placement; "rotate" for the turn).
    func setKey(_ track: KeyTrack, at frame: Int, _ kv: [String: Double]) {
        switch track {
        case .adjust:
            var v = clipAdjustKeys.value(at: frame) ?? ImageAdjustments()
            for k in ImageAdjustments.Key.allCases {
                if let x = kv[k.rawValue] { v[k] = x }
            }
            clipAdjustKeys.set(v, at: frame)
        case .place:
            var c = clipCropKeys.place.value(at: frame) ?? CropSpec()
            if c.contentWidth == 0, let a = stage.a {
                c.contentWidth = a.width
                c.contentHeight = a.height
            }
            for (k, x) in kv {
                switch k {
                case "scale":
                    c.scaleX = x
                    c.scaleY = x
                case "scale_x": c.scaleX = x
                case "scale_y": c.scaleY = x
                case "offset_x": c.offsetX = x
                case "offset_y": c.offsetY = x
                default: break
                }
            }
            clipCropKeys.place.set(c, at: frame)
        case .turn:
            clipCropKeys.turn.set(Turn(degrees: kv["rotate"] ?? 0), at: frame)
        }
        clipTracksChanged()
    }

    /// Dev and snapshot aid: crop values by the core's names
    /// ("scale", "rotate", "offset_x", "pad_a", ...).
    func setCropValues(_ kv: [String: Double]) {
        changeCrop { c in
            for (k, v) in kv {
                switch k {
                case "scale":
                    c.scaleX = v
                    c.scaleY = v
                case "scale_x": c.scaleX = v
                case "scale_y": c.scaleY = v
                case "rotate": c.rotate = v
                case "offset_x": c.offsetX = v
                case "offset_y": c.offsetY = v
                case "pad_r": c.pad[0] = v
                case "pad_g": c.pad[1] = v
                case "pad_b": c.pad[2] = v
                case "pad_a": c.pad[3] = v
                default: break
                }
            }
        }
        // A clip's turn and background are its own, not the placement's.
        guard keyedStage else { return }
        if let r = kv["rotate"] { setCropRotate(r) }
        let pads = ["pad_r", "pad_g", "pad_b", "pad_a"]
        if pads.contains(where: { kv[$0] != nil }) {
            var rgba = clipCropKeys.pad
            for (i, k) in pads.enumerated() {
                if let v = kv[k] { rgba[i] = v }
            }
            clipCropKeys.pad = rgba
            clipTracksChanged()
        }
    }

    /// The caption for a job that ended without a result to show. A job
    /// that FINISHED made its image -- it is in the project -- even when
    /// it cannot be shown; that is not a failure.
    private static func endLabel(_ state: String) -> String {
        switch state {
        case "cancelled": String(localized: "Cancelled")
        case "finished": String(localized: "Made, but not shown")
        default: String(localized: "Failed")
        }
    }

    private func reportUnshown() {
        let why = log.last { $0.level == "error" }?.text
            ?? String(localized: "its asset was not found")
        flash(String(localized: "The image was made and is in the project, but could not be shown: \(why)"))
    }

    // MARK: - Messages

    /// A short-lived message under the simple-mode composer.
    func flash(_ text: String) {
        bannerTask?.cancel()
        withAnimation(Self.motion) { banner = text }
        bannerTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(6))
            guard !Task.isCancelled else { return }
            withAnimation(AppModel.motion) { self?.banner = nil }
        }
    }

    func note(_ level: String, _ text: String) {
        log.append(LogLine(level: level, text: text))
        if log.count > 200 { log.removeFirst(log.count - 200) }
    }
}

extension UTType {
    /// A Valtz project: a PACKAGE (Info.plist's exported type, which
    /// conforms to com.apple.package). Looked up by its extension among
    /// packages -- `UTType(filenameExtension:)` alone looks among data
    /// files, finds none, and makes up a dynamic type no project
    /// conforms to: the open panel greyed out every project.
    static let valtzProject = UTType(filenameExtension: "valtz",
                                     conformingTo: .package) ?? .package
}

