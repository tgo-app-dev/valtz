import Foundation

/// Decoders for the JSON documents the controller returns. Field names
/// mirror the C++ side (snake_case, converted automatically).
enum DTO {
    static func decode<T: Decodable>(_ type: T.Type, _ data: Data) -> T? {
        let d = JSONDecoder()
        d.keyDecodingStrategy = .convertFromSnakeCase
        return try? d.decode(type, from: data)
    }

    static func decode<T: Decodable>(_ type: T.Type, _ any: Any?) -> T? {
        guard let any, JSONSerialization.isValidJSONObject(any),
              let data = try? JSONSerialization.data(withJSONObject: any)
        else { return nil }
        return decode(type, data)
    }
}

struct HardwareInfo: Decodable, Sendable {
    var chip: String
    var modelId: String
    var osVersion: String
    var ramGb: Int
    var perfCores: Int
    var efficiencyCores: Int
    var gpuCores: Int
    var gpuMatrixCores: Bool
    /// Neural Engine cores (0: not known).
    var aneCores: Int?
    var gpuWorkingSetGb: Double
    var thermal: String
    var assistantModel: String?
}

struct ModelOption: Decodable, Sendable, Hashable {
    var model: String
    var name: String
    var state: String      // missing | partial | installed
    var fits: Bool
    var diskGb: Double
    var minRamGb: Int
    var dir: String
}

/// What the model field's Auto picks for one modality and op (core
/// Controller::auto_models): `chosen` is "" when nothing in `models` runs
/// here yet.
struct AutoChoice: Decodable, Sendable {
    struct Model: Decodable, Sendable {
        var model: String
        var name: String
        var runs: Bool
    }
    var chosen: String
    var models: [Model]
}

struct CapabilityStatus: Decodable, Sendable, Identifiable {
    var capability: String
    var label: String       // English, from the core's catalog
    /// `label` in the user's language, by the capability's stable id.
    var localizedLabel: String { Self.localizedLabel(capability, label) }

    /// A capability named in the user's language by its id -- a fleet
    /// member's, which comes without a label (`english`: the catalog's,
    /// else the id read as words).
    static func localizedLabel(_ capability: String,
                               _ english: String? = nil) -> String {
        switch capability {
        case "text-to-image": String(localized: "Text to image")
        case "image-edit": String(localized: "Image edit")
        case "text-to-video": String(localized: "Text to video")
        case "image-to-video": String(localized: "Image to video")
        case "reference-to-video": String(localized: "Reference to video")
        case "upscale-image": String(localized: "Image upscale")
        case "upscale-video": String(localized: "Video upscale")
        case "live-preview": String(localized: "Live preview")
        case "prompt-enhance": String(localized: "Prompt enhancement")
        case "intent": String(localized: "Intent detection")
        case "caption": String(localized: "Image captioning")
        case "alpha-output": String(localized: "Transparent output")
        case "audio-output": String(localized: "Video with audio")
        case "audio-transcribe": String(localized: "Transcribe audio")
        case "text-to-audio": String(localized: "Text to audio")
        case "text-to-speech": String(localized: "Text to speech")
        default: english ?? capability.replacingOccurrences(of: "-", with: " ")
        }
    }
    var availability: String  // ready | needs-download | needs-more-ram | ...
    var chosen: String
    var options: [ModelOption]
    var id: String { capability }
}

struct CatalogModel: Decodable, Sendable, Identifiable {
    var id: String
    var name: String
    var family: String
    var role: String
    var hfPath: String
    var capabilities: [String]
    var diskGb: Double
    var diskMeasured: Bool
    var minRamGb: Int
    var rank: Int
    var license: String
    var gated: Bool
    var notes: String
    /// The grid its pictures are made on: sizes are multiples of it.
    var sizeAlign: Int?
    /// What its prompts call an edit's n-th picture ("<image{n}>"); empty
    /// when it has no such tags.
    var referenceTag: String?
    /// The steps each preference runs here ("speed", "balanced",
    /// "quality") -- a few-step adapter's when it is installed -- and that
    /// adapter's name ("" without one).
    var steps: [String: Int]?
    var turbo: String?
    /// Each preset ("speed", "balanced", "quality") as it runs here: its
    /// steps, its few-step adapter, the LoRAs it needs and lacks, the
    /// switches it sets (core Controller::preset_summary).
    var presets: [String: PresetSummary]?
    /// A clip's lengths: frames = offset + n × step, at `fps`; its default
    /// and longest. Nil for a model that makes pictures.
    var frameGrid: FrameGrid?
    var fps: Double?
    var frames: Int?
    var maxFrames: Int?
    /// A sound's longest: the model decides a song's length, up to this.
    var maxSeconds: Double?
    /// How a continuation's prompt opens, as its model was trained
    /// ("[video continuation + audio reference]"); nil for one without.
    var continuation: String?
    /// It has a prompt outline (catalog `prompting.outline`): an empty
    /// prompt box can start from it.
    var hasOutline: Bool?
    /// A quantized variant of `quantizeFrom` (its bits): listed as that
    /// one -- Favor picks the file (core Controller::weights_variant_).
    var quantizeFrom: String?
    var quantizeBits: Int?
}

/// A prompt as a song's words (core assist::split_song): its style, and
/// its lyrics under section headers -- how many sections, how many sung
/// lines.
struct SongWords: Sendable, Equatable {
    var style: String
    var lyrics: String
    var sections: Int
    var lines: Int

    static let none = SongWords(style: "", lyrics: "", sections: 0, lines: 0)
}

/// A Favor preset as it runs here (core Controller::preset_summary).
struct PresetSummary: Decodable, Sendable, Hashable {
    struct Lora: Decodable, Sendable, Hashable {
        var id: String
        var name: String
    }
    var steps: Int
    /// Its few-step adapter's name; "" without one.
    var turbo: String
    /// The LoRAs it runs with, none of them downloaded: it cannot run.
    var missing: [Lora]
    var solAttn: Bool
    var sageAttn: Bool
    var i8Gemm: Bool
    /// The Neural Engine's share (before M5, on Fast and Med: core
    /// models/tuning.cc); nil from an older core.
    var aneFfn: Bool?
    var aneQkv: Bool?
    var motionCache: Bool
    /// Speech: the model's weights held at 8 bits (MOSS-TTS's
    /// `w8_weights`); nil for anything else.
    var w8: Bool?
}

struct FrameGrid: Decodable, Sendable, Hashable {
    var step: Int
    var offset: Int
}

struct ColorDTO: Decodable, Sendable, Hashable {
    var primaries: Int
    var transfer: Int
    var matrix: Int
    var range: Int
    var icc: String?

    /// Short label, following media/color.h's CICP code points.
    var label: String {
        if let icc, primaries == 2, transfer == 2 { return icc }
        let p: String = switch primaries {
        case 1: "709"
        case 9: "2020"
        case 11: "DCI-P3"
        case 12: "P3"
        case 2: String(localized: "untagged")
        default: "cicp \(primaries)"
        }
        let t: String = switch transfer {
        case 13: "sRGB"
        case 1, 6, 14, 15: "gamma"
        case 8: "linear"
        case 16: "PQ"
        case 18: "HLG"
        default: ""
        }
        return t.isEmpty ? p : "\(p) \(t)"
    }

    var isHDR: Bool { transfer == 16 || transfer == 18 }
}

struct FrameDTO: Decodable, Sendable, Hashable {
    var w: Int
    var h: Int
    var format: String
    var alpha: String
    var bits: Int
    var color: ColorDTO
}

struct MediaInfoDTO: Decodable, Sendable, Hashable {
    var type: String
    var codec: String
    var codecName: String
    var frame: FrameDTO
    /// A clip's frame rate ([num, den]) and frame count.
    var fps: [Int]?
    var frames: Int?
    /// Its length, [num, den] seconds (a sound's, a clip's).
    var duration: [Int]?
    /// A clip's sound track: whether it has one.
    var audio: SoundTrackDTO?

    var seconds: Double? {
        guard let d = duration, d.count == 2, d[1] > 0 else { return nil }
        return Double(d[0]) / Double(d[1])
    }

    var frameRate: FrameRate? {
        guard let f = fps, f.count == 2, f[0] > 0, f[1] > 0 else {
            return nil
        }
        return FrameRate(num: f[0], den: f[1])
    }
    /// What the camera or editor recorded, under EXIF's names
    /// (media/exif.h); nil when there is nothing.
    var exif: [String: ExifValue]?
}

/// A clip's sound track as the core reports it ([has, channels, rate]):
/// whether there is one. Anything else reads as none.
struct SoundTrackDTO: Decodable, Sendable, Hashable {
    var has = false

    init(from decoder: Decoder) throws {
        guard var c = try? decoder.unkeyedContainer() else { return }
        has = (try? c.decode(Bool.self)) ?? false
    }
}

/// An EXIF value as the core reports it: text or a number. Anything else
/// reads as text, so one odd value never drops the asset list.
enum ExifValue: Decodable, Sendable, Hashable {
    case text(String)
    case number(Double)

    init(from decoder: Decoder) throws {
        let c = try decoder.singleValueContainer()
        if let d = try? c.decode(Double.self) {
            self = .number(d)
        } else if let s = try? c.decode(String.self) {
            self = .text(s)
        } else {
            self = .text("")
        }
    }

    var text: String? {
        if case .text(let s) = self, !s.isEmpty { return s }
        return nil
    }

    var number: Double? {
        if case .number(let d) = self { return d }
        return nil
    }
}

struct RecipeDTO: Decodable, Sendable, Hashable {
    var op: String
    var model: String
    var params: RecipeParamsDTO?
    var inputs: [RecipeInputDTO]?
    var created: Int64?   // ms since 1970

    var createdDate: Date? {
        created.map { Date(timeIntervalSince1970: Double($0) / 1000) }
    }
}

/// How long a result took to make (core controller/job-timing.h): the
/// whole, and each phase it went through. Read leniently, as the recipe
/// is: a field of an unexpected type is dropped, never the asset list.
/// A state in the project's generation history (core
/// project::HistoryEntry): a generation's result as made, or an edit's
/// base as the model got it -- frozen, by the time it was.
struct HistoryEntryDTO: Decodable, Sendable, Identifiable, Hashable {
    var id: String
    /// Milliseconds since 1970.
    var created: Int64
    /// "base" | "result".
    var role: String
    var path: String
    var width: Int
    var height: Int
    var asset: String
    var version: Int
    var generation: String
    var label: String
    /// Drawn as the model got it (adjusted, cropped, developed): no
    /// file of the asset is this picture.
    var rendered: Bool
    /// A clip made: its frames and length (its movie is `path`); a
    /// song's length.
    var frames: Int?
    var seconds: Double?
    /// What `path` holds: "image", "video" or "audio"; nil from an older
    /// core (a clip then has frames).
    var kind: String?

    var url: URL { URL(fileURLWithPath: path) }
    var date: Date { Date(timeIntervalSince1970: Double(created) / 1000) }
    var isBase: Bool { role == "base" }
    var isClip: Bool { kind == "video" || (kind == nil && (frames ?? 0) > 0) }
    /// A song made: its sound is `path`.
    var isSound: Bool { kind == "audio" }
    /// A picture: what the compare takes.
    var isPicture: Bool { !isClip && !isSound }

    /// What the stage's caption calls it: "Base · 12:38".
    var caption: String {
        let t = date.formatted(date: .omitted, time: .shortened)
        return isBase ? String(localized: "Base · \(t)")
                      : String(localized: "Result · \(t)")
    }
}

struct TimingDTO: Decodable, Sendable, Hashable {
    var seconds: Double?
    var phases: [String: Double]?

    private enum K: String, CodingKey { case seconds, phases }

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: K.self)
        seconds = try? c.decodeIfPresent(Double.self, forKey: .seconds)
        phases = try? c.decodeIfPresent([String: Double].self, forKey: .phases)
    }
}

/// What a generation or edit was asked for. Every field is optional and
/// read leniently: a value of an unexpected type drops that field, never
/// the asset list it came with.
struct RecipeParamsDTO: Decodable, Sendable, Hashable {
    var prompt: String?
    var negative: String?
    var seed: String?     // shown, never computed with; may exceed Int64
    var steps: Int?
    var width: Int?
    var height: Int?
    /// A clip's: its frames, and the run-time adapter it ran with (a
    /// catalog id).
    var frames: Int?
    var lora: String?
    /// A song's: the score it planned (SongPlan's raw value) and the
    /// longest it was let run, seconds.
    var cot: String?
    var maxSeconds: Double?

    private enum K: String, CodingKey {
        case prompt, negative, seed, steps, width, height, frames, lora
        case cot, maxSeconds
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: K.self)
        prompt = try? c.decode(String.self, forKey: .prompt)
        negative = try? c.decode(String.self, forKey: .negative)
        seed = (try? c.decode(UInt64.self, forKey: .seed)).map(String.init)
            ?? (try? c.decode(Int64.self, forKey: .seed)).map(String.init)
        steps = try? c.decode(Int.self, forKey: .steps)
        width = try? c.decode(Int.self, forKey: .width)
        height = try? c.decode(Int.self, forKey: .height)
        frames = try? c.decode(Int.self, forKey: .frames)
        lora = try? c.decode(String.self, forKey: .lora)
        cot = try? c.decode(String.self, forKey: .cot)
        maxSeconds = try? c.decode(Double.self, forKey: .maxSeconds)
    }
}

struct FrameSizeDTO: Decodable, Sendable, Hashable {
    var w: Int
    var h: Int
}

/// A clip's stack, ready for the player to draw (bridge stack_plan): its
/// plan's token, the frame it draws, its timeline, and the clips its
/// video layers show, in order -- the player's tracks.
struct StackPlayback: Equatable, Sendable {
    var plan: UInt64
    var width: Int
    var height: Int
    var frames: Int
    var rate: FrameRate
    var clips: [URL]
    /// Where each clip's spans play on the timeline: [at, length, source,
    /// source length] in seconds, per clip (core media::time_segments).
    var segments: [[[Double]]] = []
    /// What the player sounds: each layer's spans and its volume ramps
    /// ([second, gain]) -- or, when a pitch is shifted, the mixer's file.
    var audio: [StackAudio] = []
    var mix: URL? = nil
    var seconds: Double = 0
    /// Each layer where it lies in time, as the core resolved it (the
    /// timeline draws them).
    var layers: [StackSpan] = []

    /// Sound alone: nothing drawn (no renderer).
    var soundOnly: Bool { plan == 0 }

    /// What the player's composition is made of: a change rebuilds it; a
    /// new plan alone draws the frames again.
    var shape: [String] {
        clips.map(\.path) + ["\(width)x\(height)", "\(frames)",
                             "\(rate.num)/\(rate.den)",
                             "\(segments)", "\(audio.map(\.shape))",
                             mix?.path ?? ""]
    }
}

/// A layer of a timeline in time (core media::LayerTiming): its span in
/// timeline seconds -- `length` nil while it runs on to the end, as a
/// picture does -- what of its source shows there (`from` to `until`,
/// its seconds), what it is, and its file (a sound's waveform).
struct StackSpan: Equatable, Sendable {
    var id: String
    var start: Double
    var length: Double?
    var from: Double
    var until: Double?
    var timed: Bool
    var video: Bool
    var audioOnly: Bool
    var file: URL?

    var end: Double? { length.map { start + $0 } }
}

/// One layer's sound, as the player plays it.
struct StackAudio: Equatable, Sendable {
    var file: URL
    var segments: [[Double]]
    var volume: [[Double]]

    var shape: String { "\(file.path)|\(segments)|\(volume)" }
}

/// Canvas Size (core media::StackCanvas): `w` x `h` pixels, the stack's
/// own frame's top-left at (x, y).
struct CanvasDTO: Decodable, Sendable, Hashable {
    var w: Int
    var h: Int
    var x: Double
    var y: Double
    /// The project's FRAME (core StackCanvas::framed): what its layers
    /// lie on, its own size, kept apart from any layer's; nil without one.
    var fw: Int?
    var fh: Int?

    /// Resized (Canvas Size): `w` x `h`, the frame at (`x`, `y`).
    var resized: Bool { w > 0 && h > 0 }
    var framed: Bool { (fw ?? 0) > 0 && (fh ?? 0) > 0 }
}

/// A folder of the asset list (core Project::folders).
struct FolderDTO: Decodable, Sendable, Hashable, Identifiable {
    var id: String
    var name: String
}

/// A TASK of the queue (DESIGN §3a; core Controller::tasks): a job that
/// makes something -- a generation, an upscale, an export -- in the order
/// they run. `position` 0 is the one running (T0 in Assets).
struct TaskDTO: Decodable, Sendable, Hashable, Identifiable {
    var job: String
    var project: String
    /// The asset it makes ("" an export).
    var asset: String
    /// What an export is of; an upscale's composition (`layer` its layer).
    var source: String?
    var layer: String?
    /// "generate" | "upscale" | "export".
    var kind: String
    var op: String
    var title: String
    /// "queued" | "running".
    var state: String
    var position: Int
    /// Where it runs: "local" -- one day, a peer.
    var runner: String
    var progress: Double
    var destination: String?

    var id: String { job }
    var running: Bool { state == "running" }
    /// "T0", "T1", ...: its place in its runner's queue.
    var badge: String { "T\(position)" }
}

struct RecipeInputDTO: Decodable, Sendable, Hashable {
    var role: String?
    var asset: String?
}

struct AssetDTO: Decodable, Sendable, Identifiable, Hashable {
    var id: String
    var name: String
    var kind: String
    var origin: String
    var head: Int
    var created: Int64
    var modified: Int64
    var sourcePath: String
    var linked: Bool
    var info: MediaInfoDTO?
    var path: String?
    var stale: String?
    var recipe: RecipeDTO?
    var linkState: String?  // ok | relocated | modified | missing
    /// Non-destructive changes recorded on the image (core
    /// project::Modifier), applied when a file is made from it.
    var modifiers: [ModifierDTO]?
    /// Its layer stack, bottom first (core project::Layer); nil or empty:
    /// the picture is its own image. Its layers' folders.
    var layers: [LayerDTO]?
    var layerFolders: [LayerFolderDTO]?
    /// How long its current version took to make; nil for a source, or a
    /// result made before it was kept.
    var timing: TimingDTO?
    /// The canvas it is shown on, when resized (core media::StackCanvas):
    /// its size, and where its own frame lies on it.
    var canvas: CanvasDTO?
    /// A clip's timeline length, in its frames; nil: its own.
    var timeline: Int?
    /// A still's pages (DESIGN §6a); nil: one.
    var pages: Int?
    /// With a canvas: the stack's own frame (its bottom layer through its
    /// crop), what its layers lie on.
    var ownFrame: FrameSizeDTO?
    /// The asset list's folder it is in; nil or "": the list's top.
    var folder: String?
    /// A song's score: the ABC it followed (core AssetVersion::outputs).
    var score: String?
    /// What it is beside its kind ("prompt": a captured prompt).
    var tags: [String]?
    /// A prompt's words, its media named by position
    /// ("<valtz_ref_img_0>"), and how much has been made from it -- once
    /// anything, it is kept as it is.
    var text: String?
    var uses: Int?
    /// What it IS (core project::AssetClass, DESIGN §6a): "flat",
    /// "markup", "generated", "still", "composition".
    var assetClass: String?
    /// A composition's `path` is its STAND-IN: its bottom-most take of
    /// its own kind, for a poster, a size and what the player plays it
    /// as; the core draws the rest.
    var standIn: Bool?
    /// A composition's frame rate (a sound's counts milliseconds), its
    /// length in its frames, its transitions; whether it is the
    /// project's.
    var rateNum: Int?
    var rateDen: Int?
    var length: Int?
    var transitions: [TransitionDTO]?
    var project: Bool?
    /// A markup's content (core project::Markup).
    var markup: MarkupDTO?
    /// A flat asset FLATTENED from another: which (a note, not a tie).
    var from: FlattenedFromDTO?

    var isComposition: Bool {
        assetClass == "still" || assetClass == "composition"
    }
    /// A composition on a timeline (a clip's, or a sound's).
    var isTimeline: Bool { assetClass == "composition" }
    var isMarkupAsset: Bool { assetClass == "markup" }
    var isProject: Bool { project == true }
    /// Drawn by the core rather than read from a file.
    var isDrawn: Bool { isComposition || isMarkupAsset }
    /// A timeline's rate; nil for anything else.
    var compositionRate: FrameRate? {
        guard let n = rateNum, let d = rateDen, n > 0, d > 0 else {
            return nil
        }
        return FrameRate(num: n, den: d)
    }
    /// A layer's place in time.
    func time(of layer: String) -> LayerTimeDTO {
        layers?.first { $0.id == layer }?.time ?? LayerTimeDTO()
    }

    /// A still's pages: drawn a page at a time, each layer on its pages
    /// (its time's offset and duration, in pages), its look keyed by page.
    var pageCount: Int { max(1, pages ?? 1) }
    var isPaged: Bool { assetClass == "still" && pageCount > 1 }

    /// The pages a layer is on, from 0; nil on none.
    func pageSpan(of layer: LayerDTO) -> ClosedRange<Int>? {
        let t = layer.time ?? LayerTimeDTO()
        let last = t.duration > 0
            ? min(t.offset + t.duration, pageCount) - 1 : pageCount - 1
        return t.offset <= last ? t.offset...last : nil
    }

    func isOnPage(_ layer: LayerDTO, _ page: Int) -> Bool {
        guard isPaged else { return true }
        return pageSpan(of: layer)?.contains(page) == true
    }

    /// A captured prompt (core Controller::is_prompt).
    var isPrompt: Bool { kind == "text" && (tags ?? []).contains("prompt") }

    /// The prompt it was made from (its recipe's input "prompt").
    var promptInput: String? {
        recipe?.inputs?.first { $0.role == "prompt" }?.asset
    }

    /// What made it: "generate-image", "edit-image", "generate-video" (a
    /// GENERATION), "modify" (a modified copy), "capture", "unfreeze";
    /// nil for an import.
    var op: String? { recipe?.op }
    var isGeneration: Bool {
        ["generate-image", "edit-image", "generate-video", "generate-audio",
         "generate-speech"]
            .contains(op ?? "")
    }
    var isCapture: Bool { op == "capture" }
    /// The assets it is made from (its recipe's inputs).
    var inputAssets: [String] {
        recipe?.inputs?.compactMap(\.asset) ?? []
    }

    /// Its layers, bottom first. A composition's own; anything else is
    /// one layer -- itself ("", its own image) -- until an edited copy of
    /// it is made to change.
    var layerStack: [LayerDTO] {
        let l = layers ?? []
        if isComposition { return l }
        return l.isEmpty ? [LayerDTO(id: "", name: "", visible: true,
                                     own: true, source: nil)] : l
    }

    /// A layer's adjustments and crop ("" is the bottom one -- the whole
    /// picture without a stack).
    /// A track keyed by page (a still with pages, or one that had them):
    /// its first key, as the core reads it.
    func adjustments(layer: String) -> ImageAdjustments {
        guard let m = modifiers?.first(where: {
            $0.kind == "adjust" && $0.layer == layer
        }) else { return ImageAdjustments() }
        if m.keys != nil {
            return adjustKeys(layer: layer).value(at: 0) ?? ImageAdjustments()
        }
        var a = ImageAdjustments()
        for key in ImageAdjustments.Key.allCases {
            if let v = m.params[key.rawValue] { a[key] = v }
        }
        return a
    }

    func crop(layer: String) -> CropSpec {
        guard let m = modifiers?.first(where: {
            $0.kind == "crop" && $0.layer == layer
        }) else { return CropSpec() }
        if m.keys != nil { return cropKeys(layer: layer).value(at: 0) }
        return CropSpec(m.params)
    }

    /// Its "adjust" modifier for the whole image, as the panel's values.
    /// Its crop and rotation (modifier "crop"), and a clip's marks
    /// ("trim").
    var crop: CropSpec {
        modifiers?.first { $0.kind == "crop" && $0.layer.isEmpty }
            .map { CropSpec($0.params) } ?? CropSpec()
    }

    var trim: TrimSpec {
        modifiers?.first { $0.kind == "trim" }
            .map { TrimSpec($0.params) } ?? TrimSpec()
    }

    /// Its marks counted at `rate` -- the ones recorded at another (a
    /// sound marked in hundredths before milliseconds) moved over, to the
    /// nearest.
    func trim(at rate: FrameRate) -> TrimSpec {
        guard let m = modifiers?.first(where: { $0.kind == "trim" }) else {
            return TrimSpec()
        }
        var t = TrimSpec(m.params)
        let num = m.params["rate_num"] ?? 0, den = m.params["rate_den"] ?? 0
        guard num > 0, den > 0,
              Int(num) != rate.num || Int(den) != rate.den else { return t }
        let k = rate.fps / (num / den)
        t.markIn = t.markIn.map { Int((Double($0) * k).rounded()) }
        t.markOut = t.markOut.map { Int((Double($0) * k).rounded()) }
        return t
    }

    /// A clip's adjustment and crop tracks; empty with none (a still's
    /// flat value reads as one key at the first frame).
    var adjustKeys: Keyframes<ImageAdjustments> { adjustKeys(layer: "") }
    var cropKeys: ClipCrop { cropKeys(layer: "") }

    /// A layer's tracks, on a clip with a stack ("" is the clip itself).
    func adjustKeys(layer: String) -> Keyframes<ImageAdjustments> {
        guard let m = modifiers?.first(where: {
            $0.kind == "adjust" && $0.layer == layer
        }) else { return Keyframes() }
        return Keyframes(m.keys ?? [m.params]) { ImageAdjustments($0) }
    }

    func cropKeys(layer: String) -> ClipCrop {
        guard let m = modifiers?.first(where: {
            $0.kind == "crop" && $0.layer == layer
        }) else { return ClipCrop() }
        return ClipCrop(m)
    }

    /// A layer's SPEED and SOUND tracks (its "speed" / "audio"
    /// modifiers), keyed from its own start; empty with none.
    func speedKeys(layer: String) -> Keyframes<LayerSpeed> {
        guard let m = modifier("speed", layer) else { return Keyframes() }
        return Keyframes(m.keys ?? []) { LayerSpeed(rate: $0["rate"] ?? 1) }
    }

    func soundKeys(layer: String) -> Keyframes<LayerSound> {
        guard let m = modifier("audio", layer) else { return Keyframes() }
        return Keyframes(m.keys ?? []) {
            LayerSound(volume: $0["volume"] ?? 1, pitch: $0["pitch"] ?? 0)
        }
    }

    /// Its pitch follows its speed, as a tape's (core
    /// Controller::pitch_follows_speed), rather than being held.
    func pitchFollowsSpeed(layer: String) -> Bool {
        (modifier("audio", layer)?.params["follow_speed"] ?? 0) != 0
    }

    private func modifier(_ kind: String, _ layer: String) -> ModifierDTO? {
        modifiers?.first { $0.kind == kind && $0.layer == layer }
    }

    var adjustments: ImageAdjustments {
        guard let m = modifiers?.first(where: {
            $0.kind == "adjust" && $0.layer.isEmpty
        }) else { return ImageAdjustments() }
        var a = ImageAdjustments()
        for key in ImageAdjustments.Key.allCases {
            if let v = m.params[key.rawValue] { a[key] = v }
        }
        return a
    }

    var linkProblem: Bool {
        linkState == "missing" || linkState == "modified"
    }

    var isDerived: Bool { origin == "derived" }
    var isVisual: Bool { ["image", "video", "contact-sheet", "mask"].contains(kind) }
    var url: URL? { path.map { URL(fileURLWithPath: $0) } }
}

/// What a flat asset was flattened from (core Asset::from).
struct FlattenedFromDTO: Decodable, Sendable, Hashable {
    var asset: String?
}

/// Where two overlapping layers meet (core project::Transition).
struct TransitionDTO: Decodable, Sendable, Hashable {
    var from: String
    var to: String
    var kind: String
}

/// A composition layer's place in time (core project::LayerTime): its
/// marks in its source (in its own frames at rate_num/rate_den -- a
/// sound's milliseconds), where it starts on the timeline and how long it
/// runs (the composition's frames; 0: its span). -1: unset.
struct LayerTimeDTO: Decodable, Sendable, Hashable {
    var `in`: Int = -1
    var out: Int = -1
    var rateNum: Int = 0
    var rateDen: Int = 1
    var offset: Int = 0
    var duration: Int = 0

    init() {}

    private enum K: String, CodingKey {
        case `in`, out, rateNum, rateDen, offset, duration
    }

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: K.self)
        `in` = (try? c.decodeIfPresent(Int.self, forKey: .in)).flatMap { $0 }
            ?? -1
        out = (try? c.decodeIfPresent(Int.self, forKey: .out))
            .flatMap { $0 } ?? -1
        rateNum = (try? c.decodeIfPresent(Int.self, forKey: .rateNum))
            .flatMap { $0 } ?? 0
        rateDen = (try? c.decodeIfPresent(Int.self, forKey: .rateDen))
            .flatMap { $0 } ?? 1
        offset = (try? c.decodeIfPresent(Int.self, forKey: .offset))
            .flatMap { $0 } ?? 0
        duration = (try? c.decodeIfPresent(Int.self, forKey: .duration))
            .flatMap { $0 } ?? 0
    }

    /// Its marks as the Trim panel holds them.
    var marks: TrimSpec {
        var t = TrimSpec()
        if `in` >= 0 { t.markIn = `in` }
        if out >= 0 { t.markOut = out }
        return t
    }
    var rate: FrameRate? {
        rateNum > 0 && rateDen > 0 ? FrameRate(num: rateNum, den: rateDen)
                                   : nil
    }

    /// As the core reads it (bridge set_layer_time).
    var json: [String: Int] {
        ["in": `in`, "out": out, "rate_num": rateNum, "rate_den": rateDen,
         "offset": offset, "duration": duration]
    }
}

struct ModifierDTO: Decodable, Sendable, Hashable {
    var kind: String
    var layer: String
    /// Numbers; a value of another type is left out, never the asset
    /// list it came with.
    var params: [String: Double]
    /// A clip's track: its keyframes, each a key's numbers with its
    /// "frame" (core media/keyframes.h); nil for a still's flat value.
    var keys: [[String: Double]]?
    /// A clip's crop: its rotation's own keys ({"frame", "rotate"}).
    var rotateKeys: [[String: Double]]?

    private enum K: String, CodingKey { case kind, layer, params }
    /// "keys" and "rotate_keys" -- the decoder converts snake case, so
    /// the property is camel case (as "rotate_keys" it never matched, and
    /// a clip's turn track read as none).
    private struct Track: Decodable {
        var keys: [[String: LenientNumber]]?
        var rotateKeys: [[String: LenientNumber]]?
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: K.self)
        kind = (try? c.decode(String.self, forKey: .kind)) ?? ""
        layer = (try? c.decode(String.self, forKey: .layer)) ?? ""
        var p: [String: Double] = [:]
        if let raw = try? c.decode([String: LenientNumber].self,
                                   forKey: .params) {
            for (k, v) in raw { if let d = v.value { p[k] = d } }
        }
        params = p
        if let t = try? c.decode(Track.self, forKey: .params) {
            keys = t.keys?.map { $0.compactMapValues(\.value) }
            rotateKeys = t.rotateKeys?.map { $0.compactMapValues(\.value) }
        }
    }
}

/// A number, or nil for anything else.
private struct LenientNumber: Decodable {
    let value: Double?
    init(from decoder: Decoder) throws {
        value = try? decoder.singleValueContainer().decode(Double.self)
    }
}

struct PathsInfo: Decodable, Sendable {
    var projects: String
    var defaultProject: String
    var models: String
    var modelRoots: [String]
    var cache: String
    var cacheBytes: Int64
    var cacheBudgetBytes: Int64
    var cacheInternal: Bool
    var engine: String
}

struct IntentInfo: Sendable {
    var intent: String
    var confidence: Double
    var fromModel: Bool

    var label: String {
        switch intent {
        case "generate-image": String(localized: "Generate image")
        case "edit-image": String(localized: "Edit image")
        case "generate-video": String(localized: "Generate video")
        case "animate-image": String(localized: "Animate image")
        case "upscale": String(localized: "Upscale")
        case "describe": String(localized: "Describe")
        default: "—"
        }
    }

    var symbol: String {
        switch intent {
        case "generate-image": "photo"
        case "edit-image": "wand.and.stars"
        case "generate-video", "animate-image": "film"
        case "upscale": "arrow.up.left.and.arrow.down.right"
        case "describe": "text.bubble"
        default: "questionmark"
        }
    }
}

/// One of Favor's options (core models/tuning.h), as the core offers it
/// for a model: its type and range, whether this Mac and install can
/// have it (and why not), and how it bears on the others.
struct TuningOption: Identifiable, Equatable, Sendable {
    let key: String
    let type: String          // "bool" | "int" | "real"
    let available: Bool
    let why: String           // "not-installed", "needs-matrix-cores", ...
    let min: Double
    let max: Double
    let step: Double
    let excludes: [String]    // turned off while it is on
    let fixedSteps: Int?      // the steps while it is on (HyperFlow)
    let needs: String?        // the option it refines (Sol's threshold)
    // The LoRA list's: how many run at once (vpipe's slots), the catalog
    // Turbo LoRA's file ("" when not installed) and the names its entries
    // go by, and the preset's steps with the Turbo LoRA on, and off.
    let slots: Int
    let turbo: String
    let names: [String: String]
    let stepsOn: Int?
    let stepsOff: Int?
    /// An extension's own option (catalog `engine.vpipe.options`): the
    /// text it goes by, read by the core in the UI's language -- the app
    /// has none of its own for it -- and the group it is listed in. nil
    /// for Valtz's options.
    let declaredLabel: String?
    let declaredNote: String?
    let declaredHelp: String?
    let declaredGroup: String?
    var id: String { key }

    init(_ j: [String: Any]) {
        key = j["key"] as? String ?? ""
        type = j["type"] as? String ?? "bool"
        available = j["available"] as? Bool ?? false
        why = j["why"] as? String ?? ""
        min = j["min"] as? Double ?? 0
        max = j["max"] as? Double ?? 0
        step = j["step"] as? Double ?? 1
        excludes = j["excludes"] as? [String] ?? []
        fixedSteps = (j["fixes"] as? [String: Any])?["steps"] as? Int
        needs = j["needs"] as? String
        slots = j["slots"] as? Int ?? 2
        turbo = j["turbo"] as? String ?? ""
        names = j["names"] as? [String: String] ?? [:]
        stepsOn = j["steps_on"] as? Int
        stepsOff = j["steps_off"] as? Int
        let declared = j["declared"] as? Bool ?? false
        func text(_ k: String) -> String? {
            guard declared, let s = j[k] as? String, !s.isEmpty else { return nil }
            return s
        }
        declaredLabel = declared ? (text("label") ?? key) : nil
        declaredNote = text("note")
        declaredHelp = text("help")
        declaredGroup = text("group")
    }
}

/// A LoRA in Custom's list -- the catalog's Turbo LoRA among them, by its
/// file: up to two on (vpipe's two slots).
struct TuningLoRA: Equatable, Codable, Sendable, Identifiable {
    var path: String
    var scale: Double = 1
    var on = false
    var missing = false
    var id: String { path }
    var name: String { URL(fileURLWithPath: path).deletingPathExtension()
        .lastPathComponent }

    var json: [String: Any] {
        ["path": path, "scale": scale, "on": on]
    }
}

/// A community checkpoint for a model's DiT or VAE: the one on replaces
/// the model's own part.
struct TuningCheckpoint: Equatable, Codable, Sendable, Identifiable {
    var path: String
    var on = false
    var missing = false
    var id: String { path }
    /// Its file's name; a model's part folder ("transformer", "vae") with
    /// the model's: "Krea-2-Turbo/vae".
    var name: String {
        let url = URL(fileURLWithPath: path).deletingPathExtension()
        let last = url.lastPathComponent
        let parts = ["transformer", "vae", "unet", "dit"]
        guard parts.contains(last.lowercased()) else { return last }
        return url.deletingLastPathComponent().lastPathComponent + "/" + last
    }
    var json: [String: Any] { ["path": path, "on": on] }
}

/// Favor's option values: switches, numbers, the LoRA list and the
/// community DiT / VAE checkpoints, by key.
struct Tuning: Equatable, Codable, Sendable {
    var flags: [String: Bool] = [:]
    var numbers: [String: Double] = [:]
    /// An older save's Turbo LoRA selector ("turbo": "" | id | file): sent
    /// as it is, the core folds it into the LoRA list, and what it settles
    /// has none.
    var choices: [String: String] = [:]
    var loras: [TuningLoRA] = []
    /// "dits", "vaes": each part's community checkpoints.
    var checkpoints: [String: [TuningCheckpoint]] = [:]
    /// The keys whose numbers are whole (sent as integers).
    var integers: Set<String> = []

    init() {}

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        flags = try c.decodeIfPresent([String: Bool].self, forKey: .flags) ?? [:]
        numbers = try c.decodeIfPresent([String: Double].self,
                                        forKey: .numbers) ?? [:]
        choices = try c.decodeIfPresent([String: String].self,
                                        forKey: .choices) ?? [:]
        loras = try c.decodeIfPresent([TuningLoRA].self, forKey: .loras) ?? []
        checkpoints = try c.decodeIfPresent([String: [TuningCheckpoint]].self,
                                            forKey: .checkpoints) ?? [:]
        integers = try c.decodeIfPresent(Set<String>.self,
                                         forKey: .integers) ?? []
    }

    /// From the core's settled values, typed by its options.
    init(_ values: [String: Any], _ options: [TuningOption]) {
        for o in options {
            switch o.type {
            case "bool": flags[o.key] = values[o.key] as? Bool ?? false
            case "list" where o.key == "loras":
                loras = (values[o.key] as? [[String: Any]] ?? []).map {
                    TuningLoRA(path: $0["path"] as? String ?? "",
                               scale: $0["scale"] as? Double ?? 1,
                               on: $0["on"] as? Bool ?? false,
                               missing: $0["missing"] as? Bool ?? false)
                }
            case "list":
                checkpoints[o.key] = (values[o.key] as? [[String: Any]] ?? [])
                    .map {
                        TuningCheckpoint(path: $0["path"] as? String ?? "",
                                         on: $0["on"] as? Bool ?? false,
                                         missing: $0["missing"] as? Bool
                                             ?? false)
                    }
            default:
                if let v = values[o.key] as? Double { numbers[o.key] = v }
                if o.type == "int" { integers.insert(o.key) }
            }
        }
    }

    /// As a request carries it.
    var json: [String: Any] {
        var out: [String: Any] = [:]
        for (k, v) in flags { out[k] = v }
        for (k, v) in numbers {
            out[k] = integers.contains(k) ? Int(v.rounded()) as Any : v as Any
        }
        for (k, v) in choices { out[k] = v }
        out["loras"] = loras.map(\.json)
        for (k, v) in checkpoints { out[k] = v.map(\.json) }
        return out
    }
}

/// Favor's options for one model: what the core settles, and the options
/// as it offers them, in the order a panel lists them.
struct TuningInfo: Equatable, Sendable {
    let family: String
    let tuning: Tuning
    let options: [TuningOption]

    /// The LoRA list's option (its slots, the Turbo LoRA's file, names).
    var loraOption: TuningOption? { options.first { $0.key == "loras" } }
    /// The catalog Turbo LoRA's file, or "".
    var turboLoRA: String { loraOption?.turbo ?? "" }

    /// What a LoRA goes by: the catalog's name, else its file's.
    func name(_ l: TuningLoRA) -> String { loraOption?.names[l.path] ?? l.name }
}

/// One layer of a picture's stack (core project::Layer): "" is the
/// bottom one; `own` shows the picture's own image, `source` another
/// picture -- the content made on it -- `markup` what the markup toolbar
/// put on it; none of them, nothing yet. A `mask` masks the layer beneath
/// it and is not drawn.
struct LayerDTO: Decodable, Sendable, Hashable, Identifiable {
    var id: String
    var name: String
    var visible: Bool
    var own: Bool
    var source: String?
    /// The markup it shows (a markup asset's content; the app fills it
    /// in as the assets load -- AppModel.reloadAssets).
    var markup: MarkupDTO? = nil
    var mask = false
    var time: LayerTimeDTO? = nil
    /// The folder it is in (AssetDTO.layerFolders); nil: none.
    var folder: String? = nil

    /// What it shows: a name given, or "Layer 0" (id ""), "Layer 2".
    var title: String {
        if !name.isEmpty { return name }
        return String(localized: "Layer \(id.isEmpty ? "0" : id)")
    }
    var isEmpty: Bool { !own && source == nil && markup == nil }
    var isMarkup: Bool { markup != nil }

    init(id: String, name: String, visible: Bool, own: Bool,
         source: String?) {
        (self.id, self.name, self.visible, self.own, self.source) =
            (id, name, visible, own, source)
    }

    private enum K: String, CodingKey {
        case id, name, visible, own, source, markup, mask, time, folder
    }

    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: K.self)
        id = try c.decode(String.self, forKey: .id)
        name = (try? c.decodeIfPresent(String.self, forKey: .name))
            .flatMap { $0 } ?? ""
        visible = (try? c.decodeIfPresent(Bool.self, forKey: .visible))
            .flatMap { $0 } ?? true
        own = (try? c.decodeIfPresent(Bool.self, forKey: .own))
            .flatMap { $0 } ?? false
        source = (try? c.decodeIfPresent(String.self, forKey: .source))
            .flatMap { $0 }
        markup = (try? c.decodeIfPresent(MarkupDTO.self, forKey: .markup))
            .flatMap { $0 }
        mask = (try? c.decodeIfPresent(Bool.self, forKey: .mask))
            .flatMap { $0 } ?? false
        time = (try? c.decodeIfPresent(LayerTimeDTO.self, forKey: .time))
            .flatMap { $0 }
        folder = (try? c.decodeIfPresent(String.self, forKey: .folder))
            .flatMap { $0 }.flatMap { $0.isEmpty ? nil : $0 }
    }
}

/// A folder of a composition's layers (core project::LayerFolder): its
/// layers lie together in the stack, each naming it.
struct LayerFolderDTO: Decodable, Sendable, Hashable, Identifiable {
    var id: String
    var name: String
}

/// A generation refused for lack of memory (job.failed with "memory",
/// core `out_of_memory`): vpipe's numbers -- what the step asked for, part
/// by part, each budget's need and room, the resource plan by phase -- and
/// what to change, from the request: the clip's length, the size, the
/// references.
struct MemoryFailure: Sendable, Equatable {
    struct Part: Sendable, Equatable { let name: String; let bytes: UInt64 }
    struct Gate: Sendable, Equatable {
        let name: String
        let need: UInt64
        let have: UInt64
        let ok: Bool
    }

    let message: String
    /// "denoise" | "decode": where it was refused.
    let step: String
    let need: UInt64
    let parts: [Part]
    let gates: [Gate]
    /// The resource plan, before anything loaded: its peak and phases.
    let planPeak: UInt64
    let planPhase: String
    let phases: [Part]
    let ram: UInt64
    /// What it was asked for: change these.
    let seconds: Double?
    let width: Int?
    let height: Int?
    let references: Int?

    init?(_ p: [String: Any], message: String) {
        guard let memory = p["memory"] as? [String: Any] else { return nil }
        func bytes(_ a: Any?) -> UInt64 { (a as? NSNumber)?.uint64Value ?? 0 }
        func parts(_ a: Any?) -> [Part] {
            (a as? [[String: Any]] ?? []).map {
                Part(name: $0["name"] as? String ?? "", bytes: bytes($0["bytes"]))
            }
        }
        let refusal = memory["refusal"] as? [String: Any] ?? [:]
        let plan = memory["plan"] as? [String: Any] ?? [:]
        let suggest = p["suggest"] as? [String: Any] ?? [:]
        self.message = message
        step = refusal["step"] as? String ?? ""
        need = bytes(refusal["need"])
        self.parts = parts(refusal["parts"])
        gates = (refusal["gates"] as? [[String: Any]] ?? []).map {
            Gate(name: $0["name"] as? String ?? "", need: bytes($0["need"]),
                 have: bytes($0["have"]), ok: $0["ok"] as? Bool ?? true)
        }
        planPeak = bytes(plan["peak"])
        planPhase = plan["phase"] as? String ?? ""
        phases = parts(plan["phases"])
        ram = bytes(plan["ram"])
        let length = suggest["length"] as? [String: Any]
        seconds = (length?["seconds"] as? NSNumber)?.doubleValue
        let size = suggest["resolution"] as? [String: Any]
        width = (size?["width"] as? NSNumber)?.intValue
        height = (size?["height"] as? NSNumber)?.intValue
        references = ((suggest["references"] as? [String: Any])?["count"]
            as? NSNumber)?.intValue
    }

    /// "22.2 GB", "512 MB": as text, never grouped.
    static func size(_ b: UInt64) -> String {
        let gb = Double(b) / Double(1 << 30)
        return gb >= 1 ? String(format: "%.1f GB", gb)
                       : String(format: "%.0f MB", Double(b) / Double(1 << 20))
    }

    /// vpipe's names for what a step holds, as a person reads them.
    static func partName(_ n: String) -> String {
        switch n {
        case "transformer": String(localized: "Transformer")
        case "vdn_branch": String(localized: "VDN branch")
        case "sol_attn": String(localized: "Sol-Attn")
        case "sage_attn": String(localized: "SageAttention")
        case "ane": String(localized: "ANE modules")
        case "decoded_frames": String(localized: "Decoded frames")
        case "frames_u8": String(localized: "Frames for the file")
        case "gpu_working_set": String(localized: "GPU working set")
        case "reclaimable_ram": String(localized: "Free memory")
        case "condition": String(localized: "Prompt and references")
        case "denoise": String(localized: "Denoising")
        case "decode": String(localized: "Decoding the frames")
        case "decode-audio": String(localized: "Decoding the sound")
        default: n
        }
    }
}
