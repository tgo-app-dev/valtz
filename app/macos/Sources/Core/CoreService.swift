import CoreGraphics
import Foundation
import ValtzBridge

/// One event from the controller: its JSON payload, plus a live preview
/// frame when the event carries one -- or, for a video, the preview CLIP:
/// every frame, played at the payload's "fps" (`image` is then its last).
struct CoreEvent: @unchecked Sendable {
    let kind: String
    let job: String?
    let payload: [String: Any]
    let image: CGImage?
    var clip: [CGImage] = []
}

/// A request's immediate answer: `{"ok": true, ...}` or an error.
struct CoreReply: @unchecked Sendable {
    let ok: Bool
    let payload: [String: Any]

    /// The reply's message in the user's language (L10n.coreMessage).
    var message: String { L10n.coreMessage(payload) }
    var job: String? { payload["job"] as? String }
    subscript(key: String) -> Any? { payload[key] }
}

/// The C++ controller as the app sees it. One per process.
///
/// Commands go in as method calls that return promptly; everything that
/// happens afterwards arrives as a `CoreEvent` on the pump started by
/// `startEvents`. The underlying object is an immortal C++ reference whose
/// methods are all thread-safe.
final class CoreService: @unchecked Sendable {
    private let core: valtz.bridge.Core
    private var pump: Thread?

    /// `configJSON` defaults to the UI language (L10n.uiLanguage), which
    /// the core passes on to the engine.
    init?(configJSON: String = CoreService.defaultConfig) {
        guard let c = valtz.bridge.Core.create(std.string(configJSON)) else {
            return nil
        }
        core = c
    }

    static var defaultConfig: String {
        let cfg = ["language": L10n.uiLanguage]
        guard let d = try? JSONSerialization.data(withJSONObject: cfg),
              let s = String(data: d, encoding: .utf8) else { return "{}" }
        return s
    }

    static var createError: String {
        String(valtz.bridge.Core.create_error())
    }

    // MARK: - Queries (JSON documents)

    var version: String { String(core.version()) }
    var engine: String { String(core.engine()) }
    var defaultProjectPath: String { String(core.default_project_path()) }

    func hardwareJSON() -> Data { Data(String(core.hardware_json()).utf8) }
    /// Settings › Capabilities: families, features, resources.
    func capabilityTreeJSON() -> Data {
        Data(String(core.capability_tree_json()).utf8)
    }
    /// Settings › Storage. Walks folders: off the main thread.
    func storageReportJSON() -> Data {
        Data(String(core.storage_report_json()).utf8)
    }
    /// A model used from a file or folder anywhere, or no longer.
    func linkModel(_ id: String, path: String) -> CoreReply {
        send(core.link_model, ["model": id, "path": path])
    }
    func unlinkModel(_ id: String) -> CoreReply {
        send(core.unlink_model, ["model": id])
    }
    /// Settings › Agentic Helper: the helpers, the one chosen and the one
    /// in use; choosing one ("" Auto) is kept for later runs.
    func assistantsJSON() -> Data {
        Data(String(core.assistants_json()).utf8)
    }
    func chooseAssistant(_ id: String) -> CoreReply {
        send(core.choose_assistant, ["model": id])
    }
    func setAssistantKeepLoaded(_ seconds: Double) -> CoreReply {
        send(core.choose_assistant, ["keep_loaded": seconds])
    }
    func setAssistantDrafter(_ kind: String, bits: Int) -> CoreReply {
        send(core.choose_assistant, ["drafter": kind, "drafter_bits": bits])
    }
    /// The Log view's rows after `seq` (any thread):
    /// {"rows", "next", "first"}.
    func logSinceJSON(_ seq: UInt64, max: Int = 4096) -> Data {
        Data(String(core.log_since(std.string(
            "{\"seq\":\(seq),\"max\":\(max)}"))).utf8)
    }
    func clearLog() { core.clear_log() }

    /// The machine's live load (the status bar's; any thread).
    func machineStatusJSON() -> Data {
        Data(String(core.machine_status_json()).utf8)
    }
    /// Whether the GPU is held back: blocks for `windowMs`, so never on
    /// the main thread.
    func gpuThermalJSON(windowMs: Int) -> Data {
        Data(String(core.gpu_thermal_json(Int32(windowMs))).utf8)
    }
    func capabilitiesJSON() -> Data {
        Data(String(core.capabilities_json()).utf8)
    }
    func catalogJSON() -> Data { Data(String(core.catalog_json()).utf8) }
    /// {"image": {"generate": AutoChoice, "edit": ...}, "video": ...}
    func autoJSON() -> Data { Data(String(core.auto_json()).utf8) }

    func pathsJSON() -> Data { Data(String(core.paths_json()).utf8) }

    func rescanModels() { core.rescan_models() }

    /// A cached JPEG thumbnail (made on first request). Blocking on a
    /// cache miss -- call off the main thread.
    /// As it looks; `plain`: its own file alone.
    func thumbnail(project: String, asset: String, maxPixels: Int,
                   plain: Bool = false) -> URL? {
        let r = reply(core.thumbnail(std.string(project), std.string(asset),
                                     Int32(maxPixels), plain))
        guard r.ok, let path = r["path"] as? String else { return nil }
        return URL(fileURLWithPath: path)
    }

    // MARK: - Commands

    func createProject(path: String, name: String) -> CoreReply {
        reply(core.create_project(std.string(path), std.string(name)))
    }

    func openProject(path: String) -> CoreReply {
        reply(core.open_project(std.string(path)))
    }

    func closeProject(_ id: String) -> CoreReply {
        reply(core.close_project(std.string(id)))
    }

    /// Closed with its unsaved changes thrown away (its working copy
    /// goes).
    func discardProject(_ id: String) -> CoreReply {
        reply(core.discard_project(std.string(id)))
    }

    /// An untitled project -- the anonymous session: a working copy at
    /// `dir`, no package until Save As (DESIGN §5b).
    func createUntitled(dir: String, name: String) -> CoreReply {
        reply(core.create_untitled(std.string(dir), std.string(name)))
    }

    func saveProject(_ id: String) -> CoreReply {
        reply(core.save_project(std.string(id)))
    }

    /// How the project was last looked at (its window's size), saved with
    /// it: {"view": {"window": {"width", "height"}}}.
    func viewState(_ id: String) -> CoreReply {
        reply(core.view_state(std.string(id)))
    }

    func setViewState(_ id: String, _ view: [String: Any]) -> CoreReply {
        send(core.set_view_state, ["project": id, "view": view])
    }

    func saveProjectAs(_ id: String, path: String) -> CoreReply {
        reply(core.save_project_as(std.string(id), std.string(path)))
    }

    func revertProject(_ id: String) -> CoreReply {
        reply(core.revert_project(std.string(id)))
    }

    func undo(project: String) -> CoreReply {
        reply(core.undo(std.string(project)))
    }

    func redo(project: String) -> CoreReply {
        reply(core.redo(std.string(project)))
    }

    /// {"state": {"dirty", "untitled", "package", "name", "busy", "undo",
    /// "redo"}}.
    func projectState(_ id: String) -> CoreReply {
        reply(core.project_state(std.string(id)))
    }

    /// Keep everything of the project inside its package (anonymous
    /// sessions).
    func setEphemeral(project: String, _ on: Bool) -> CoreReply {
        reply(core.set_project_ephemeral(std.string(project), on))
    }

    func assets(project: String) -> CoreReply {
        reply(core.assets_json(std.string(project)))
    }

    /// Canvas Size: the picture or clip on a `width` x `height` canvas,
    /// anchored (0, 0.5, 1 across and down).
    func setCanvas(project: String, asset: String, width: Int, height: Int,
                   anchorX: Double, anchorY: Double) -> CoreReply {
        send(core.set_canvas, ["project": project, "asset": asset,
                               "width": width, "height": height,
                               "anchor_x": anchorX, "anchor_y": anchorY])
    }

    /// A clip's stack, ready for the player: `live` is the layer being
    /// edited, its tracks as the panels hold them (keyframe JSON).
    func stackPlan(project: String, asset: String,
                   live: (layer: String, adjust: Any, crop: Any)?)
        -> StackPlayback? {
        var req: [String: Any] = ["project": project, "asset": asset]
        if let live {
            req["live"] = ["layer": live.layer, "adjust": live.adjust,
                           "crop": live.crop]
        }
        let r = send(core.stack_plan, req)
        let planValue = r["plan"] as? NSNumber
        guard r.ok, let plan = planValue?.uint64Value,
              let w = r["width"] as? Int, let h = r["height"] as? Int,
              let frames = r["frames"] as? Int,
              let num = r["rate_num"] as? Int,
              let den = r["rate_den"] as? Int else { return nil }
        let clips = (r["clips"] as? [String] ?? []).map {
            URL(fileURLWithPath: $0)
        }
        func numbers(_ v: Any?) -> [[Double]] {
            (v as? [[Any]] ?? []).map { row in
                row.compactMap { ($0 as? NSNumber)?.doubleValue }
            }
        }
        let segments = (r["segments"] as? [Any] ?? []).map(numbers)
        let audio: [StackAudio] = (r["audio"] as? [[String: Any]] ?? [])
            .compactMap { a in
                guard let f = a["file"] as? String else { return nil }
                return StackAudio(file: URL(fileURLWithPath: f),
                                  segments: numbers(a["segments"]),
                                  volume: numbers(a["volume"]))
            }
        let mix = (r["mix"] as? String).flatMap {
            $0.isEmpty ? nil : URL(fileURLWithPath: $0)
        }
        return StackPlayback(plan: plan, width: w, height: h, frames: frames,
                             rate: FrameRate(num: num, den: den),
                             clips: clips, segments: segments, audio: audio,
                             mix: mix,
                             seconds: (r["seconds"] as? NSNumber)?
                                 .doubleValue ?? 0)
    }

    func releaseStackPlan(_ plan: UInt64) {
        _ = send(core.release_stack_plan, ["plan": plan])
    }

    /// Frame `frame` of a stack drawn into `out` on the GPU, from the
    /// clips' frames (nil where a clip has none). Any thread.
    func renderStack(plan: UInt64, frame: Int, clips: [CVPixelBuffer?],
                     into out: CVPixelBuffer) -> Bool {
        let items: [AnyObject] = clips.map { pb -> AnyObject in
            if let pb { return pb }
            return kCFNull
        }
        return valtz.bridge.stack_render(core, plan, Int64(frame),
                                         items as CFArray, out)
    }

    /// Frame `frame` of a stack as a picture (a poster).
    func stackStill(plan: UInt64, frame: Int) -> CGImage? {
        valtz.bridge.stack_still(core, plan, Int64(frame))?
            .takeRetainedValue()
    }

    /// Back to its own frame, at its own place.
    func resetCanvas(project: String, asset: String) -> CoreReply {
        send(core.set_canvas, ["project": project, "asset": asset,
                               "reset": true])
    }

    /// A clip's timeline length, in its frames; 0 is its own.
    func setTimeline(project: String, asset: String, frames: Int)
        -> CoreReply {
        send(core.set_timeline, ["project": project, "asset": asset,
                                 "frames": frames])
    }

    /// The generation history, oldest first ("entries").
    func history(project: String) -> CoreReply {
        reply(core.history_json(std.string(project)))
    }

    /// A prompt as an asset (bridge capture_prompt): {"project",
    /// "prompt", "inline"?, "row"?, "prompt_asset"?} -> {"asset"}.
    func capturePrompt(_ req: [String: Any]) -> CoreReply {
        send(core.capture_prompt, req)
    }

    /// The task queue (generations, upscales, exports), in the order they
    /// run.
    func tasks() -> CoreReply {
        reply(core.tasks_json())
    }

    func versions(project: String, asset: String) -> CoreReply {
        reply(core.versions_json(std.string(project), std.string(asset)))
    }

    func importFiles(project: String, paths: [String]) -> CoreReply {
        send(core.import_files, ["project": project, "paths": paths])
    }

    func generateImage(_ request: [String: Any]) -> CoreReply {
        send(core.generate_image, request)
    }

    /// A clip with its soundtrack (MiniMax H3); see valtz-bridge.h.
    func generateVideo(_ request: [String: Any]) -> CoreReply {
        send(core.generate_video, request)
    }

    /// What a clip's references are called, as the model reads them --
    /// asset id to "<Picture 1>" -- for the prompt row's badges; empty
    /// when they cannot be read (too many, a kind it does not take).
    func videoReferenceTags(project: String, model: String,
                            references: [String],
                            continueFrom: String?) -> [String: String] {
        var req: [String: Any] = ["project": project, "model": model,
                                  "references": references]
        if let c = continueFrom { req["continue"] = c }
        let r = send(core.video_reference_tags, req)
        return r["tags"] as? [String: String] ?? [:]
    }

    /// `model`'s prompt outline written for a row holding `row` ("image",
    /// "video", "audio", in order); "" without one.
    func promptOutline(model: String, row: [String]) -> String {
        let r = send(core.prompt_outline, ["model": model, "row": row])
        return r["text"] as? String ?? ""
    }

    /// A song (YuE2); see valtz-bridge.h.
    func generateAudio(_ request: [String: Any]) -> CoreReply {
        send(core.generate_audio, request)
    }

    /// A layer's clip rendered at its scale by an upscaler; the layer then
    /// shows it at scale 1 (core Controller::upscale_layer).
    func upscaleLayer(_ request: [String: Any]) -> CoreReply {
        send(core.upscale_layer, request)
    }

    /// The prompt as a song's words, as generate_audio reads it: its
    /// style, and its lyrics under section headers (core
    /// assist::split_song).
    func songText(_ text: String) -> SongWords {
        let req: [String: Any] = ["text": text]
        guard let d = try? JSONSerialization.data(withJSONObject: req),
              let s = String(data: d, encoding: .utf8) else { return .none }
        let r = reply(core.song_text(std.string(s)))
        return SongWords(style: r["style"] as? String ?? "",
                         lyrics: r["lyrics"] as? String ?? "",
                         sections: r["sections"] as? Int ?? 0,
                         lines: r["lines"] as? Int ?? 0)
    }

    /// Record `a` on the asset as its adjust modifier (an identity removes
    /// it): applied when a file is made from it, not to its bytes.
    @discardableResult
    func setAdjustments(project: String, asset: String,
                        _ a: ImageAdjustments) -> CoreReply {
        send(core.set_adjustments,
             ["project": project, "asset": asset, "adjust": a.json])
    }

    /// The Crop panel's values recorded on a picture (its "crop"
    /// modifier); the identity removes it.
    func setCrop(project: String, asset: String,
                 _ c: CropSpec) -> CoreReply {
        send(core.set_crop, ["project": project, "asset": asset,
                             "crop": c.json])
    }

    /// A layer's adjustments or crop: as above, for one layer of a
    /// picture's stack ("" is the bottom one).
    func setAdjustments(project: String, asset: String, layer: String,
                        _ a: ImageAdjustments) -> CoreReply {
        send(core.set_adjustments, ["project": project, "asset": asset,
                                    "layer": layer, "adjust": a.json])
    }

    func setCrop(project: String, asset: String, layer: String,
                 _ c: CropSpec) -> CoreReply {
        send(core.set_crop, ["project": project, "asset": asset,
                             "layer": layer, "crop": c.json])
    }

    /// A picture's layer stack (core project::Layer): "add" (`above`),
    /// The asset list (valtz-bridge.h asset_op): "create-folder",
    /// "rename-folder", "delete-folder", "move", "remove", "modify",
    /// "capture", "unfreeze".
    func assetOp(project: String, _ op: String,
                 _ extra: [String: Any] = [:]) -> CoreReply {
        var req: [String: Any] = ["project": project, "op": op]
        req.merge(extra) { $1 }
        return send(core.asset_op, req)
    }

    /// "move" (`by`), "show", "hide", "rename" (`name`), "source"
    /// (`source`), "remove". An added layer's id comes back as "layer".
    func layerOp(project: String, asset: String, _ op: String,
                 layer: String = "", extra: [String: Any] = [:]) -> CoreReply {
        var req: [String: Any] = ["project": project, "asset": asset,
                                  "op": op, "layer": layer]
        req.merge(extra) { $1 }
        return send(core.layer_op, req)
    }

    /// The picture's stack flattened into a 16-bit PNG at `path` -- with
    /// one layer's values as the panels hold them (`look`), or that layer
    /// alone (`only`).
    /// `hidden`: markup objects the app is drawing itself, left out.
    func flatten(project: String, asset: String, to path: String,
                 look: (layer: String, adjust: ImageAdjustments,
                        crop: CropSpec)? = nil,
                 only: String? = nil,
                 hidden: Set<String> = [], page: Int = 0) -> CoreReply {
        var req: [String: Any] = ["project": project, "asset": asset,
                                  "path": path, "page": page]
        if let look {
            req["look"] = ["layer": look.layer, "adjust": look.adjust.json,
                           "crop": look.crop.json]
        }
        if let only { req["only"] = only }
        if !hidden.isEmpty { req["hidden"] = Array(hidden) }
        return send(core.flatten, req)
    }

    /// The stack drawn for the screen, by the GPU into a surface a layer
    /// shows as it is (bridge flatten_surface): no PNG made and read back.
    /// nil on a failure (the log says why).
    func flattenSurface(project: String, asset: String,
                        look: (layer: String, adjust: ImageAdjustments,
                               crop: CropSpec)? = nil,
                        hidden: Set<String> = [],
                        page: Int = 0) -> IOSurface? {
        // `page`: a still's page (DESIGN §6a).
        var req: [String: Any] = ["project": project, "asset": asset,
                                  "page": page]
        if let look {
            req["look"] = ["layer": look.layer, "adjust": look.adjust.json,
                           "crop": look.crop.json]
        }
        if !hidden.isEmpty { req["hidden"] = Array(hidden) }
        guard let data = try? JSONSerialization.data(withJSONObject: req),
              let json = String(data: data, encoding: .utf8) else {
            return nil
        }
        return valtz.bridge.flatten_surface(core, std.string(json))?
            .takeRetainedValue()
    }

    /// A clip's adjustment or crop track (its "adjust" / "crop"
    /// modifier; core media/keyframes.h); none removes it.
    func setKeys(project: String, asset: String, kind: String,
                 layer: String = "", _ track: [String: Any]) -> CoreReply {
        send(core.set_keys, ["project": project, "asset": asset,
                             "kind": kind, "layer": layer, "keys": track])
    }

    /// A composition layer's place in time: its marks in its source,
    /// where it starts, how long it runs (LayerTimeDTO.json).
    func setLayerTime(project: String, asset: String, layer: String,
                      _ t: [String: Int]) -> CoreReply {
        send(core.set_layer_time, ["project": project, "asset": asset,
                                   "layer": layer, "time": t])
    }

    /// The file an asset is drawn from: a composition's or a markup's
    /// rendering, made and kept in the cache as needed (a sound
    /// composition's mix). Blocking; off the main thread.
    func renderedPath(project: String, asset: String) -> URL? {
        let r = send(core.rendered_path, ["project": project, "asset": asset])
        return (r["path"] as? String).map { URL(fileURLWithPath: $0) }
    }

    /// Where `c` puts a picture of `width` x `height` on its canvas, as
    /// the core says (media/crop.h); nil if it cannot.
    func cropPlacement(_ c: CropSpec, width: Int,
                       height: Int) -> CropPlacement? {
        var spec = c
        if spec.contentWidth == 0 {
            spec.contentWidth = width
            spec.contentHeight = height
        }
        let req: [String: Any] = [
            "crop": spec.json.isEmpty ? Self.identity(spec) : spec.json,
            "width": width, "height": height,
        ]
        guard let d = try? JSONSerialization.data(withJSONObject: req),
              let s = String(data: d, encoding: .utf8) else { return nil }
        let r = reply(core.crop_placement(std.string(s)))
        guard r.ok, let canvas = r["canvas"] as? [Double], canvas.count == 2,
              let t = r["transform"] as? [Double], t.count == 6 else {
            return nil
        }
        return CropPlacement(
            canvas: CGSize(width: canvas[0], height: canvas[1]),
            transform: CGAffineTransform(a: t[0], b: t[1], c: t[2], d: t[3],
                                         tx: t[4], ty: t[5]),
            pad: spec.padColor)
    }

    /// An identity crop spelled out, so the core places the content on
    /// its own canvas (its JSON form is empty).
    private static func identity(_ c: CropSpec) -> [String: Double] {
        ["content_w": Double(c.contentWidth),
         "content_h": Double(c.contentHeight)]
    }

    /// The Core Image filters `a` is (core media/adjust.h): the preview is
    /// built from the same chain the engine lays on a base before the
    /// model sees it.
    /// Favor's options for `model` at `preference` with Custom's values
    /// laid on, settled by the core (models/tuning.h).
    func tuning(model: String, preference: String, edit: Bool,
                overrides: [String: Any]) -> TuningInfo? {
        let req: [String: Any] = ["model": model, "preference": preference,
                                  "edit": edit, "tuning": overrides]
        guard let d = try? JSONSerialization.data(withJSONObject: req),
              let s = String(data: d, encoding: .utf8) else { return nil }
        let r = reply(core.tuning(std.string(s)))
        guard r.ok, let values = r["values"] as? [String: Any],
              let opts = r["options"] as? [[String: Any]] else { return nil }
        let options = opts.map(TuningOption.init)
        return TuningInfo(family: r["family"] as? String ?? "",
                          tuning: Tuning(values, options), options: options)
    }

    /// What a dropped weight file or folder is: "lora", "dit", "vae", or
    /// "" (core models::checkpoint_kind).
    func checkpointKind(_ url: URL) -> String {
        let req: [String: Any] = ["path": url.path]
        guard let d = try? JSONSerialization.data(withJSONObject: req),
              let s = String(data: d, encoding: .utf8) else { return "" }
        let r = reply(core.checkpoint_kind(std.string(s)))
        return r["kind"] as? String ?? ""
    }

    func adjustmentChain(_ a: ImageAdjustments) -> [ImageAdjustments.Step] {
        guard let d = try? JSONSerialization.data(withJSONObject: a.json),
              let s = String(data: d, encoding: .utf8) else { return [] }
        let r = reply(core.adjustment_chain(std.string(s)))
        guard r.ok, let chain = r["chain"] as? [[String: Any]] else {
            return []
        }
        return chain.compactMap { step in
            guard let name = step["filter"] as? String,
                  let params = step["params"] as? [String: Any] else {
                return nil
            }
            var p: [String: [Double]] = [:]
            for (k, v) in params {
                if let n = v as? Double {
                    p[k] = [n]
                } else if let a = v as? [Double] {
                    p[k] = a
                }
            }
            return ImageAdjustments.Step(filter: name, params: p)
        }
    }

    func enhancePrompt(_ request: [String: Any]) -> CoreReply {
        send(core.enhance_prompt, request)
    }

    func exportAsset(_ request: [String: Any]) -> CoreReply {
        send(core.export_asset, request)
    }

    func detectIntent(_ request: [String: Any]) -> CoreReply {
        send(core.detect_intent, request)
    }

    /// `token`: a gated model's Hugging Face access token, "" none --
    /// handed through for this download, kept nowhere.
    func downloadModel(_ id: String, token: String = "") -> CoreReply {
        reply(core.download_model(std.string(id), std.string(token)))
    }

    /// A quantized variant made from its source (bridge quantize_model).
    func quantizeModel(_ id: String) -> CoreReply {
        reply(core.quantize_model(std.string(id)))
    }

    /// The capture sources there now, and the microphone's permission.
    func captureSources() -> CoreReply {
        reply(core.capture_sources_json())
    }

    /// A recording started, polled, stopped or cancelled (bridge
    /// capture_op).
    func captureOp(_ req: [String: Any]) -> CoreReply {
        send(core.capture_op, req)
    }

    /// The camera (bridge camera_op): sources, start, state, snap, record,
    /// stop-recording, stop.
    func cameraOp(_ req: [String: Any]) -> CoreReply {
        send(core.camera_op, req)
    }

    /// The camera's newest frame, the surface it came into (bridge
    /// camera_frame); nil with the camera off.
    func cameraFrame() -> IOSurface? {
        valtz.bridge.camera_frame(core)?.takeRetainedValue()
    }

    func cancel(job: String) -> CoreReply {
        reply(core.cancel(std.string(job)))
    }

    func shutdown() {
        pump?.cancel()
        core.shutdown()
    }

    // MARK: - Events

    /// Start the event pump: a dedicated thread blocks on the controller's
    /// event stream and hands each event to `handler` (on that thread).
    func startEvents(_ handler: @escaping @Sendable (CoreEvent) -> Void) {
        let thread = Thread { [self] in
            while !Thread.current.isCancelled {
                let ev = core.next_event(250)
                if !ev.valid { continue }
                let json = String(ev.json)
                guard
                    let obj = try? JSONSerialization.jsonObject(
                        with: Data(json.utf8)) as? [String: Any]
                else { continue }
                var image: CGImage?
                var clip: [CGImage] = []
                if ev.image_token != 0 {
                    // CF_RETURNS_RETAINED on a C++ function still comes
                    // through as Unmanaged; take the +1 we were given.
                    if (obj["frames"] as? Int ?? 0) > 0 {
                        // A video's preview: the whole clip, converted
                        // here, off the main thread.
                        let arr = valtz.bridge.take_preview_clip(
                            core, ev.image_token)?.takeRetainedValue()
                        clip = (arr as? [CGImage]) ?? []
                        image = clip.last
                    } else {
                        image = valtz.bridge.take_preview(
                            core, ev.image_token)?.takeRetainedValue()
                    }
                }
                handler(CoreEvent(
                    kind: obj["kind"] as? String ?? "",
                    job: obj["job"] as? String,
                    payload: obj,
                    image: image,
                    clip: clip))
            }
        }
        thread.name = "valtz.events"
        thread.qualityOfService = QualityOfService.userInitiated
        pump = thread
        thread.start()
    }

    // MARK: - Plumbing

    private func send(
        _ fn: (std.string) -> std.string, _ request: [String: Any]
    ) -> CoreReply {
        guard
            let data = try? JSONSerialization.data(withJSONObject: request),
            let text = String(data: data, encoding: .utf8)
        else {
            return CoreReply(ok: false, payload: ["message": "bad request"])
        }
        return reply(fn(std.string(text)))
    }

    private func reply(_ s: std.string) -> CoreReply {
        let text = String(s)
        let obj = (try? JSONSerialization.jsonObject(
            with: Data(text.utf8)) as? [String: Any]) ?? [:]
        return CoreReply(ok: obj["ok"] as? Bool ?? false, payload: obj)
    }
}
