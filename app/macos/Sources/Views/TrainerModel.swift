import AppKit
import SwiftUI

/// The LoRA TRAINER's state (Settings › Experimental; DESIGN §8c): what
/// it trains on -- the pictures dropped on it, each one's caption -- how
/// (vpipe's train-lora, its dataset's and optimizer's keys), and the
/// training at work, followed from its events. Not a project's: a
/// window's, and the runs kept in the models folder (core
/// Controller::trained_loras).
@MainActor @Observable
final class TrainerModel {
    struct ModelChoice: Identifiable, Equatable {
        let id: String
        let name: String
        let installed: Bool
        let defaults: [String: Any]
        static func == (a: Self, b: Self) -> Bool {
            a.id == b.id && a.installed == b.installed
        }
    }

    /// A picture to train on and its caption ("" none: the trigger alone,
    /// or as Missing Caption says).
    struct Item: Identifiable, Equatable {
        let id = UUID()
        let url: URL
        var caption: String
        /// What it is made from (Ref2VA's references, an edit's source
        /// first) -- given here, or the .refs beside it.
        var references: [URL] = []
        /// Its caption written by the agent's model (auto captions) --
        /// the model's name; "" the person's or its .txt.
        var captionedBy = ""
        var name: String { url.lastPathComponent }
        var isClip: Bool {
            TrainerModel.clipTypes.contains(url.pathExtension.lowercased())
        }
    }

    struct Checkpoint: Identifiable, Equatable {
        let step: Int
        let path: String
        let loss: Double
        var id: String { path }
    }

    struct Run: Identifiable, Equatable {
        let name: String
        let model: String
        let trigger: String
        let folder: String
        let adapter: String
        let checkpoints: Int
        let previews: [String]
        let pictures: Int
        let resumable: Bool
        let training: Bool
        let request: [String: Any]
        let parent: String
        var id: String { name }
        static func == (a: Self, b: Self) -> Bool {
            a.name == b.name && a.adapter == b.adapter
                && a.checkpoints == b.checkpoints && a.previews == b.previews
                && a.training == b.training && a.resumable == b.resumable
        }
    }

    // MARK: Configuration (vpipe's keys; 0 / "" its own choice)

    var modelId = ""
    var name = ""
    /// Where a new run's folder goes ("": the models folder's trained/),
    /// kept between launches.
    var folder = UserDefaults.standard.string(forKey: TrainerModel.folderKey)
        ?? "" {
        didSet {
            UserDefaults.standard.set(folder, forKey: Self.folderKey)
        }
    }
    static let folderKey = "trainer.folder"
    private(set) var defaultFolder = ""
    var trigger = "ohwx"
    /// AUTO CAPTIONS: what the trigger stands for, in the person's words
    /// (asked for the first time samples come without captions), and
    /// whether a sample without a caption is captioned by the agent's
    /// model as it comes (kept between launches).
    var triggerMeaning = "" {
        didSet { if triggerMeaning != oldValue { askedMeaning = false } }
    }
    var autoCaption = UserDefaults.standard.object(
        forKey: "trainer.autoCaption") as? Bool ?? true {
        didSet { UserDefaults.standard.set(autoCaption,
                                           forKey: "trainer.autoCaption") }
    }
    /// The question "what does ohwx mean?" open (the trainer's sheet).
    var askingMeaning = false
    /// Asked once and put off ("Not Now"): not asked again until the
    /// meaning changes or Caption Missing is pressed.
    @ObservationIgnored private var askedMeaning = false
    /// The samples being captioned now, and the one the model is on.
    private(set) var captionQueue: [Item.ID] = []
    private(set) var captioningNow: Item.ID?
    private(set) var captionError: String?
    /// Who captions here (the core's choice: the agent's model where it
    /// sees, else the micro agent, else the helper); "" none.
    private(set) var captioner = ""
    var captioning: Bool { captioningNow != nil || !captionQueue.isEmpty }
    var uncaptioned: [Item] { items.filter { $0.caption.isEmpty } }
    var captionTemplate = "{trigger}, {caption}"
    var missingCaption = "trigger_only"
    var resolutions: Set<Int> = [512, 768, 1024]
    var offeredResolutions: [Int] = [512, 768, 1024, 1328]
    var bucketStep = 32
    var maxAspect = 2.0
    var flip = false
    var shuffleTags = false
    var keepTags = 1
    var repeats = 1
    var previewPrompts = "{trigger}\na photo of {trigger} in a snowy park"

    enum Length: String, CaseIterable, Identifiable {
        case auto, steps, epochs
        var id: String { rawValue }
    }
    var length: Length = .auto
    var steps = 600
    var epochs = 2
    var batch = 1
    var rank = 16
    var alpha = 0           // 0: the rank's
    var targets = "attn_ff"
    var optimizer = "adamw"
    var learningRate = 0.0  // 0: the optimizer's own
    var lrSchedule = "auto"
    var warmup = -1         // -1: 2% of the run
    var gradClip = 1.0
    var ema = 0.0
    var captionDropout = 0.05
    var timestepSampling = "auto"
    var seed = 0
    var saveEvery = 0       // 0: automatic
    var previewSteps = 20
    var previewCfg = 4.0
    var recompute = "auto"
    var int8 = false
    var ane = false
    var loraPlus = 1.0      // 1: off
    // A clip model's (MiniMax H3): each clip's windows, their soundtrack,
    // FL2VA's keyframe tasks by weight.
    var frames = 39
    var windows = 2
    var soundtrack = true
    var audioLossWeight = 1.0
    var taskT2VA = 1.0
    var taskI2VA = 0.0
    var taskL2VA = 0.0
    var taskFL2VA = 0.0

    // MARK: The dataset

    var items: [Item] = []
    var selected: Item.ID?
    /// The title bar's search: the samples whose file name or caption
    /// holds every word typed, case and accents aside.
    var query = ""
    var shownItems: [Item] {
        let words = query.split(whereSeparator: \.isWhitespace)
            .map { $0.folding(options: [.caseInsensitive, .diacriticInsensitive],
                              locale: nil) }
        guard !words.isEmpty else { return items }
        return items.filter { it in
            let hay = (it.name + " " + it.caption).folding(
                options: [.caseInsensitive, .diacriticInsensitive],
                locale: nil)
            return words.allSatisfy { hay.contains($0) }
        }
    }

    // MARK: The trainer's world

    private(set) var models: [ModelChoice] = []
    private(set) var ready = false
    private(set) var matrixCores = false
    private(set) var runs: [Run] = []
    /// A section of Settings scrolled into sight (the hooks').
    var scrollTo: String?
    /// The settings island's tab.
    var tab: TrainerTab = .settings
    /// The settings island's width and the progress island's height, as
    /// their edges were dragged -- kept between launches.
    static let widthRange: ClosedRange<Double> = 300...640
    static let heightRange: ClosedRange<Double> = 170...520
    var islandWidth = TrainerModel.stored("trainer.islandWidth", 360,
                                          TrainerModel.widthRange) {
        didSet { UserDefaults.standard.set(islandWidth,
                                           forKey: "trainer.islandWidth") }
    }
    var progressHeight = TrainerModel.stored("trainer.progressHeight", 232,
                                             TrainerModel.heightRange) {
        didSet { UserDefaults.standard.set(progressHeight,
                                           forKey: "trainer.progressHeight") }
    }
    private static func stored(_ key: String, _ d: Double,
                               _ r: ClosedRange<Double>) -> Double {
        let v = UserDefaults.standard.double(forKey: key)
        return v > 0 ? min(max(v, r.lowerBound), r.upperBound) : d
    }
    /// The loss curve's steps shown (nil: all of them).
    var lossRange: ClosedRange<Double>?

    // MARK: The training at work

    private(set) var job: String?
    private(set) var phase = ""      // "prepare" | "train-encode" | "train"
    private(set) var done = 0
    private(set) var total = 0
    private(set) var detail = ""
    private(set) var losses: [Double] = []
    private(set) var stepSeconds = 0.0
    private(set) var lr = 0.0
    private(set) var checkpoints: [Checkpoint] = []
    private(set) var previews: [String] = []
    private(set) var started: Date?
    private(set) var outcome: String?     // the last training's end, in words
    private(set) var failed = false
    private(set) var commanding = false
    /// The preview chosen for the stage (a training's; nil: the newest).
    var shownPreview: String?
    /// What the stage shows: the sample selected, or a preview.
    enum StageSource: Hashable { case sample, preview }
    var stageShows: StageSource = .sample
    /// The stage's path, as it shows.
    var stagePath: String? {
        switch stageShows {
        case .sample: return selectedItem?.url.path
        case .preview: return shownPreview ?? previews.last
        }
    }
    /// A preview clicked: on the stage.
    func showPreview(_ p: String) {
        shownPreview = p
        stageShows = .preview
    }
    /// A sample clicked: on the stage.
    func showSample(_ id: Item.ID) {
        selected = id
        stageShows = .sample
    }

    // The stage's ZOOM (the title bar's zoom group, ⌘− ⌘0 ⌘9 ⌘+): nil
    // fits the picture to the stage; else its scale, 1 its own pixels.
    var stageZoom: Double?
    /// The scale that fits, as the stage last laid the picture out.
    var fitScale = 1.0
    var stageScale: Double { stageZoom ?? fitScale }
    static let zoomLadder: [Double] =
        [0.1, 0.25, 0.33, 0.5, 0.67, 0.75, 1, 1.5, 2, 3, 4, 6, 8]
    func zoom(_ step: ZoomStep) {
        let now = stageScale
        switch step {
        case .fit: stageZoom = nil
        case .actualSize: stageZoom = 1
        case .zoomIn:
            stageZoom = Self.zoomLadder.first { $0 > now + 1e-6 } ?? 8
        case .zoomOut:
            stageZoom = Self.zoomLadder.last { $0 < now - 1e-6 } ?? 0.1
        }
    }

    @ObservationIgnored private weak var app: AppModel?
    @ObservationIgnored private var runName = ""
    /// Stop (not Pause): the run is ended once its job has stopped.
    @ObservationIgnored private var endAfterStop = false

    init(app: AppModel) { self.app = app }

    private var core: CoreService? { app?.core }

    var model: ModelChoice? { models.first { $0.id == modelId } }
    /// The model takes clips (MiniMax H3's video partitions).
    var takesClips: Bool {
        (model?.defaults["media"] as? [String])?.contains("clips") ?? false
    }
    /// Its samples carry references: "optional" (Ref2VA's), "edit" (an
    /// edit's source, MiniMax-H3-Image), nil none.
    var referenceKind: String? { model?.defaults["references"] as? String }
    /// FL2VA's keyframe tasks.
    var takesKeyframes: Bool { model?.defaults["keyframes"] as? Bool ?? false }
    /// MiniMax H3: guidance-distilled, its previews take no CFG.
    var guidanceFree: Bool { model?.defaults["shape"] != nil }
    var training: Bool { job != nil }
    /// The kept run of the name in the trainer, if any.
    var currentRun: Run? { runs.first { $0.name == name } }
    /// Stopped with its state kept: Start resumes it.
    var resumable: Bool { !training && currentRun?.resumable == true }
    /// Where the run's folder is, or goes.
    var whereShown: String {
        if let r = currentRun, !r.parent.isEmpty { return r.parent }
        return folder.isEmpty ? defaultFolder : folder
    }
    var selectedItem: Item? { items.first { $0.id == selected } }

    /// What Start needs, in words; nil when it can.
    var blocker: String? {
        if !ready {
            return String(localized: "This engine has no LoRA trainer.")
        }
        guard let m = model else {
            return String(localized: "Choose a model to train on.")
        }
        if !m.installed {
            return String(localized: "\(m.name) is not downloaded: download it in Settings › Capabilities.")
        }
        if name.trimmingCharacters(in: .whitespaces).isEmpty {
            return String(localized: "Name the LoRA.")
        }
        if items.isEmpty {
            return takesClips
                ? String(localized: "Drop pictures, clips or folders of them here.")
                : String(localized: "Drop pictures or folders of them here.")
        }
        if resolutions.isEmpty {
            return String(localized: "Choose a resolution at least.")
        }
        if captioning {
            return String(localized: "Captioning the samples: Start waits for them.")
        }
        return nil
    }

    /// The trainer's models and runs read again.
    func reload() {
        guard let core else { return }
        let r = core.trainerInfo()
        guard r.ok, let t = r["trainer"] as? [String: Any] else { return }
        ready = t["ready"] as? Bool ?? false
        defaultFolder = t["default_folder"] as? String ?? ""
        matrixCores = ((t["mac"] as? [String: Any])?["matrix_cores"]
                       as? Bool) ?? false
        models = (t["models"] as? [[String: Any]] ?? []).map {
            ModelChoice(id: $0["id"] as? String ?? "",
                        name: $0["name"] as? String ?? "",
                        installed: $0["installed"] as? Bool ?? false,
                        defaults: $0["defaults"] as? [String: Any] ?? [:])
        }
        if model == nil, let first = models.first(where: \.installed)
            ?? models.first {
            choose(first.id)
        }
        readRuns(t["runs"] as? [[String: Any]] ?? [])
        captioner = (t["captioner"] as? [String: Any])?["name"] as? String
            ?? ""
    }

    /// A model chosen: its catalog defaults where they are given.
    func choose(_ id: String) {
        modelId = id
        guard let d = model?.defaults else { return }
        if let r = d["resolutions"] as? [Int], !r.isEmpty {
            offeredResolutions = r
        }
        if let r = d["resolution"] as? [Int], !r.isEmpty {
            resolutions = Set(r)
        }
        if let n = d["rank"] as? Int { rank = n }
        if let t = d["targets"] as? String { targets = t }
        if let o = d["optimizer"] as? String { optimizer = o }
        if let lr = (d["learning_rate"] as? NSNumber)?.doubleValue {
            learningRate = lr
        }
        if let t = d["trigger"] as? String, trigger.isEmpty { trigger = t }
        if let p = d["preview_prompts"] as? [String], !p.isEmpty {
            previewPrompts = p.joined(separator: "\n")
        }
        if let f = d["frames"] as? [Int], let first = f.first { frames = first }
        if let w = d["windows"] as? Int { windows = w }
        if let s = d["preview_steps"] as? Int { previewSteps = s }
        // A picture model takes no clip: those added before go.
        if !takesClips { items.removeAll(where: \.isClip) }
        if selected.map({ id in !items.contains { $0.id == id } }) ?? true {
            selected = items.first?.id
        }
    }

    private func readRuns(_ list: [[String: Any]]) {
        runs = list.map {
            Run(name: $0["name"] as? String ?? "",
                model: $0["model_name"] as? String
                    ?? $0["model"] as? String ?? "",
                trigger: $0["trigger"] as? String ?? "",
                folder: $0["folder"] as? String ?? "",
                adapter: $0["adapter"] as? String ?? "",
                checkpoints: ($0["checkpoints"] as? [Any])?.count ?? 0,
                previews: $0["previews"] as? [String] ?? [],
                pictures: ($0["items"] as? [Any])?.count ?? 0,
                resumable: $0["resumable"] as? Bool ?? false,
                training: !($0["job"] as? String ?? "").isEmpty,
                request: $0["request"] as? [String: Any] ?? [:],
                parent: $0["parent"] as? String ?? "")
        }
    }

    // MARK: The dataset

    nonisolated static let pictureTypes: Set<String> =
        ["png", "jpg", "jpeg", "webp", "bmp", "tif", "tiff"]
    /// What vpipe's training-dataset reads as a clip.
    nonisolated static let clipTypes: Set<String> =
        ["mp4", "mov", "mkv", "webm", "avi", "m4v"]
    private func takes(_ u: URL) -> Bool {
        let e = u.pathExtension.lowercased()
        return Self.pictureTypes.contains(e)
            || (takesClips && Self.clipTypes.contains(e))
    }

    /// The .refs beside a sample: a path a line, relative to its folder.
    static func refs(beside u: URL) -> [URL] {
        let side = u.deletingPathExtension().appendingPathExtension("refs")
        guard let text = try? String(contentsOf: side, encoding: .utf8) else {
            return []
        }
        return text.split(whereSeparator: \.isNewline).compactMap { l in
            let line = l.trimmingCharacters(in: .whitespaces)
            guard !line.isEmpty, !line.hasPrefix("#") else { return nil }
            return line.hasPrefix("/") ? URL(fileURLWithPath: line)
                : u.deletingLastPathComponent().appendingPathComponent(line)
        }
    }

    /// Files and folders dropped (or chosen): each picture in the list
    /// once, a folder's in name order, each with the caption of the .txt
    /// beside it. The number taken.
    @discardableResult
    func add(_ urls: [URL]) -> Int {
        var found: [URL] = []
        let fm = FileManager.default
        for url in urls {
            var dir: ObjCBool = false
            guard fm.fileExists(atPath: url.path, isDirectory: &dir) else {
                continue
            }
            if dir.boolValue {
                let all = fm.enumerator(at: url, includingPropertiesForKeys:
                                            nil, options: [.skipsHiddenFiles])
                var inside: [URL] = []
                while let u = all?.nextObject() as? URL {
                    // A references folder holds what samples are made
                    // from, never samples (as vpipe reads one).
                    if u.lastPathComponent == "references" {
                        all?.skipDescendants()
                        continue
                    }
                    if takes(u) { inside.append(u) }
                }
                found += inside.sorted { $0.path < $1.path }
            } else if takes(url) {
                found.append(url)
            }
        }
        let have = Set(items.map { $0.url.standardizedFileURL })
        var n = 0
        for u in found where !have.contains(u.standardizedFileURL) {
            let side = u.deletingPathExtension().appendingPathExtension("txt")
            let caption = (try? String(contentsOf: side, encoding: .utf8))?
                .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
            items.append(Item(url: u, caption: caption,
                              references: Self.refs(beside: u)))
            n += 1
        }
        if selected == nil { selected = items.first?.id }
        if n > 0 { captionMissing(asked: false) }
        if name.isEmpty, let first = urls.first {
            var dir: ObjCBool = false
            FileManager.default.fileExists(atPath: first.path,
                                           isDirectory: &dir)
            name = dir.boolValue ? first.lastPathComponent
                : first.deletingLastPathComponent().lastPathComponent
        }
        return n
    }

    func remove(_ ids: Set<Item.ID>) {
        items.removeAll { ids.contains($0.id) }
        captionQueue.removeAll { ids.contains($0) }
        if let s = selected, ids.contains(s) { selected = items.first?.id }
    }

    func setCaption(_ id: Item.ID, _ text: String) {
        guard let i = items.firstIndex(where: { $0.id == id }) else { return }
        items[i].caption = text
        items[i].captionedBy = ""
        // Typed over: no longer waiting for the model.
        captionQueue.removeAll { $0 == id }
    }

    // MARK: Auto captions

    /// The samples without a caption captioned by the agent's model, one
    /// after another off the main thread -- each as it finishes, the
    /// person's typing never overwritten. Without the trigger's meaning,
    /// it is asked for first (`asked`: Caption Missing pressed, asked
    /// again even if put off).
    func captionMissing(asked: Bool = true) {
        guard asked || autoCaption, !training else { return }
        let missing = uncaptioned.map(\.id).filter {
            !captionQueue.contains($0) && $0 != captioningNow
        }
        guard !missing.isEmpty else { return }
        if captioner.isEmpty {
            if asked {
                captionError = String(localized: "No model here sees pictures to caption them: choose an Agent model that sees, or download the micro agent (LFM3-VL 2B), in Settings.")
            }
            return
        }
        if triggerMeaning.trimmingCharacters(in: .whitespaces).isEmpty {
            if asked || !askedMeaning {
                askedMeaning = true
                askingMeaning = true
            }
            return
        }
        captionError = nil
        captionQueue += missing
        if captioningNow == nil { captionNext() }
    }

    /// The question answered: the meaning kept, the captions begun.
    func answerMeaning(_ text: String) {
        triggerMeaning = text.trimmingCharacters(in: .whitespacesAndNewlines)
        askingMeaning = false
        askedMeaning = true
        captionMissing(asked: false)
    }
    func putOffMeaning() {
        askingMeaning = false
        askedMeaning = true
    }

    /// The captions waiting dropped (removed samples, Stop Captioning).
    func stopCaptioning() { captionQueue.removeAll() }

    private func captionNext() {
        guard let core, !captionQueue.isEmpty else {
            captioningNow = nil
            return
        }
        let id = captionQueue.removeFirst()
        guard let item = items.first(where: { $0.id == id }),
              item.caption.isEmpty else {
            captionNext()
            return
        }
        captioningNow = id
        let path = item.url.path, t = trigger, m = triggerMeaning
        Task.detached(priority: .userInitiated) { [weak self] in
            let r = core.captionTrainingSample(path, trigger: t, meaning: m)
            await MainActor.run {
                guard let self else { return }
                if r.ok, let c = r["caption"] as? String, !c.isEmpty,
                   let i = self.items.firstIndex(where: { $0.id == id }),
                   self.items[i].caption.isEmpty {
                    self.items[i].caption = c
                    self.items[i].captionedBy = r["model"] as? String ?? ""
                } else if !r.ok {
                    self.captionError = r.message
                }
                self.captioningNow = nil
                self.captionNext()
            }
        }
    }

    /// A sample's references added (pictures or clips), or one taken out.
    func addReferences(_ id: Item.ID, _ urls: [URL]) {
        guard let i = items.firstIndex(where: { $0.id == id }) else { return }
        let ok = urls.filter {
            let e = $0.pathExtension.lowercased()
            return Self.pictureTypes.contains(e)
                || (referenceKind == "optional" && Self.clipTypes.contains(e))
        }
        for u in ok where !items[i].references.contains(u) {
            items[i].references.append(u)
        }
    }
    func removeReference(_ id: Item.ID, _ url: URL) {
        guard let i = items.firstIndex(where: { $0.id == id }) else { return }
        items[i].references.removeAll { $0 == url }
    }

    /// A kept run's settings and pictures back in the trainer: to train
    /// on further (its name resumes it) or again.
    func load(_ run: Run) {
        let r = run.request
        name = run.name
        if let m = r["model"] as? String { modelId = m }
        let ds = r["dataset"] as? [String: Any] ?? [:]
        let opt = r["optimizer"] as? [String: Any] ?? [:]
        let tr = r["train"] as? [String: Any] ?? [:]
        trigger = ds["trigger_word"] as? String ?? ""
        triggerMeaning = r["trigger_meaning"] as? String ?? ""
        captionTemplate = ds["caption_template"] as? String
            ?? "{trigger}, {caption}"
        missingCaption = ds["missing_caption"] as? String ?? "trigger_only"
        if let res = ds["resolution"] as? [Int] { resolutions = Set(res) }
        if let res = ds["resolution"] as? Int { resolutions = [res] }
        flip = (ds["flip"] as? String) == "random"
        shuffleTags = ds["shuffle_tags"] as? Bool ?? false
        if let p = ds["preview_prompts"] as? [String] {
            previewPrompts = p.joined(separator: "\n")
        }
        repeats = r["repeats"] as? Int ?? 1
        rank = tr["rank"] as? Int ?? rank
        alpha = tr["alpha"] as? Int ?? 0
        targets = tr["targets"] as? String ?? "attn_ff"
        if let s = tr["steps"] as? Int {
            length = .steps
            steps = s
        } else if let e = tr["epochs"] as? Int {
            length = .epochs
            epochs = e
        } else {
            length = .auto
        }
        batch = tr["batch_size"] as? Int ?? 1
        seed = tr["seed"] as? Int ?? 0
        saveEvery = tr["save_every"] as? Int ?? 0
        int8 = tr["i8_gemm"] as? Bool ?? false
        ane = tr["ane"] as? Bool ?? false
        audioLossWeight = (tr["audio_loss_weight"] as? NSNumber)?
            .doubleValue ?? 1
        loraPlus = (opt["loraplus_lr_ratio"] as? NSNumber)?.doubleValue ?? 1
        if let f = ds["frames"] as? [Int], let first = f.first { frames = first }
        windows = ds["windows"] as? Int ?? windows
        soundtrack = (ds["audio"] as? String) != "none"
        let mix = ds["task_mix"] as? [String: Any] ?? [:]
        func w(_ k: String, _ d: Double) -> Double {
            (mix[k] as? NSNumber)?.doubleValue ?? (mix.isEmpty ? d : 0)
        }
        taskT2VA = w("t2va", 1)
        taskI2VA = w("i2va", 0)
        taskL2VA = w("l2va", 0)
        taskFL2VA = w("fl2va", 0)
        optimizer = opt["optimizer"] as? String ?? "adamw"
        learningRate = (opt["learning_rate"] as? NSNumber)?.doubleValue ?? 0
        lrSchedule = opt["lr_schedule"] as? String ?? "auto"
        items = (r["items"] as? [[String: Any]] ?? []).compactMap {
            guard let p = $0["path"] as? String,
                  FileManager.default.fileExists(atPath: p) else { return nil }
            return Item(url: URL(fileURLWithPath: p),
                        caption: $0["caption"] as? String ?? "",
                        references: ($0["references"] as? [String] ?? [])
                            .map { URL(fileURLWithPath: $0) })
        }
        selected = items.first?.id
        shownPreview = nil
        previews = run.previews
    }

    // MARK: The training

    /// The request as the core takes it (Controller::train_lora).
    var request: [String: Any] {
        var ds: [String: Any] = [
            "trigger_word": trigger,
            "caption_template": captionTemplate,
            "missing_caption": missingCaption,
            "resolution": resolutions.sorted(),
            "bucket_step": bucketStep,
            "max_aspect": maxAspect,
            "flip": flip ? "random" : "off",
            "preview_prompts": previewPrompts.split(separator: "\n")
                .map { $0.trimmingCharacters(in: .whitespaces) }
                .filter { !$0.isEmpty },
        ]
        if shuffleTags {
            ds["shuffle_tags"] = true
            ds["keep_tags"] = keepTags
        }
        if takesClips {
            ds["frames"] = [frames]
            ds["windows"] = windows
            ds["audio"] = soundtrack ? "track" : "none"
        }
        if takesKeyframes {
            var mix: [String: Double] = [:]
            for (k, v) in [("t2va", taskT2VA), ("i2va", taskI2VA),
                           ("l2va", taskL2VA), ("fl2va", taskFL2VA)]
            where v > 0 { mix[k] = v }
            if !mix.isEmpty, mix != ["t2va": 1] { ds["task_mix"] = mix }
        }
        var opt: [String: Any] = ["optimizer": optimizer,
                                  "lr_schedule": lrSchedule,
                                  "grad_clip": gradClip]
        if learningRate > 0 { opt["learning_rate"] = learningRate }
        if warmup >= 0 { opt["warmup_steps"] = warmup }
        if ema > 0 { opt["ema"] = ema }
        if loraPlus > 1 { opt["loraplus_lr_ratio"] = loraPlus }
        var tr: [String: Any] = [
            "rank": rank, "targets": targets, "batch_size": batch,
            "seed": seed, "caption_dropout": captionDropout,
            "timestep_sampling": timestepSampling,
            "preview_steps": previewSteps, "preview_cfg": previewCfg,
            "recompute": recompute,
        ]
        if alpha > 0 { tr["alpha"] = alpha }
        switch length {
        case .auto: break
        case .steps: tr["steps"] = steps
        case .epochs: tr["epochs"] = epochs
        }
        if saveEvery > 0 {
            tr["save_every"] = saveEvery
            tr["preview_every"] = saveEvery
        }
        if matrixCores { tr["i8_gemm"] = int8 } else { tr["ane"] = ane }
        if takesClips, soundtrack, audioLossWeight != 1 {
            tr["audio_loss_weight"] = audioLossWeight
        }
        if guidanceFree { tr.removeValue(forKey: "preview_cfg") }
        var where_: [String: Any] = [:]
        if !folder.isEmpty, currentRun == nil { where_["folder"] = folder }
        return where_.merging(
               ["model": modelId, "name": name, "repeats": repeats,
                "trigger_meaning": triggerMeaning,
                "items": items.map { it -> [String: Any] in
                    var d: [String: Any] = ["path": it.url.path,
                                            "caption": it.caption]
                    if referenceKind != nil {
                        d["references"] = it.references.map(\.path)
                    }
                    return d
                },
                "dataset": ds, "optimizer": opt, "train": tr]) { a, _ in a }
    }

    func start() {
        guard blocker == nil, let core, !training else { return }
        let r = core.trainLoRA(request)
        guard r.ok, let j = r["job"] as? String else {
            app?.flash(r.message, level: .error)
            return
        }
        job = j
        runName = name
        phase = "prepare"
        done = 0
        total = 0
        detail = ""
        losses = []
        checkpoints = []
        previews = runs.first { $0.name == name }?.previews ?? []
        started = Date()
        outcome = nil
        failed = false
        endAfterStop = false
        lossRange = nil
        reload()
    }

    /// Pause: the job stopped, its state kept -- Start (Resume) trains on.
    func pause() {
        endAfterStop = false
        stop()
    }

    /// Stop: the run ENDED -- its adapter kept, its resume state deleted,
    /// so a Start of its name trains afresh. A run at work is stopped
    /// first.
    func endRun() {
        if training {
            endAfterStop = true
            stop()
            return
        }
        guard let core, currentRun != nil else { return }
        let r = core.finishTrained(name)
        if !r.ok { app?.flash(r.message, level: .error) }
        outcome = String(localized: "Ended: its adapter is kept, and Start trains afresh.")
        reload()
    }

    /// Stop: vpipe saves the adapter and its state; the same name trains
    /// on from there.
    func stop() {
        guard let job, let core else { return }
        _ = core.cancel(job: job)
    }

    /// A checkpoint now, or the previews now, between two steps.
    func command(_ name: String) {
        guard let job, let core, !commanding else { return }
        commanding = true
        let args: [String: Any] = name == "preview" ? ["prompt": -1] : [:]
        Task.detached(priority: .userInitiated) { [weak self] in
            let r = core.trainCommand(job, name, args)
            await MainActor.run {
                guard let self else { return }
                self.commanding = false
                if !r.ok { self.app?.flash(r.message, level: .error) }
                self.readPreviews()
            }
        }
    }

    private func readPreviews() {
        guard let run = runs.first(where: { $0.name == runName })
            ?? runs.first(where: { $0.name == name }) else { return }
        let dir = URL(fileURLWithPath: run.folder)
            .appendingPathComponent("previews")
        let files = ((try? FileManager.default.contentsOfDirectory(
            atPath: dir.path)) ?? [])
            .filter { $0.hasSuffix(".png") || $0.hasSuffix(".mp4") }.sorted()
            .map { dir.appendingPathComponent($0).path }
        // A preview chosen stays chosen; with none chosen the stage
        // shows the newest (stagePath).
        if files != previews { previews = files }
    }

    /// A training's events (AppModel.handle): its job's alone.
    func handle(_ ev: CoreEvent) {
        guard let job, ev.job == job else { return }
        let p = ev.payload
        switch ev.kind {
        case "job.started":
            if phase.isEmpty { phase = "prepare" }
        case "job.progress":
            if let ph = p["phase"] as? String { phase = ph }
            if let m = p["metrics"] as? [String: Any] {
                done = m["step"] as? Int ?? done
                total = m["total"] as? Int ?? total
                if let l = (m["loss"] as? NSNumber)?.doubleValue {
                    losses.append(l)
                }
                stepSeconds = (m["sec"] as? NSNumber)?.doubleValue
                    ?? stepSeconds
                lr = (m["lr"] as? NSNumber)?.doubleValue ?? lr
            } else if let c = p["checkpoint"] as? [String: Any] {
                checkpoints.append(Checkpoint(
                    step: c["step"] as? Int ?? 0,
                    path: c["path"] as? String ?? "",
                    loss: (c["loss"] as? NSNumber)?.doubleValue ?? 0))
                // Its previews are written after it: read in a moment.
                Task { @MainActor [weak self] in
                    try? await Task.sleep(for: .seconds(4))
                    self?.readPreviews()
                }
            } else {
                if let d = p["done"] as? Int, phase != "train" { done = d }
                if let t = p["total"] as? Int, phase != "train" { total = t }
                if let d = p["detail"] as? String { detail = d }
            }
        case "job.finished":
            end(String(localized: "Trained: \(runName).safetensors"), false)
        case "job.failed":
            end(p["message"] as? String
                ?? String(localized: "The training failed."), true)
        case "job.cancelled":
            if endAfterStop {
                endAfterStop = false
                self.job = nil
                _ = core?.finishTrained(runName)
                end(String(localized: "Ended at step \(String(done)): its adapter is kept, and Start trains afresh."), false)
            } else {
                end(String(localized: "Paused at step \(String(done)): Resume trains on from there."), false)
            }
        default:
            break
        }
    }

    private func end(_ words: String, _ failure: Bool) {
        job = nil
        outcome = words
        failed = failure
        reload()
        readPreviews()
    }

    /// Seconds left at the last step's pace.
    var secondsLeft: Double? {
        guard phase == "train", total > done, stepSeconds > 0 else {
            return nil
        }
        return Double(total - done) * stepSeconds
    }
}
