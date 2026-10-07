// ValtzBridge -- the only C++ the SwiftUI app sees.
//
// Swift imports this header directly (Swift/C++ interop, module
// "ValtzBridge"). It is deliberately NARROW and FLAT:
//
//   * no core headers: Swift's importer never parses nlohmann/json,
//     LMDB or vpipe, so interop stays fast and a core refactor cannot
//     break the Swift build;
//   * requests and results are JSON strings, decoded with Codable on the
//     Swift side -- the same shapes the event stream uses;
//   * nothing throws (a C++ exception unwinding into Swift is fatal);
//     failures come back as {"ok": false, "code": ..., "message": ...};
//   * nothing calls INTO Swift. The app pulls events with next_event()
//     on its own thread; live preview pixels are fetched by token as a
//     retained CGImage (a video's, as an array of them).

#ifndef VALTZ_BRIDGE_H
#define VALTZ_BRIDGE_H

#include <CoreGraphics/CoreGraphics.h>
#include <CoreVideo/CVPixelBuffer.h>
#include <IOSurface/IOSurfaceRef.h>

#include <cstdint>
#include <string>

#if __has_include(<swift/bridging>)
#include <swift/bridging>
#else
#define SWIFT_IMMORTAL_REFERENCE
#endif

namespace valtz::bridge {

struct Event {
  bool          valid = false;
  std::string   json;          // {"seq", "kind", "job", ...}
  std::uint64_t image_token = 0;  // non-zero: a preview frame is waiting
};

class Core {
public:
  // `config_json`: {"with_engine": bool, "model_roots": [...]} (all
  // optional). Returns nullptr on failure; see create_error().
  static Core* create(const std::string& config_json);
  static std::string create_error();

  std::string version() const;
  std::string engine() const;
  std::string hardware_json() const;
  // The machine's live load, as vpipe's status bar reads it: {"gpu_util_pct",
  // "ane_util_pct", "phys_footprint_bytes", "phys_total_bytes",
  // "sys_used_bytes" (the whole Mac's, as Activity Monitor counts it), ...}
  // (Controller::machine_status; each key absent when unavailable). Poll
  // at a steady pace: the ANE's is a delta since the last call.
  std::string machine_status_json() const;
  // Settings > Capabilities (Controller::capability_tree): the model
  // families, their features and members, where each member is.
  std::string capability_tree_json() const;
  // The extension packages found at launch, as this run took them
  // (Controller::extensions): each one's state, what it brings and what
  // was withheld, and the interface this build offers.
  std::string extensions_json() const;
  // Settings > Storage (Controller::storage_report): the volume, the
  // managed models, the projects, the cache. Walks folders: off the main
  // thread.
  std::string storage_report_json() const;
  // {"model", "path"} -> {"ok"}: a model linked to a file or folder
  // anywhere; {"model"} -> {"ok"}: the link undone.
  std::string link_model(const std::string& request_json);
  std::string unlink_model(const std::string& request_json);
  // Settings > Agentic Helper (Controller::assistants): the helpers, the
  // one chosen ("" Auto), Auto's pick and the one in use; {"model"} ->
  // {"ok"}: that one chosen ("" Auto), kept for later runs;
  // {"keep_loaded": seconds}: how long it stays loaded after a request;
  // {"drafter": "mtp" | "dflash", "drafter_bits": 8 | 4}: what it
  // drafts with.
  std::string assistants_json() const;
  std::string choose_assistant(const std::string& request_json);
  // {"seq", "max"?} -> {"ok", "rows", "next", "first"}: the Log view's
  // rows after `seq` (Controller::log_since); clear_log empties it.
  std::string log_since(const std::string& request_json) const;
  void clear_log();
  // Whether the GPU is held back now: {"verdict", "clock_mhz",
  // "ceiling_mhz", "gpu_active_pct", "temp_c", "os_thermal_state"}.
  // Blocks for `window_ms` -- call it off the main thread.
  std::string gpu_thermal_json(int window_ms) const;
  std::string capabilities_json() const;
  // What the model field's Auto picks (Controller::auto_models):
  // {"image": {"generate": {"chosen": id or "", "models": [{"model",
  //  "name", "runs"}]}, "edit": {...}}, "video": {...}}, each list in
  // priority order.
  std::string auto_json() const;
  // The catalog, each model with what runs here: "steps" per preference
  // ({"speed", "balanced", "quality"}; a few-step adapter's when it is
  // installed) and "turbo", that adapter's name ("" without one).
  std::string catalog_json() const;
  std::string jobs_json() const;
  // The TASK QUEUE (Controller::tasks): {"ok", "tasks": [...]}, in the
  // order they run.
  std::string tasks_json() const;
  void rescan_models();

  // {"projects", "default_project", "models", "model_roots", "cache",
  //  "cache_bytes", "cache_budget_bytes", "cache_internal", "engine"}
  std::string paths_json() const;

  // Results: {"ok": true, ...} or {"ok": false, "code", "message"}.
  std::string default_project_path() const;
  std::string create_project(const std::string& path,
                             const std::string& name);
  // {"project", "name", "untitled", "recovered", "legacy",
  //  "missing_extensions"}: a project opened in its working copy
  //  (DESIGN §5b) -- resumed when it was left with unsaved changes.
  std::string open_project(const std::string& path);
  // An untitled project (the anonymous session): a working copy at
  // `dir`, no package.
  std::string create_untitled(const std::string& dir,
                              const std::string& name);
  // Closed: its working copy goes when it is clean (kept dirty, to be
  // resumed); discarded, its unsaved changes go too.
  std::string close_project(const std::string& project_id);
  std::string discard_project(const std::string& project_id);
  // Saving, reverting, undoing: {"project": id[, "path"]} (save_as).
  std::string save_project(const std::string& project_id);
  std::string save_project_as(const std::string& project_id,
                              const std::string& path);
  std::string revert_project(const std::string& project_id);
  std::string undo(const std::string& project_id);
  std::string redo(const std::string& project_id);
  // {"dirty", "untitled", "package", "name", "busy", "undo": {"kind",
  //  "args"} | null, "redo": ...}: Controller::project_state.
  std::string project_state(const std::string& project_id) const;
  // Keep everything of the project inside its package (the anonymous
  // session): Controller::set_ephemeral.
  std::string set_project_ephemeral(const std::string& project_id,
                                    bool ephemeral);
  std::string assets_json(const std::string& project_id) const;
  // {"entries": [{"id", "created", "role" (base|result), "path", "width",
  //  "height", "asset", "version", "generation", "generation_version",
  //  "label", "rendered"}]}: the generation history, oldest first
  //  (Controller::history).
  std::string history_json(const std::string& project_id) const;
  // How the project was last looked at (Controller::view_state): {"ok",
  // "view": {"window": {"width", "height"}}}; set with {"project",
  // "view"}, saved with the project.
  std::string view_state(const std::string& project_id) const;
  std::string set_view_state(const std::string& request_json);
  std::string versions_json(const std::string& project_id,
                            const std::string& asset_id) const;
  // {"ok", "path"} -- where an asset version's media is on disk.
  std::string media_path(const std::string& project_id,
                         const std::string& asset_id,
                         std::uint32_t version) const;

  // {"project": id, "paths": [...], "placement"?: "auto"|"copy"|"link"}
  // auto = link video in place, copy everything else.
  std::string import_files(const std::string& request_json);
  // {"ok", "path"} -- a cached JPEG thumbnail of the asset as it looks;
  // `plain`: its own file alone. Blocking on a cache miss: call off the
  // main thread.
  std::string thumbnail(const std::string& project_id,
                        const std::string& asset_id, std::int32_t max_px,
                        bool plain = false);
  // {"project", "prompt", "negative"?, "model"?, "width"?, "height"?,
  //  "steps"?, "preference"?: "speed"|"balanced"|"quality", "seed"?,
  //  "base"?: asset id (the picture to edit), "base_adjust"?:
  //  {"exposure": 0.4, ...} (laid on the base before the model sees it),
  //  "base_crop"?: {...} (then its crop), "references"?: [asset ids]}
  std::string generate_image(const std::string& request_json);
  // {"project", "prompt", "model"?, "width"?, "height"?, "frames"?,
  //  "fps"?, "steps"?, "preference"?, "seed"?, "turbo"?: bool,
  //  "first"?: asset id (a picture the clip opens on), "first_adjust"?,
  //  "first_crop"?, "references"?: [asset ids: pictures, clips, sounds,
  //  in order], "continue"?: a clip to carry on from (its tail),
  //  "tail_seconds"?, "reference_sound"?: bool (the first sound's own as
  //  the soundtrack), "inline"?: [asset ids mentioned in the prompt]}:
  // a clip with its soundtrack (Controller::generate_video). Its
  // job.preview events are clips too (take_preview_clip).
  std::string generate_video(const std::string& request_json);
  // {"project", "prompt" (its style, then its lyrics under section
  //  headers), "lyrics"?, "model"?, "plan"?: "full"|"melody"|"off",
  //  "score"?: ABC to follow, "max_seconds"?, "steps"?, "preference"?,
  //  "tuning"?, "seed"?}: a song (Controller::generate_audio). Its
  //  result lists the "score" it followed (assets_json). With a model
  //  that speaks (MOSS-TTS) -- or Auto and a sound in "row" -- SPEECH:
  //  the prompt its fields then its words, "voice"?: a sound to clone
  //  (none: the row's first), "seconds"?: about how long.
  std::string generate_audio(const std::string& request_json);
  // {"project", "asset" (the composition), "layer"?, "model"?, "seed"?}:
  // the layer's clip rendered at its scale by an upscaler, which the
  // layer then shows at scale 1 (Controller::upscale_layer) -> {"job"}.
  std::string upscale_layer(const std::string& request_json);
  // A PROMPT as an asset (Controller::capture_prompt; DESIGN §10c):
  // {"project", "prompt", "inline"?: [ids], "row"?: [ids],
  //  "prompt_asset"?} -> {"ok", "asset"}; and {"project", "asset",
  //  "text"} -> {"ok"}: its words changed in place, refused once
  //  something has been made from it. Every generate_* request takes
  //  "row" (the reference row, in order) and "prompt_asset" too;
  //  assets_json lists a prompt's "text" and "uses".
  std::string capture_prompt(const std::string& request_json);
  std::string set_prompt_text(const std::string& request_json);
  // {"project", "model"?, "references": [ids], "continue"?} -> {"ok",
  //  "tags": {asset: "<Picture 1>"}}: what a clip's references are
  //  called (Controller::video_reference_tags).
  std::string video_reference_tags(const std::string& request_json) const;
  // {"model", "row": ["image" | "video" | "audio", ...]} -> {"ok",
  //  "text"}: the model's prompt outline for a row holding those kinds,
  //  "" without one (Controller::prompt_outline).
  std::string prompt_outline(const std::string& request_json) const;
  // {"text"} -> {"style", "lyrics", "sections", "lines"}: a prompt as a
  // song's words, as generate_audio reads it (assist::split_song).
  std::string song_text(const std::string& request_json) const;
  // {"project", "asset", "adjust": {"exposure": 0.4, ...}, "layer"?}:
  // the Adjust panel's values recorded on the image as its modifier
  // (applied when a file is made from it, not to its bytes); an empty
  // "adjust" removes it. assets_json lists an asset's "modifiers".
  std::string set_adjustments(const std::string& request_json);
  // {"project", "asset", "crop": {"scale", "offset_x", ...}, "layer"?}:
  // the Crop panel's crop and rotation (media/crop.h) recorded on the
  // picture as its "crop" modifier; an empty "crop" removes it.
  std::string set_crop(const std::string& request_json);
  // {"project", "asset", "layer", "time": {"in", "out", "rate_num",
  // "rate_den", "offset", "duration"}}: a composition layer's place in
  // time -- its marks in its source, where it starts, how long it runs.
  std::string set_layer_time(const std::string& request_json);
  // {"project", "asset"} -> {"path"}: the file an asset is drawn from --
  // a composition's or a markup's rendering, made and cached as needed
  // (an audio timeline's mix, a still's picture). Blocking.
  std::string rendered_path(const std::string& request_json);
  // {"project", "asset", "live"?: {"layer", "adjust", "crop"}}: a clip's
  // stack, ready to draw for the player (media::StackRenderer) -- the
  // layer being edited with its tracks as the panels hold them.
  // {"plan": token, "width", "height", "frames", "rate_num", "rate_den",
  //  "seconds", "clips": [the video layers' files, in order],
  //  "segments": [per clip: [[at, length, source, source_length]...]]
  //  -- where its spans play on the timeline, seconds --, "audio":
  //  [{"file", "segments", "volume": [[second, gain]...]}] -- what the
  //  player sounds, ramped -- or "mix": the mixer's file, when a pitch
  //  is shifted (media/sound.h)}. stack_render draws with it;
  //  release_stack_plan ({"plan"}) lets it go.
  std::string stack_plan(const std::string& request_json);
  std::string release_stack_plan(const std::string& request_json);
  // {"project", "asset", "width", "height", "anchor_x", "anchor_y" (0,
  //  0.5, 1)}: Canvas Size (media::StackCanvas), anchored; {"project",
  //  "asset", "reset": true}: back to its own frame.
  std::string set_canvas(const std::string& request_json);
  // {"project", "asset", "frames"}: a clip's timeline length; 0 is its
  //  own.
  std::string set_timeline(const std::string& request_json);
  // {"project", "asset", "kind": "adjust" | "crop", "keys": {"keys":
  // [{"frame", ...}], "rate_num", "rate_den"}, "layer"?}: a clip's
  // adjustment or crop track (media/keyframes.h) -- a layer's, on a clip
  // with a stack; all identity removes it.
  std::string set_keys(const std::string& request_json);
  // {"crop": {...}, "width", "height"} -> {"ok", "canvas": [w, h],
  // "transform": [a, b, c, d, tx, ty]}: where the crop puts the content,
  // decoded at width x height, on its canvas -- Core Image's frame (y
  // up). The app renders a crop from this, as the engine does.
  std::string crop_placement(const std::string& request_json) const;
  // {"exposure": 0.4, ...} -> {"ok", "chain": [{"filter", "params"}]}:
  // the Core Image filters the adjustments ARE (media/adjust.h). The app
  // renders its preview from this, so the stage shows what the engine
  // will hand the model.
  std::string adjustment_chain(const std::string& adjustments_json) const;
  // {"model", "preference", "edit"?, "tuning"?: {Custom's values}} ->
  // {"ok", "family", "preference", "values", "options": [...]}: Favor's
  // options for the model (models/tuning.h) -- what the preference
  // settles, with Custom's laid on; each option's range and whether this
  // Mac can have it. generate_image / generate_video take "tuning".
  std::string tuning(const std::string& request_json) const;
  // {"path"} -> {"ok", "kind": "lora" | "dit" | "vae" | "", "family",
  // "origin"}: what a dropped weight file or folder is -- an extension's
  // recognition rules first (Controller::classify_weights) -- so the
  // Custom panel files it in the right list.
  std::string checkpoint_kind(const std::string& request_json) const;
  // A composition's layers (project::Layer): {"project", "asset", "op":
  // "add" ("above"), "move" ("layer", "by": +1 up / -1 down), "show" /
  // "hide" ("layer"), "rename" ("layer", "name"), "source" ("layer",
  // "source": an asset), "remove" ("layer"), "decompose" ("layer": one
  // showing a composition), "speed" / "sound" ("layer", "keys": a track
  // of {"frame", "rate"} / {"frame", "volume", "pitch"}), "transition"
  // ("layer": from, "to", "kind": "cut" | "dissolve" | "" none)} ->
  // {"ok", "layer" (an added one's id)}. Assets list their "layers".
  // A still's PAGES (DESIGN §6a; from 0): "page-add" ("after"?: none,
  // after the last) -> {"page"}; "page-remove" ("page"); "pages"
  // ("layer", "first", "count": 0 to the last page); "add" and
  // "markup-target" take the "page" shown (a new layer on it alone).
  // Assets list a still's "pages" (absent: one).
  // Markup (media/markup.h): "markup-target" ("selected": [layer ids])
  // -> {"layer"}: the layer to draw on, made when there is none;
  // "canvas" -> {"width", "height"}; "stroke" ("layer", "stroke");
  // "objects" ("layer", "objects": the whole list); "materialize"
  // ("layer", "objects": ids); "merge" ("layer", "with"); "mask" /
  // "unmask" ("layer").
  std::string layer_op(const std::string& request_json);
  // The asset list: {"project", "op", ...} -> {"ok", ...}.
  // "create-folder" ("name") -> {"folder"}; "rename-folder" ("folder",
  // "name"); "delete-folder" ("folder"); "move" ("asset", "folder": ""
  // the top); "output" -> {"output": {"color", "fps": [n, d],
  // "channels", "sample_rate"}}: the project's output; "set-output"
  // ("output"); "set-up-project" ("still", "width", "height", "output")
  // -> {"asset"}; "rename" ("asset", "name"); "remove" ("asset"); "modify"
  // ("asset", "name"?) -> {"asset"}: an edited copy to change instead (a
  // composition around it); "capture" ("asset", "layers"?: [ids], "name"?) -> {"asset"};
  // "instantiate" ("asset", "onto"?, "at"?: a layer, "" layer 0,
  // "offset"?: a timeline's frame) -> {"asset", "layer"}: a new layer
  // showing it; "place-result" ("asset", "onto"?, "at"?, "offset"?,
  // "join"?) -> {"asset", "layer"}: a generation's result landing -- a
  // new layer of `onto`, else a composition of its own; "place" ("asset",
  // "layer"?) -> {"placed", "asset"?}: the
  // project's composition with the asset as its new take (or on
  // `layer`) -- not placed when it is another kind; "flatten" ("asset",
  // "name"?) -> {"asset"}: a flat asset of it as drawn; "project"
  // ("asset"): that composition the project's; "new-composition"
  // ("still", "width", "height" -- 0 x 0 sound alone --, "rate_num"?,
  // "rate_den"?, "as_project"?, "name"?) -> {"asset"}; "guide" ("asset":
  // a clip, "seconds", "model"?, "name"?) -> {"asset", "frames",
  // "seconds", "fps"}: its last seconds as a continuation reads them;
  // "grab" ("asset", "frame", "name"?) -> {"asset"}: a still of its frame.
  // assets_json lists
  // the "folders" too, and marks the project's composition "project".
  std::string asset_op(const std::string& request_json);
  // {"project", "asset", "path", "look"?: {"layer", "adjust", "crop"},
  // "only"?: layer, "hidden"?: [markup object ids], "page"?: a still's,
  // from 0} -> {"ok"}: the
  // picture's stack (or the picture with its look) flattened into a
  // 16-bit PNG at path -- with one layer's values as the panel holds
  // them, or that layer alone; without the markup objects the app is
  // drawing itself.
  std::string flatten(const std::string& request_json);
  // {"prompt", "target"?: "image"|"video", "style"?}
  std::string enhance_prompt(const std::string& request_json);
  // {"project", "asset", "format" (png16|tiff16|exr|png|tiff|jpeg|
  //  prores4444|prores422hq|hevc10|h264), "path", "quality"? (a JPEG's,
  //  1..100)}: the asset written out; job.finished carries the "path".
  //  A still with pages: a file a page, numbered beside "path"
  //  ("Doc-1.png"), job.finished listing them all ("paths").
  std::string export_asset(const std::string& request_json);
  // {"text", "attachments"?: [{"kind", "name"}], "use_model"?: false}
  std::string detect_intent(const std::string& request_json);
  // A catalog model downloaded -> {"ok", "job"}. `hf_token`: the
  // person's Hugging Face access token for a GATED model (the capability
  // tree's member "gated"), "" none -- passed to the job in memory, never
  // kept (Controller::download_model).
  std::string download_model(const std::string& model_id,
                             const std::string& hf_token);
  // A quantized variant made from its source (Controller::quantize_model:
  // capability_tree_json lists a member's "quantize" -- "from",
  // "from_name", "bits", "group_size", "ready") -> {"ok", "job"}; its
  // events are a job's, purpose "quantize".
  std::string quantize_model(const std::string& model_id);
  // Sound CAPTURED (Controller::capture_*; DESIGN §7b): {"sources":
  // [{"id", "name", "kind": "microphone" | "system", "preferred"}],
  // "microphone": "granted" | "denied" | "undetermined"}; and
  // {"op": "start", "project", "asset" (a composition of sound alone; ""
  // none: the asset list alone), "source"} | {"op": "state"} ->
  // {"recording", "seconds", "level", "source", "name", ...} | {"op":
  // "stop"} -> {"asset": the recording, on the composition if any} |
  // {"op": "cancel"}.
  std::string capture_sources_json() const;
  std::string capture_op(const std::string& request_json);
  // The CAMERA (Controller::*camera*; DESIGN §7b): {"op": "sources"} ->
  // {"cameras", "camera", "microphone"} | {"op": "start", "project",
  // "camera"?, "sound"?} | {"op": "state"} -> {"on", "recording",
  // "seconds", "width", "height", "frames", ...} | {"op": "snap"} ->
  // {"asset": a still} | {"op": "record"} | {"op": "stop-recording"} ->
  // {"asset": the clip} | {"op": "stop"}: off. Its frames: camera_frame.
  std::string camera_op(const std::string& request_json);
  std::string cancel(const std::string& job_id);

  // Blocks up to `timeout_ms`. Call from ONE background thread.
  Event next_event(std::int32_t timeout_ms);

  void shutdown();

private:
  Core() = default;
  struct Impl;
  Impl* _impl = nullptr;

  friend CGImageRef take_preview(Core*, std::uint64_t);
  friend CFArrayRef take_preview_clip(Core*, std::uint64_t);
  friend IOSurfaceRef flatten_surface(Core*, const std::string&);
  friend IOSurfaceRef camera_frame(Core*);
  friend bool stack_render(Core*, std::uint64_t, std::int64_t, CFArrayRef,
                           CVPixelBufferRef);
  friend CGImageRef stack_still(Core*, std::uint64_t, std::int64_t);
} SWIFT_IMMORTAL_REFERENCE;

// The preview frame behind an Event's image_token, as an sRGB CGImage
// (a clip's last frame); null if it was superseded or already taken.
// Caller owns the result.
CGImageRef take_preview(Core* core, std::uint64_t token)
    CF_RETURNS_RETAINED;

// A VIDEO's live preview -- the job.preview event says "frames" -- as
// every frame of the clip, in order: an array of sRGB CGImages, played
// at the event's "fps". Null if superseded or already taken. Caller
// owns the result.
CFArrayRef take_preview_clip(Core* core, std::uint64_t token)
    CF_RETURNS_RETAINED;

// A picture's stack, as Core::flatten takes it ({"project", "asset",
// "look"?, "only"?, "hidden"?, "page"?}; no "path"), drawn by the GPU into an
// 8-bit sRGB surface the stage's layer shows as it is: no file made and
// read back each time the stage composes it. Null on a failure (the log
// says why). Caller owns the result.
IOSurfaceRef flatten_surface(Core* core, const std::string& request_json)
    CF_RETURNS_RETAINED;

// The camera's newest frame (Core::camera_op "start"): the IOSurface it
// came into, BGRA, which a layer shows as it is. Null with the camera off
// or before its first frame. Caller owns the result.
IOSurfaceRef camera_frame(Core* core) CF_RETURNS_RETAINED;

// Frame `frame` of a clip's stack (a stack_plan token) drawn into `out`
// on the GPU: `clips` holds the video layers' frames as the player
// decoded them, in order (kCFNull where a clip has none there). Safe on
// any thread. False if the plan is gone or the draw failed.
bool stack_render(Core* core, std::uint64_t plan, std::int64_t frame,
                  CFArrayRef clips, CVPixelBufferRef out);

// Frame `frame` of the stack as a picture -- a poster, for the stage's
// layout. Caller owns the result.
CGImageRef stack_still(Core* core, std::uint64_t plan, std::int64_t frame)
    CF_RETURNS_RETAINED;

}

#endif
