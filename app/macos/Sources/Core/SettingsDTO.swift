import Foundation

/// Settings › Capabilities (core Controller::capability_tree): the model
/// families, what each makes here, and every resource it runs with.
struct CapabilityTree: Decodable, Sendable {
    var families: [Family]
    var downloadRoot: String

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
        /// A quantized variant (core catalog `quantize`): made here from
        /// its source -- which must be here first -- not downloaded.
        var quantize: Quantize?
        var id: String { model }
    }

    struct Quantize: Decodable, Sendable {
        var from: String
        var fromName: String
        var bits: Int
        var groupSize: Int
        var ready: Bool
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
    }

    struct Drafter: Decodable, Sendable {
        var id: String
        var name: String
        var state: String
        var diskGb: Double?
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
