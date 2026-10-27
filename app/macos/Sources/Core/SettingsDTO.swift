import Foundation

/// Settings › General › Memory's WIRED LIMIT (core Controller::wired_limit):
/// how much memory the local models may make unswappable. Bytes; `chosen`
/// 0 is Automatic -- `automatic` here; a limit set lies in `min`...`max`.
struct WiredLimit: Decodable, Sendable, Equatable {
    var chosen: Int64
    var automatic: Int64
    var min: Int64
    var max: Int64
    /// The engine's now (0 without one).
    var inForce: Int64
    /// Wired now.
    var used: Int64
    /// A lower limit waiting for the work under way.
    var pending: Bool
}

/// Settings › Capabilities (core Controller::capability_tree): the model
/// families, what each makes here, and every resource it runs with.
struct CapabilityTree: Decodable, Sendable {
    var families: [Family]
    var downloadRoot: String
    /// Where downloads come from (core download_sources): vpipe's model
    /// sources -- Hugging Face, ModelScope -- and the one chosen.
    var downloadSources: [DownloadSource]?
    var downloadSource: String?

    /// A place models are downloaded from (vpipe's model-source table).
    struct DownloadSource: Decodable, Sendable, Identifiable, Hashable {
        var name: String
        var label: String
        var id: String { name }
        /// As a person reads it: Valtz's own words for the sources it
        /// knows, vpipe's label for another.
        var title: String {
            switch name {
            case "huggingface": String(localized: "Hugging Face")
            case "modelscope": String(localized: "ModelScope")
            default: label
            }
        }
    }

    /// A source carrying a member's repo, under its path there.
    struct SourcePath: Decodable, Sendable, Hashable {
        var name: String
        var path: String
    }

    struct Family: Decodable, Sendable, Identifiable {
        var id: String
        var name: String
        var features: [Feature]
        var members: [Member]
    }

    /// "video-gen", "video-edit", "image-gen", "image-edit", "audio-gen",
    /// "speech-gen", "helper", "video-upscale", "image-upscale"; `why`
    /// "ready" | "download" |
    /// "memory" | "engine".
    struct Feature: Decodable, Sendable, Identifiable {
        var feature: String
        var available: Bool
        var why: String
        var id: String { feature }
    }

    struct Member: Decodable, Sendable, Identifiable {
        var model: String
        var label: String
        var name: String
        var role: String
        var hfPath: String
        var url: String
        var state: String       // installed | partial | missing
        var bytes: Int64
        var diskGb: Double
        var fits: Bool
        var minRamGb: Int
        var path: String
        var source: String      // valtz | vpipe | link | ""
        var license: String
        var notes: String
        /// Gated on Hugging Face (catalog `gated`): its license accepted
        /// there first, and an access token to download it.
        var gated: Bool?
        var isGated: Bool { gated == true }
        /// The download sources carrying its repo (nil: not downloaded --
        /// a quantized variant -- or a core without them).
        var sources: [SourcePath]?
        /// Its path on `source`; nil where that source does not carry it.
        func path(on source: String) -> String? {
            sources?.first { $0.name == source }?.path
        }
        /// A quantized variant (core catalog `quantize`): made here from
        /// its source -- which must be here first -- not downloaded.
        var quantize: Quantize?
        /// COMPRESSIBLE (core catalog `compress`, DESIGN §8b): what each
        /// width would take, or why not here.
        var compress: Compress?
        /// A compressed copy in its download's place.
        var compressed: Compressed?
        /// The parts that can run from Draw Things checkpoints (core
        /// catalog `check.drawthings`: "dit", "encoder", "vae"), and the
        /// ones it is linked to.
        var drawthings: [String]?
        var parts: [String: PartLink]?
        var takesDrawThings: Bool { !(drawthings ?? []).isEmpty }
        var id: String { model }
    }

    /// A part of a model linked apart -- a Draw Things checkpoint -- and
    /// whether the file is still there.
    struct PartLink: Decodable, Sendable {
        var path: String
        var present: Bool
    }

    struct Quantize: Decodable, Sendable {
        var from: String
        var fromName: String
        var bits: Int
        var groupSize: Int
        var ready: Bool
    }

    /// `why`: "" ready, "missing" (not here), "compressed" (already),
    /// "link" / "vpipe" (files Valtz did not download), "engine".
    struct Compress: Decodable, Sendable {
        var ready: Bool
        var why: String
        var widths: [Width]

        struct Width: Decodable, Sendable, Identifiable {
            var bits: Int
            var bytes: Int64
            var saves: Int64
            var id: Int { bits }
        }
    }

    /// A compressed copy: in its download's place, or kept BESIDE it
    /// (`originalKept`: a compress that kept the original, to try the
    /// copy first) -- in use or set aside for the original.
    struct Compressed: Decodable, Sendable {
        var bits: Int
        var originalBytes: Int64
        var bytes: Int64?
        var inUse: Bool?
        var originalKept: Bool?
        var copyInUse: Bool { inUse ?? true }
        var besideOriginal: Bool { originalKept ?? false }
    }
}

/// A model's files CHECKED (core models/model-check.h, DESIGN §8b): a
/// link's, before it is made, or what is installed -- the model found in
/// or around what was chosen, whether its files say they are it, and
/// what was found wrong or could not be told.
struct ModelCheck: Sendable, Equatable {
    struct Note: Sendable, Equatable {
        let level: String   // "error" | "warning" | "info"
        let text: String    // in the UI's language
    }
    let path: String
    let moved: String       // "down" | "up" | ""
    let confirmed: Bool
    let usable: Bool
    let linked: Bool?
    let notes: [Note]

    init?(_ j: [String: Any]?) {
        guard let j, let path = j["path"] as? String else { return nil }
        self.path = path
        moved = j["moved"] as? String ?? ""
        confirmed = j["confirmed"] as? Bool ?? false
        usable = j["usable"] as? Bool ?? false
        linked = j["linked"] as? Bool
        notes = (j["notes"] as? [[String: Any]] ?? []).map {
            Note(level: $0["level"] as? String ?? "info",
                 text: L10n.coreMessage($0))
        }
    }

    /// The headline: the first note (errors come first).
    var headline: Note? { notes.first }
}

/// Settings › Upscaling (core Controller::upscale_settings): an
/// upscaler -- FlashVSR+ or FlashVSR 1.1 for clips, VOSR for pictures --
/// and the arithmetic it runs with, as Favor's Custom lists a model's
/// options: settled by the core from Med's preset with the person's own
/// laid on.
struct UpscalerSettings: Identifiable, Equatable, Sendable {
    let id: String
    let name: String
    let kind: String        // "video" | "image"
    let state: String       // "installed", "not-installed", ...
    let runs: Bool          // what Render Upscaled runs for its kind now
    let custom: Bool        // anything set apart from the defaults
    let info: TuningInfo

    init?(_ j: [String: Any]) {
        guard let id = j["id"] as? String,
              let t = j["tuning"] as? [String: Any],
              let values = t["values"] as? [String: Any],
              let opts = t["options"] as? [[String: Any]] else { return nil }
        self.id = id
        name = j["name"] as? String ?? id
        kind = j["kind"] as? String ?? "image"
        state = j["state"] as? String ?? ""
        runs = j["runs"] as? Bool ?? false
        custom = j["custom"] as? Bool ?? false
        let options = opts.map(TuningOption.init)
        info = TuningInfo(family: t["family"] as? String ?? "",
                          tuning: Tuning(values, options), options: options)
    }

    /// What the page lists: the options this upscaler takes at all.
    var options: [TuningOption] {
        info.options.filter { $0.why != "not-for-model" && $0.type != "list" }
    }
}

/// Which upscaler a kind runs (core Controller::upscale_settings'
/// "kinds"): the person's pick ("" Auto), Auto's -- the best installed --
/// and what Render Upscaled runs now (the pick, else Auto's).
struct UpscalerKind: Equatable, Sendable {
    let kind: String        // "video" | "image"
    let chosen: String
    let auto: String
    let runs: String

    init?(_ j: [String: Any]) {
        guard let kind = j["kind"] as? String else { return nil }
        self.kind = kind
        chosen = j["chosen"] as? String ?? ""
        auto = j["auto"] as? String ?? ""
        runs = j["runs"] as? String ?? ""
    }
}

/// Settings › Agentic Helper (core Controller::assistants): the models
/// that write and read prompts with the person, the one chosen ("" Auto),
/// Auto's pick and the one in use -- Auto's while the chosen one is not
/// here.
struct HelperList: Decodable, Sendable {
    var choice: String
    var auto: String
    var using: String
    /// Seconds it stays loaded after a request (vpipe's warm hold); 0:
    /// unloaded with each.
    var keepLoaded: Double?
    /// What it drafts with: "mtp" (its MTP head) or "dflash" (a DFlash 2
    /// block drafter, held at `drafterBits` in memory).
    var drafter: String?
    var drafterBits: Int?
    /// A video summary's FRAME INTERVAL (DESIGN §4i): as set (0: Auto --
    /// by the clip's length), what it comes to for a clip of no known
    /// length, and the memory bandwidth Auto goes by (GB/s); its FRAME
    /// SIZE, "standard" | "high" | "highest".
    var videoEvery: Double?
    var videoEveryNow: Double?
    var videoSize: String?
    var memoryBandwidthGbs: Double?
    /// Its context at most, in tokens, as set (0: Auto -- each helper's
    /// own, as far as this Mac's memory holds it).
    var context: Int?
    var models: [Helper]

    struct Helper: Decodable, Sendable, Identifiable {
        var id: String
        var name: String
        var state: String       // installed | partial | missing
        var fits: Bool
        var diskGb: Double
        var minRamGb: Int
        var rank: Int
        /// It decodes with a multi-token-prediction drafter: its own head,
        /// or one shipped apart (`drafter`).
        var mtp: Bool
        var drafter: Drafter?
        /// A DFlash 2 block drafter it can decode with instead.
        var dflash: Drafter?
        var sampling: Sampling
        /// Its context: its own, what this Mac's memory holds of it
        /// (Auto), and what it is given; installed only.
        var context: Context?
    }

    struct Context: Decodable, Sendable, Equatable {
        var model: Int
        var auto: Int
        /// The most of it this Mac's memory holds beside what the model
        /// holds -- more than Auto's where Auto is held below it
        /// (DeepSeek V4.1 Flash: 131,072 of 1,048,576).
        var fits: Int?
        var here: Int
    }

    struct Drafter: Decodable, Sendable {
        var id: String
        var name: String
        var state: String
        var diskGb: Double?
        /// A DFlash 2 drafter's memory beside the model, GB, by the bits
        /// it is held at ("8", "4").
        var heldGb: [String: Double]?
    }

    /// The sampler its chat decodes with (catalog engine.vpipe.sampling,
    /// the model card's recommendation); none: greedy.
    struct Sampling: Decodable, Sendable {
        var temperature: Double?
        var topP: Double?
        var topK: Int?
        var minP: Double?
        var presencePenalty: Double?
        var repetitionPenalty: Double?
    }

    func helper(_ id: String) -> Helper? { models.first { $0.id == id } }
}

/// Settings › Storage (core Controller::storage_report).
struct StorageReport: Decodable, Sendable {
    var volume: Volume
    var models: Models
    var projects: Projects
    var cache: Cache

    struct Volume: Decodable, Sendable {
        var path: String
        var capacity: Int64
        var free: Int64
    }
    struct Models: Decodable, Sendable {
        var root: String
        var bytes: Int64
        var items: [ModelItem]
    }
    struct ModelItem: Decodable, Sendable, Identifiable {
        var repo: String
        var names: [String]
        var bytes: Int64
        /// When its folder was made, ms since 1970 (0 unknown).
        var created: Int64?
        var id: String { repo }
    }
    struct Projects: Decodable, Sendable {
        var root: String
        var bytes: Int64
        var items: [Project]
    }
    struct Project: Decodable, Sendable, Identifiable {
        var name: String
        var path: String
        var bytes: Int64
        var created: Int64?
        var open: Bool
        var ephemeral: Bool
        var assets: [Asset]?
        var other: Int64?
        var id: String { path }
    }
    struct Asset: Decodable, Sendable, Identifiable {
        var id: String
        var name: String
        var kind: String
        var bytes: Int64
        var linked: Bool
        var created: Int64?
    }
    struct Cache: Decodable, Sendable {
        var root: String
        var bytes: Int64
        var budget: Int64
    }
}

/// A row of the Log view (core LogBook): what the engine (vpipe) or Valtz
/// reported.
struct LogRow: Decodable, Sendable, Identifiable {
    var seq: UInt64
    var time: Int64      // ms since 1970
    var level: String    // error | warn | info | debug
    var source: String   // vpipe | valtz
    var text: String
    var id: UInt64 { seq }
}
