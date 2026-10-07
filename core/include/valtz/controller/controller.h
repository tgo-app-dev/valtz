// valtz::Controller -- the model-controller layer's single entry point.
//
// One per process. It owns:
//   * the open projects (each its own LMDB file; several may be open,
//     one per window),
//   * the model catalog, the on-disk model store and the hardware probe,
//   * the execution engine (libvpipe in-process today; see engine.h),
//   * the job table and the EventBus the UI drains.
//
// Every method is safe to call from any thread and returns promptly:
// work that touches media, models or the GPU is started as a JOB and
// reported through events(). Methods return Result<> for requests that
// are invalid on their face (unknown project, model not installed);
// failures during the work arrive as "job.failed" events.
//
// Event kinds (Event::kind) and their data fields:
//   log              level, category, message
//   project.opened   project, name, path
//   project.closed   project
//   assets.changed   project, asset?, reason
//   models.changed   -
//   job.queued       title, op, purpose
//   job.started      engine
//   job.progress     step, steps, progress
//   job.preview      step, steps, progress, frames?, fps? (+ Event::tensor;
//                    a video's is a clip, [F, 3, H, W])
//   job.text         text                    (streamed LLM output)
//   job.finished     project?, asset?, version?, result?
//   job.failed       code, message
//   job.cancelled    -
//   assist.partial   prompt                  (the suggestion so far, as
//                    it is written: assist::partial_enhance_reply)
//   assist.enhanced  prompt, negative, title
//   assist.intent    intent, confidence, subject, params, from_model

#ifndef VALTZ_CONTROLLER_CONTROLLER_H
#define VALTZ_CONTROLLER_CONTROLLER_H

#include "valtz/assist/assistant.h"
#include "valtz/base/paths.h"
#include "valtz/base/result.h"
#include "valtz/cache/cache-store.h"
#include "valtz/controller/event-bus.h"
#include "valtz/controller/job-timing.h"
#include "valtz/controller/log-book.h"
#include "valtz/controller/job-queue.h"
#include "valtz/engine/engine.h"
#include "valtz/ext/extension.h"
#include "valtz/media/adjust.h"
#include "valtz/media/camera.h"
#include "valtz/media/capture.h"
#include "valtz/media/crop.h"
#include "valtz/media/keyframes.h"
#include "valtz/media/markup.h"
#include "valtz/media/model-input.h"
#include "valtz/media/sound.h"
#include "valtz/models/capabilities.h"
#include "valtz/models/tuning.h"
#include "valtz/project/project.h"
#include "valtz/project/workspace.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace valtz {

struct ControllerConfig {
  // Empty = ~/Library/Application Support/com.tgous.valtz (paths.h);
  // tests point it at a temp dir.
  std::filesystem::path              support_root;
  // Empty = the app's models dir plus vpipe's dev roots when present.
  std::vector<std::filesystem::path> model_roots;
  std::uint64_t                      cache_budget_bytes = 0;  // 0 = auto
  bool                               with_engine = true;
  std::string                        engine_log_level = "info";
  // The UI language (BCP-47, e.g. "en", "zh-Hans"); "" = English. The
  // core itself does not translate (base/message.h); this reaches the
  // engine, whose own user-facing messages follow it where it can.
  std::string                        language;
  // Where EXTENSION packages are looked for (ext/extension.h); empty =
  // the standard roots (the app's Contents/Extensions,
  // <support>/extensions, $VALTZ_EXTENSIONS). Tests point it at a temp
  // dir, or at nothing with `extensions = false`.
  std::vector<std::filesystem::path> extension_roots;
  bool                               extensions = true;
  // A project's working copy is kept when it closes clean too (valtzctl:
  // its history lives on from one run to the next). The app's goes.
  bool                               keep_working_copies = false;
  // The engine to run jobs on, in place of the one `with_engine` picks:
  // a test's own, which takes jobs and runs them when it says.
  std::function<std::unique_ptr<engine::Engine>()> engine_factory;
};

// THE PROMPT IS AN ASSET (DESIGN §10c). Every generation request carries
// its prompt -- words, MENTIONS (U+FFFC, each bound to a medium: the
// request's inline list) and positional TAGS ("<valtz_ref_img_0>": the
// row's first picture, whatever is there) -- with its reference ROW:
// the media of the prompt's row, in order. The core makes the mentions
// the tags of where their media sit, refuses a tag that names nothing,
// and CAPTURES the prompt: a text asset holding tags only (a "prompt"),
// which the result is derived from (recipe input role "prompt") beside
// its media. `prompt_asset`: the prompt the box came from -- reused when
// the words are its own, changed in place when nothing has been made
// from it yet, else a new prompt is made. The model is given its own
// names for the media (<image2>, <Picture 1>), never the tags.

struct GenerateImageRequest {
  ProjectId            project;
  std::string          prompt;
  // The reference row, in order (any kind); empty: the base, then the
  // references.
  std::vector<AssetId> row;
  std::optional<AssetId> prompt_asset;
  std::string          negative;
  std::string          model;        // catalog id; "" = the default
  std::string          name;         // result asset name
  std::int32_t         width = 0;    // 0 = the model's default
  std::int32_t         height = 0;
  std::int32_t         steps = 0;    // 0 = from the model and preference
  // "speed" | "balanced" | "quality": scales the model's default step
  // count (x0.5 / x1 / x1.5) when `steps` is 0. The resolved count is
  // what the recipe records.
  std::string          preference = "balanced";
  // Favor's Custom (models/tuning.h): option values laid on the
  // preference's -- {"steps": 6, "sage_attn": true, ...}; {} = the
  // preset. The settled values are the recipe's "tuning".
  Json                 tuning = Json::object();
  std::int64_t         seed = -1;    // -1 = random (materialized)
  bool                 preview = true;
  // The picture being edited -- resampled (Lanczos) to the output's
  // frame -- if there is one; recorded as the recipe's "base" input.
  std::optional<AssetId> base;
  // Adjustments laid on the base before the model sees it (the Adjust
  // panel, pre-generation): on top of a RAW's development, in linear
  // light, what the stage showed. Recorded as the recipe's
  // "base_adjust" -- part of what made the result.
  media::Adjustments   base_adjust;
  // The base's crop and rotation (the Crop panel; media/crop.h), laid on
  // after the adjustments -- or what the base carries (its "crop"
  // modifier). Recorded as the recipe's "base_crop".
  media::Crop          base_crop;
  // Pictures to draw from: kept at their own pixels (padded to the
  // model's grid, shrunk only when too large).
  std::vector<AssetId> references;
  // `model` "" or "auto": Auto's pick for an image (auto_models): its
  // edit list when the request edits, else its generate list.
  // "auto": an EDIT when there is a base or `references` hold pictures
  // and the model can edit, otherwise text-to-image (a model that cannot
  // edit keeps them as compare-only inputs). "edit" / "generate" force
  // it; an edit the model cannot do is an error. An edit without a base
  // composes a new picture from the references. Width/height 0 on an
  // edit = the base's shape at the model's default area, or the model's
  // default size without a base.
  std::string          mode = "auto";
  // Pictures INLINE in `prompt`: each U+FFFC in it (assist::
  // kInlinePicture) stands for the asset at the same index here -- none
  // for a file not imported, or not a picture. On an edit the model
  // reads each as its tag (catalog `prompting.reference_tag`,
  // "<image2>"), numbered as the edit lists its pictures, the base
  // first; the base at the very start needs none. The recipe records the
  // tagged prompt, and the result's name is the words alone.
  std::vector<std::optional<AssetId>> inline_pictures;
};

// A clip -- and its soundtrack, made with it -- from a prompt (MiniMax H3
// FL2VA), optionally opening on a picture. The result is a derived
// video asset: HEVC Main10 with AAC sound, in an .mp4.
struct GenerateVideoRequest {
  ProjectId            project;
  // Pictures inline in it (U+FFFC) are dropped: a clip reads the words.
  std::string          prompt;
  // The reference row (above); empty: the clip continued, then the
  // references -- or the picture it opens on.
  std::vector<AssetId> row;
  std::optional<AssetId> prompt_asset;
  std::string          model;        // catalog id; "" / "auto" = Auto's
  std::string          name;         // result asset name
  std::int32_t         width = 0;    // 0 = the model's default; edges go
  std::int32_t         height = 0;   //   UP to its grid (32 for H3)
  // 0 = the model's default. Rounded UP to a length the model makes
  // (catalog `frame_grid`, 17n + 5 for H3: 124 frames is 5.2 s at 24
  // fps), and held to its `max_frames`.
  std::int32_t         frames = 0;
  double               fps = 0;      // 0 = the model's
  std::int32_t         steps = 0;    // 0 = from the preference
  std::string          preference = "balanced";
  Json                 tuning = Json::object();  // as for a picture
  std::int64_t         seed = -1;    // -1 = random (materialized)
  // The model's few-step adapter (catalog `turbo`, H3's Turbo LoRA), when
  // it is installed: its own step counts replace the model's. (Custom's
  // "turbo", when it says.)
  bool                 turbo = true;
  // A picture the clip opens on (FL2VA's first keyframe), cropped to fill
  // the frame; recorded as the recipe's "first" input, its adjustments
  // (laid on it as on an edit's base) as "base_adjust".
  std::optional<AssetId> first;
  media::Adjustments   first_adjust;
  media::Crop          first_crop;    // "base_crop", as for an edit
  // REFERENCES (MiniMax H3 Ref2VA; DESIGN §4e): pictures, clips and
  // sounds the clip draws on -- a subject, a motion, a voice, a song --
  // in the prompt row's order, each through its look (a picture) or its
  // trim (a clip, a sound). Read by a model that takes references
  // (Auto's video `edit` pick); never with `first`.
  std::vector<AssetId> references;
  // A clip CONTINUED: its tail -- up to its mark-out -- picture and
  // sound, the clip the new one carries on from.
  std::optional<AssetId> continue_from;
  double               tail_seconds = 0;  // 0: the catalog's (3.75 s)
  // The result's soundtrack is the first sound reference's own, over the
  // clip's length -- a music video's song, as it is -- in place of the
  // one generated with the picture.
  bool                 reference_sound = false;
  // Media INLINE in `prompt` (U+FFFC each, the asset at the same index):
  // with references, each becomes what the model calls it (<Picture 1>,
  // <Video 1>, <Audio 1>); otherwise it is dropped.
  std::vector<std::optional<AssetId>> inline_media;
};

// A SONG from words (YuE2; DESIGN §4d). The prompt holds its style and,
// under section headers, its lyrics (assist::split_song): lyrics make a
// song from them, none make the music from the description alone. The
// model plans a score first (`plan`), then sings and plays to it, and
// decides how long the song runs. The result is a derived audio asset,
// a 16-bit WAV; the score it followed is kept with the version
// (AssetVersion::outputs).
// A layer's clip or picture rendered LARGER by an upscaler (DESIGN §4f):
// what its layer shows -- a clip over its marks (FlashVSR), a picture
// (VOSR) -- restored at its size times the layer's scale, a new asset,
// which the layer then shows instead, its scale divided by as much: it
// looks as it did, at the pixels it is shown at. Its crop has one key at
// most: an upscale renders one size.
struct UpscaleRequest {
  ProjectId    project;
  AssetId      composition;
  std::string  layer;             // "" = layer 0
  std::string  model = "auto";    // an upscaler; auto: the first that runs
  std::int64_t seed = 0;
};

struct GenerateAudioRequest {
  ProjectId            project;
  // Pictures inline in it (U+FFFC) are dropped: a song reads the words.
  std::string          prompt;
  std::vector<AssetId> row;                // the reference row (above)
  std::optional<AssetId> prompt_asset;
  // Lyrics given apart (valtzctl --lyrics, a file's): in place of any in
  // the prompt, which is then the style.
  std::string          lyrics;
  std::string          model;        // catalog id; "" / "auto" = Auto's
  std::string          name;         // result asset name
  // The score planned first: "full" (melody and chords), "melody", or
  // "off" (straight to the song).
  std::string          plan = "full";
  // A score to follow (ABC, in the model's two-voice dialect) instead of
  // one it plans: a cover, an edit of an earlier song. Needs a plan.
  std::string          score;
  // The longest it may run, seconds; 0: the model ends it, up to the
  // catalog's `max_seconds`.
  double               max_seconds = 0;
  std::int32_t         steps = 0;    // flow matching; 0 = the preference's
  std::string          preference = "balanced";
  Json                 tuning = Json::object();  // as for a picture
  std::int64_t         seed = -1;    // -1 = random (materialized)
  // SPEECH (a model that speaks -- MOSS-TTS; DESIGN §4g): the prompt is
  // its fields, then its words (assist::split_speech). `voice`: a sound
  // (or a clip with one) whose voice it speaks in; none: the row's first
  // sound, if any. With one, Auto picks from its audio `edit` list.
  // `seconds`: about how long it runs (0: as the words take).
  std::optional<AssetId> voice;
  double               seconds = 0;
};

// A model QUANTIZED from its source (catalog `quantize`; vpipe's
// model-quantize): Controller::quantize_model.

// An asset written out in another format (engine::export_formats()):
// a picture as a 16-bit PNG / TIFF or OpenEXR, a video as ProRes 4444 or
// HEVC Main10 HDR -- as deep as the asset and tagged with its colour,
// its EXIF carried. The file goes to `destination` (replaced if there);
// the project is not changed. `quality` is a JPEG's, 1..100 (0: vpipe's
// default, 90); a PNG's or TIFF's quality is its depth, the format. A
// still with PAGES writes one file a page, each `destination` numbered
// (page_path); job.finished lists them ("paths").
struct ExportRequest {
  ProjectId             project;
  AssetId               asset;
  std::string           format;
  std::filesystem::path destination;
  int                   quality = 0;   // a JPEG's, 1..100
  // A movie's encoding: H.264's / HEVC's rate, keyframes, B-frames,
  // profile, level, entropy; ProRes's flavour (engine::VideoEncoding).
  engine::VideoEncoding video;
};

// Page `page` (from 0) of `pages`, beside `destination` with its number:
// "Poster.png" -> "Poster-1.png", or "Poster-01.png" of ten or more, so
// the files sort as the pages do.
std::filesystem::path page_path(const std::filesystem::path& destination,
                                std::int64_t page, std::int64_t pages);

// A generation refused for lack of memory, as job.failed carries it: the
// message (core.out_of_memory: what the short budget needed and had),
// "memory" -- vpipe's numbers as they came (SessionIntf::reports): the
// refusal part by part, each budget's need and room, the resource plan
// by phase -- and "suggest": what to change, from the request -- a clip's
// "length" (frames, seconds), the "resolution" (width, height), the
// "references" (count) -- each present when the request had it.
Json out_of_memory_report(const Json& memory, const project::Recipe*);

// A prompt to enhance, for the model it is meant for. A model whose
// makers publish their own rewriter (catalog `prompting.enhance`) gets
// it: the assistant follows those instructions, is shown an edit's
// pictures, and names them as the model does ("<image2>").
struct EnhancePromptRequest {
  std::string prompt;            // may hold inline pictures (U+FFFC)
  std::vector<AssetId> row;      // its tags' row; empty: base, references
  std::string target = "image";  // "image" | "video" | "audio"
  std::string style;
  // The target model; "auto": Auto's pick for the target's generate
  // list; "" = none (Valtz's own instructions, the request's language).
  std::string model;
  std::string mode = "auto";     // as GenerateImageRequest's
  // The output size the user chose ("1600x1216"), told to the rewriter
  // as theirs; "" = theirs to choose.
  std::string size;
  // A song's Length (seconds; 0 = Auto): its lyrics are written to fit.
  double      seconds = 0;
  // An edit's pictures and where they sit in the prompt, as for
  // generate_image; they need the project they are in.
  std::optional<ProjectId>            project;
  std::optional<AssetId>              base;
  std::vector<AssetId>                references;
  std::vector<std::optional<AssetId>> inline_pictures;
  // A clip, as generate_video would make it: its length (0: the
  // model's default), the picture it opens on (`base`, for a model that
  // opens on one), or its references -- `references` all the row's
  // media, the clip continued, a song kept as its soundtrack. A video
  // model's makers' guide (MiniMax H3's) is written to them: the mode,
  // the length, each reference by the model's name.
  int                    frames = 0;
  double                 fps = 0;
  std::optional<AssetId> continue_from;
  bool                   reference_sound = false;
};

class Controller {
public:
  static Result<std::unique_ptr<Controller>> create(ControllerConfig = {});
  ~Controller();

  Controller(const Controller&) = delete;
  Controller& operator=(const Controller&) = delete;

  EventBus& events() noexcept { return _bus; }
  const AppPaths& paths() const noexcept { return _paths; }
  cache::CacheStore& cache() noexcept { return *_cache; }
  std::string version() const;
  std::string engine_description() const;

  // ---- machine and models -------------------------------------------

  const models::HardwareInfo& hardware() const noexcept { return _hw; }
  // Its live load and whether its GPU is held back, as the engine reads
  // them (Engine::machine_status / gpu_thermal): the status bar's.
  // gpu_thermal blocks for `window_ms`.
  Json machine_status();
  Json gpu_thermal(int window_ms = 600);
  // The Log view's rows (log-book.h): the engine's and Valtz's lines
  // after `seq`, at most `max`; and every row cleared.
  Json log_since(std::uint64_t seq, std::size_t max = 4096) const;
  void clear_log();
  const models::Catalog& catalog() const noexcept { return _catalog; }
  std::vector<models::CapabilityStatus> capabilities() const;
  // Settings > Capabilities: the catalog's families (catalog.h Family),
  // each feature checked where a member makes it here, and each member
  // with where it is:
  //   {"families": [{"id", "name",
  //     "features": [{"feature", "available", "why": "ready" |
  //                   "download" | "memory" | "engine"}],
  //     "members": [{"model", "label", "name", "role", "hf_path", "url",
  //                  "state": installed | partial | missing, "bytes",
  //                  "disk_gb", "fits", "min_ram_gb", "path",
  //                  "source": "valtz" | "vpipe" | "link" | "", "license",
  //                  "notes"}]}],
  //    "download_root"}
  Json capability_tree() const;
  // EXTENSIONS (ext/extension.h, DESIGN §8a): every package found at
  // launch, as this run took it --
  //   {"interface": {"current", "oldest", "features", "shapes"},
  //    "extensions": [{"id", "version", "name", "description", "vendor",
  //      "license", "interface", "interface_min", "builtin", "dir",
  //      "state": ready | partial | disabled | refused | backend-failed,
  //      "why", "why_args", "plugins", "backends" (the engine's word on
  //      each), "withheld": [{"item", "why", "detail"}], "models",
  //      "skills", "recognize"}]}
  // with text in the UI's language.
  Json extensions() const;
  // Turn a package off or on (<support>/extensions.json). Taken at the
  // next launch: a backend loads into the engine once per process, and
  // what was made with it stays made.
  Status set_extension_enabled(const std::string& id, bool on);
  // What a dropped weight file (or folder) is: the catalog's recognition
  // rules first, then the heuristics -- models::classify_checkpoint.
  Json classify_weights(const std::filesystem::path& path) const;
  // A model maker's -- or an extension's -- enhancement instructions by
  // name: an extension's (catalog `skills`), then a downloaded resource
  // that provides the name (its `prompting.provides`: QwenLM's own
  // Qwen-Image rewriters, non-commercial, fetched from Capabilities),
  // then the ones compiled in (Valtz's own guide in their place); ""
  // when none.
  std::string skill_text(std::string_view name) const;
  // A model kept somewhere else -- a file or folder anywhere -- linked,
  // or the link undone (ModelStore::link).
  Status link_model(const std::string& id,
                    const std::filesystem::path& target);
  Status unlink_model(const std::string& id);
  // Settings > Storage: what Valtz keeps on the volume of its support
  // folder (the internal SSD), by category -- every model in the
  // models folder it manages, every project package (an open one's
  // assets each), the managed cache -- and the volume's size and free
  // space:
  //   {"volume": {"path", "capacity", "free"},
  //    "models": {"root", "bytes", "items": [{"repo", "names", "bytes"}]},
  //    "projects": {"root", "bytes", "items": [{"name", "path", "bytes",
  //      "open", "ephemeral", "assets": [{"id", "name", "kind", "bytes",
  //      "linked"}], "other"}]},
  //    "cache": {"root", "bytes", "budget"}}
  // Walks the folders: call it off the main thread.
  Json storage_report() const;
  // What Auto picks, per modality and op: the catalog's `auto` order and
  // the first model in it this machine can run -- installed, within its
  // RAM, and the engine has the op's graph. `chosen` is "" when none can
  // (video edits, until Ref2VA's graph lands). generate_image(),
  // generate_video() and generate_audio() use it whenever the request
  // names no model (or "auto"). Audio has a generate list only.
  struct AutoPick {
    std::string              modality;  // "image" | "video" | "audio"
    std::string              op;        // "generate" | "edit"
    std::vector<std::string> order;
    std::string              chosen;
  };
  std::vector<AutoPick> auto_models() const;
  // ---- compositions (DESIGN §6a) ---------------------------------------
  //
  // Only a COMPOSITION -- a still, or a composition on a timeline -- has
  // layers, looks and time; every call below that changes them refuses
  // anything else (kNotComposition). Its layers show other assets
  // (`source`: any but text), each with its look and tracks recorded as
  // the composition's modifiers with the layer's id.

  // The Adjust panel's sliders on a layer ("" = layer 0), recorded as its
  // "adjust" modifier; the Crop panel's crop and turn as its "crop". The
  // identity removes it. A picture's value is a track's first key.
  Status set_adjustments(ProjectId, AssetId, const media::Adjustments&,
                         const std::string& layer = "");
  static media::Adjustments adjustments_of(const project::Asset&,
                                           const std::string& layer = "");
  Status set_crop(ProjectId, AssetId, const media::Crop&,
                  const std::string& layer = "");
  static media::Crop crop_of(const project::Asset&,
                             const std::string& layer = "");
  // A layer's place in TIME: its marks in its source, where it starts on
  // the timeline, how long it runs (project::LayerTime).
  Status set_layer_time(ProjectId, AssetId, const std::string& layer,
                        const project::LayerTime&);
  // Tracks of keyframes, counted from the layer's start: its look, its
  // SPEED and its SOUND (volume, pitch). All identity removes one.
  Status set_adjustment_keys(ProjectId, AssetId,
                             const media::KeyedAdjustments&,
                             const std::string& layer = "");
  Status set_crop_keys(ProjectId, AssetId, const media::KeyedCrop&,
                       const std::string& layer = "");
  Status set_speed_keys(ProjectId, AssetId, const media::KeyedSpeed&,
                        const std::string& layer = "");
  // `follow_speed`: the layer's pitch follows its speed (a tape's) or is
  // held (false); none keeps what it was (media::SoundLayer).
  Status set_sound_keys(ProjectId, AssetId, const media::KeyedSound&,
                        const std::string& layer = "",
                        std::optional<bool> follow_speed = std::nullopt);
  static media::KeyedAdjustments adjustment_keys_of(
      const project::Asset&, const std::string& layer = "");
  static media::KeyedCrop crop_keys_of(const project::Asset&,
                                       const std::string& layer = "");
  static media::KeyedSpeed speed_keys_of(const project::Asset&,
                                         const std::string& layer = "");
  static media::KeyedSound sound_keys_of(const project::Asset&,
                                         const std::string& layer = "");
  static bool pitch_follows_speed(const project::Asset&,
                                  const std::string& layer = "");
  // What happens where two layers overlap in time: "cut" (at the middle)
  // or "dissolve"; "" removes it (project::Transition).
  Status set_transition(ProjectId, AssetId, const std::string& from,
                        const std::string& to, const std::string& kind);
  // A new, blank layer above `above` (a layer's id); its id is returned
  // ("" for the first, then "1", "2", ...).
  // On a still with pages, `page`: the new layer is on that page alone.
  Result<std::string> add_layer(ProjectId, AssetId,
                                const std::string& above = "",
                                std::optional<std::int64_t> page = {});
  // Up (+1) or down (-1). On a frame of its own every layer moves; a
  // stack from before keeps its bottom one.
  Status move_layer(ProjectId, AssetId, const std::string& layer, int by);
  Status set_layer_visible(ProjectId, AssetId, const std::string& layer,
                           bool visible);
  Status rename_layer(ProjectId, AssetId, const std::string& layer,
                      std::string name);
  // What a layer shows -- the content made on it, an asset dropped on its
  // row. Refused where the composition cannot show it (a sound in a
  // still), or it shows the composition (a recursion). Its marks go. A
  // clip dropped on a BLANK layer of a timeline (no `job`) is laid after
  // what is there, as instantiate lays it.
  Status set_layer_source(ProjectId, AssetId, const std::string& layer,
                          AssetId source,
                          std::optional<JobId> job = std::nullopt);
  // A layer gone, with its looks and transitions. The last one stays. On
  // a stack from before frames, the BOTTOM one goes while a layer is
  // above it: the layer above takes its place, placed by a crop exactly
  // where it showed (not on a timeline: hide it instead).
  Status remove_layer(ProjectId, AssetId, const std::string& layer);

  // ---- markup (DESIGN §6a: a MARKUP asset, shown on a layer) ---------
  //
  // The layer the markup toolbar draws on, made when there is none. The
  // top one of `selected` when it is EMPTY -- it gets a new markup, the
  // composition's frame in size -- or already shows a markup; else the
  // top layer when it shows one; else a new layer with a new markup.
  // On a still with pages, `page`: only a layer on that page is drawn
  // on, and a new one is on it alone.
  Result<std::string> markup_layer(ProjectId, AssetId,
                                   const std::vector<std::string>& selected,
                                   std::optional<std::int64_t> page = {});
  // A brush stroke painted into (or erased from) the markup a layer shows.
  Status paint_stroke(ProjectId, AssetId, const std::string& layer,
                      const media::Stroke&);
  // Its vector objects, as the app now has them: kept normalized.
  Status set_markup_objects(ProjectId, AssetId, const std::string& layer,
                            const Json& objects);
  // Objects made pixels: drawn into its raster and gone from its objects.
  Status materialize_markup(ProjectId, AssetId, const std::string& layer,
                            const std::vector<std::string>& objects);
  // Two layers made one -- what they show together, each through its
  // look and the mask between them, as pixels: a FLAT picture the lower
  // layer shows, in its place. Refused where a mask ties either one to a
  // layer outside the two, and on a timeline.
  Status merge_layers(ProjectId, AssetId, const std::string& a,
                      const std::string& b);
  // A layer made the MASK of the layer beneath it (media/layers.h), or
  // released -- a layer again, as it was.
  Status set_layer_mask(ProjectId, AssetId, const std::string& layer,
                        bool mask);
  // The composition's own FRAME, in pixels -- what its layers lie on, and
  // what markup is measured in. A flat asset: its own size.
  Result<media::PixelSize> canvas_size(ProjectId, AssetId);
  // A picture-like asset drawn into one picture (a composition's visible
  // layers, each with its look; anything else as it is), written to `out`
  // as a 16-bit PNG. `look`: one layer's values as a panel holds them now
  // (not yet recorded -- the stage draws while a slider moves). `only`:
  // that layer alone, on the canvas (a layer dragged out). `hidden`:
  // markup objects left out (the app draws the ones it is editing).
  struct LayerLook {
    std::string        layer;
    media::Adjustments adjust;
    media::Crop        crop;
  };
  // `page`: a still's page to draw (DESIGN §6a; its first by default).
  Status flatten(ProjectId, AssetId, const std::filesystem::path& out,
                 const std::optional<LayerLook>& look = std::nullopt,
                 const std::optional<std::string>& only = std::nullopt,
                 const std::set<std::string>& hidden = {},
                 std::int64_t page = 0);
  // The same drawn for the SCREEN: by the GPU into a surface the app's
  // layers show as it is (media::flatten_layers_surface) -- no PNG made
  // and read each time the stage composes it. Retained: the caller
  // releases it.
  Result<IOSurfaceRef> flatten_surface(
      ProjectId, AssetId, const std::optional<LayerLook>& look = std::nullopt,
      const std::optional<std::string>& only = std::nullopt,
      const std::set<std::string>& hidden = {}, std::int64_t page = 0);
  // CANVAS SIZE (media::StackCanvas): the composition shown on a canvas
  // `size` big, anchored -- what lies at the anchor stays put (0, 0.5, 1
  // across and down). Back to its own frame and place: unset.
  Status set_canvas(ProjectId, AssetId, media::PixelSize size,
                    double anchor_x, double anchor_y);
  Status reset_canvas(ProjectId, AssetId);
  Result<media::PixelSize> own_frame(ProjectId, AssetId);
  // A timeline's layers as they are drawn (media::MovieStack): files, the
  // tracks and where each lies in time, its canvas and length. Nested
  // compositions are their renderings. `for_job`: markup drawn into files
  // (a job names files); else drawn in memory. `live`: a layer's tracks
  // as the panels hold them, in place of what is recorded.
  struct LiveTracks {
    std::string              layer;
    media::KeyedAdjustments  adjust;
    media::KeyedCrop         crop;
  };
  Result<media::MovieStack> movie_stack(
      ProjectId, AssetId, bool for_job = false,
      const std::optional<LiveTracks>& live = std::nullopt);
  // A timeline's SOUND as the mixer and the player take it; its mix (a
  // WAV, cached; empty when nothing sounds); its length in its frames.
  Result<media::SoundPlan> sound_plan(ProjectId, AssetId);
  Result<std::filesystem::path> sound_mix(ProjectId, AssetId);
  Result<std::int64_t> composition_length(ProjectId, AssetId);
  // A timeline's length, in its frames; 0: its layers' latest end.
  Status set_timeline(ProjectId, AssetId, std::int64_t frames);
  // A still's PAGES (DESIGN §6a): drawn a page at a time, as a timeline's
  // frames are, with no sound -- a layer's `offset` its first page, its
  // `duration` its pages (0: to the last), its look keyed by page from
  // its own first. A page ADDED after `after` (none: after the last; its
  // index is returned) continues what runs across it -- a layer to the
  // last page, a span reaching past it -- whose keys stay where their
  // pages went; a layer starting later moves with its pages. One unframed
  // is framed where it is. A page REMOVED takes the layers only it had;
  // the others close up, each page showing what it showed.
  static constexpr std::int64_t kMaxPages = 1000;
  Result<std::int64_t> add_page(ProjectId, AssetId,
                                std::optional<std::int64_t> after = {});
  Status remove_page(ProjectId, AssetId, std::int64_t page);
  // A layer of a still on pages [first, first + count) -- count 0: to
  // the last page (its time's offset and duration).
  Status set_layer_pages(ProjectId, AssetId, const std::string& layer,
                         std::int64_t first, std::int64_t count);

  // ---- the asset list (the inspector's Assets) ------------------------
  //
  // FOLDERS group the list ({"id", "name"} each, in order); an asset is
  // in one or at the top (""). Deleting a folder puts its assets at the
  // top.
  Result<Json> folders(ProjectId);
  // How the project was last LOOKED AT (Project::view_state: the app's
  // window size), saved with it: no command -- nothing to undo -- and a
  // write that only marks the project changed when it changes something.
  Result<Json> view_state(ProjectId);
  Status set_view_state(ProjectId, const Json& view);
  Result<std::string> create_folder(ProjectId, std::string name);
  Status rename_folder(ProjectId, const std::string& folder,
                       std::string name);
  Status delete_folder(ProjectId, const std::string& folder);
  Status move_asset(ProjectId, AssetId, const std::string& folder);
  // Its name in the list (one line, at most 200 bytes; empty refused).
  // The project's composition is named by the project, not here.
  Status rename_asset(ProjectId, AssetId, std::string name);
  // The asset gone -- refused while another asset is built from it or
  // shows it on a layer, and for the project's composition.
  Status remove_asset(ProjectId, AssetId);

  // The PROJECT's OUTPUT (DESIGN §6b; project::OutputSettings): what
  // exporting the project writes -- its colour space, a timeline's frame
  // rate, its sound's channels and sample rate; an asset keeps its own.
  // The frame rate is the project timeline's own -- a clip at another
  // rate is resampled to it -- so it changes only while that timeline is
  // empty (kOutputRateSet). A command.
  Result<project::OutputSettings> project_output(ProjectId);
  Status set_project_output(ProjectId, const project::OutputSettings&);
  // The project SET UP (Information › Project): its composition -- a
  // still at `size`, a timeline at `size` and the output's frame rate,
  // sound alone (0 x 0) -- and its output; one command.
  Result<AssetId> set_up_project(ProjectId, project::AssetClass,
                                 media::PixelSize,
                                 const project::OutputSettings&);

  // A new composition: a still (`size`) or a timeline (`size`, 0 x 0 for
  // sound alone; `rate`, 0: 24 fps -- a sound's counts milliseconds).
  // `as_project`, or -- `claim` -- a project that has none: the PROJECT's
  // from now on. A blank one made from the asset list claims nothing.
  Result<AssetId> create_composition(ProjectId, project::AssetClass,
                                     media::PixelSize size,
                                     Rational rate = {0, 1},
                                     std::string name = {},
                                     bool as_project = false,
                                     bool claim = true);

  // ---- capture (DESIGN §7b: a microphone, the system's audio) ---------
  //
  // The sources there now ({"sources": [{"id", "name", "kind",
  // "preferred"}], "microphone": "granted" | "denied" | "undetermined"}),
  // a recording STARTED into a composition of sound alone -- or, with
  // none, into the asset list alone (attached to a prompt) -- one at a
  // time, its state polled ({"recording", "project", "asset", "source",
  // "seconds", "level"}), and STOPPED: the file imported as a flat sound
  // ("<source> <time>") and put on the composition, if any -- in a blank
  // layer, else a new one from its start -- one command. Cancelled,
  // nothing stays.
  Json capture_sources() const;
  Status start_capture(ProjectId, std::optional<AssetId> composition,
                       const std::string& source);
  Json capture_state() const;
  Result<AssetId> stop_capture();
  Status cancel_capture();

  // ---- the CAMERA (DESIGN §7b; media/camera.h) -----------------------
  //
  // The cameras there now ({"cameras": [{"id", "name", "preferred"}],
  // "camera": "granted" | "denied" | "undetermined", "microphone": ...}).
  // One at a time, STARTED for a project and shown -- its newest frame for
  // a preview (camera_frame: the surface retained, the caller's) -- its
  // state polled ({"on", "project", "camera", "name", "recording",
  // "seconds", "width", "height", "frames"}). A STILL taken is a flat
  // picture of the project ("<camera> <time>", a JPEG); a CLIP recorded
  // until stopped, a flat clip (with the default microphone's sound when
  // Valtz may use it) -- each one command. Stopped, the camera is off: a
  // clip being recorded is dropped.
  Json camera_sources() const;
  Status start_camera(ProjectId, const std::string& camera = "",
                      bool sound = true);
  Json camera_state() const;
  media::CameraCapture::Frame camera_frame() const;
  Result<AssetId> camera_snap();
  Status camera_record();
  Result<AssetId> camera_stop_recording();
  Status stop_camera();
  // The PROJECT's composition -- what the stage shows of the work; none
  // until the first generation, or one set up.
  std::optional<AssetId> project_composition(ProjectId);
  Status set_project_composition(ProjectId, AssetId);
  // A result placed in the project's composition (DESIGN §6a). None yet:
  // made of it -- a picture makes a still at its size, a clip a timeline
  // at its size and rate, a song a timeline of sound. A result of the
  // project's kind is a new take in layer 0 (`layer` ""; the look the
  // last one had there goes) or goes on `layer`; while the project is just
  // its take -- the canvas never resized, nothing on another layer, no
  // length set -- the frame follows the take, else it stays and the take
  // lies on it, centred. Another kind is NOT placed: none is returned.
  // `job`: the generation it is the result of -- its command joined.
  Result<std::optional<AssetId>> place_in_project(
      ProjectId, AssetId asset, const std::string& layer = "",
      std::optional<JobId> job = std::nullopt);
  // A RESULT LANDS (DESIGN §3a): as a NEW LAYER of `onto` -- the
  // composition active when it was asked for: in its blank `at`, else
  // right above it, at `offset` on a timeline (instantiate) -- or, with
  // none, or one that cannot show it (a sound in a still), in a
  // composition of ITS OWN: a still at a picture's size, a timeline at a
  // clip's size and rate, a composition of sound alone for a sound --
  // the project's when the project has none. Never resized to the
  // surface: a picture smaller than the canvas lies centred on it. One
  // command with the generation (`job`). The composition and the layer.
  Result<std::pair<AssetId, std::string>> place_result(
      ProjectId, AssetId asset, std::optional<AssetId> onto,
      std::optional<std::string> at = std::nullopt, std::int64_t offset = 0,
      std::optional<JobId> job = std::nullopt);
  // An asset INSTANTIATED in `onto` (what the stage works on; none: the
  // project's composition, made of it when there is none): a NEW LAYER
  // showing it -- in `at` when that layer is blank, else right above it;
  // none, on top -- starting at `offset` on a timeline (a still's page:
  // on it alone; a blank `at` keeps its pages). A CLIP (or a composition
  // that is one) put in a blank layer of a timeline is laid after what
  // is there instead -- from its content's end, a length set growing to
  // hold it (sequenced_). Refused where onto cannot show it, or it
  // shows onto (a recursion). The composition and the layer.
  Result<std::pair<AssetId, std::string>> instantiate(
      ProjectId, AssetId asset, std::optional<AssetId> onto,
      std::optional<std::string> at = std::nullopt,
      std::int64_t offset = 0);
  // A layer showing a composition, DECOMPOSED: replaced by that
  // composition's layers -- copies, with their looks, marks and keys --
  // its placement and time carried over to each. Refused, by name, where
  // they cannot be carried exactly (kDecompose*).
  Status decompose(ProjectId, AssetId, const std::string& layer);
  // The layer's composition made FLAT (flatten_asset) and shown in its
  // place, its marks, look and placement kept: what an upscaler can take
  // (DESIGN §4f) -- one command. The flat asset's id is returned.
  Result<AssetId> flatten_layer(ProjectId, AssetId, const std::string& layer);
  // An EDITED copy of `source` to change instead of it: a flat or
  // generated asset wrapped in a composition of one layer showing it; a
  // composition or a markup, copied.
  Result<AssetId> derive_modified(ProjectId, AssetId source,
                                  std::string name = {});
  // A CAPTURE of `source` as it is: a composition of its layers (all, or
  // `layers`), their looks and keys, its canvas and timeline, what each
  // shows PINNED to the version it shows now. Changing the source later
  // leaves it as it was.
  Result<AssetId> capture(ProjectId, AssetId source,
                          const std::vector<std::string>& layers = {},
                          std::string name = {});
  // A FLAT asset of `asset` as it is drawn: a still or a markup a
  // picture, a timeline a movie with its sound (or a sound), a generated
  // asset a flat copy of its take. A still with pages makes a picture of
  // each ("<name>, page 2"), the first returned. Blocking (a long
  // timeline is encoded).
  Result<AssetId> flatten_asset(ProjectId, AssetId asset,
                                std::string name = {});
  // The FILE an asset is drawn from -- its own for a flat or generated
  // one; a composition's or a markup's rendering, made once and kept in
  // the managed cache under its render key (DESIGN §6a). Blocking.
  Result<std::filesystem::path> rendered(ProjectId, AssetId,
                                         std::uint32_t version = 0);
  // Can this machine run `m` for that modality and op now? (A song needs
  // its decoder installed too.)
  bool runs(const models::ModelEntry& m, std::string_view modality,
            std::string_view op) const;
  // The few-step adapter `m` runs with (its catalog `turbo.lora`) when it
  // is installed; else null.
  const models::ModelEntry* turbo_adapter(const models::ModelEntry& m) const;
  // How many steps `preference` ("speed" | "balanced" | "quality") runs
  // on `m`: the catalog's per-preference `steps` -- its turbo block's,
  // when `turbo` and the adapter is installed -- else the model's default
  // (its edit default for an edit) scaled x0.5 / x1 / x1.5. What a
  // request without `steps` records.
  int steps_for(const models::ModelEntry& m, std::string_view preference,
                bool edit = false, bool turbo = true) const;
  // Favor's options for model `model_id` at `preference` -- their values,
  // ranges and whether this Mac and install can have them -- with
  // `overrides` (Custom's) settled in: models::tuning_options.
  Result<Json> tuning(const std::string& model_id,
                      std::string_view preference, bool edit,
                      const Json& overrides = Json::object()) const;
  // What decides the options beyond the catalog, for `m`.
  models::TuningContext tuning_context(const models::ModelEntry& m) const;
  // What `preference` runs `m` with, as the Favor selector says it: {"steps",
  // "turbo": its adapter's name ("" none), "missing": [{"id", "name"}] --
  // the LoRAs it needs that are not installed -- and the switches it sets,
  // "sol_attn", "sage_attn", "i8_gemm", "motion_cache"}.
  Json preset_summary(const models::ModelEntry& m,
                      std::string_view preference) const;
  const models::ModelEntry* assistant_model() const;
  // The assistant decodes with its MTP head (multi-token prediction: a
  // drafter vpipe's text-chat verifies, token for token the same reply,
  // faster) where its catalog entry says it ships one (`engine.vpipe.mtp`,
  // on unless false). Off: the plain decode, to compare.
  void set_assistant_mtp(bool on) { _assistant_mtp = on; }
  // A catalog assistant to use in place of the tier's pick and the
  // choice, for this run only (valtzctl's --assistant: one measured where
  // the pick would not take it); "" none.
  void set_assistant(std::string id) { _assistant = std::move(id); }
  bool assistant_mtp(const models::ModelEntry& m) const;
  // Settings > Agentic Helper: the helper CHOSEN, by catalog id, "" Auto
  // (the tier's pick). Kept in <support>/assistant.json, so the app and
  // valtzctl take the same one. A choice not installed -- removed,
  // unlinked -- is passed over for Auto until it is back, never refused
  // at Enhance.
  Status choose_assistant(const std::string& id);
  // How long the helper stays LOADED after a request, in seconds (vpipe's
  // text-chat `keep_loaded`: the next request gets it with no read and no
  // warmup -- the 27B's 16 GB took 10-45 s before each first token).
  // Released sooner when a generation needs other weights. 0: unloaded
  // with each request. Kept in assistant.json ("keep_loaded"); 600 by
  // default.
  static constexpr double kDefaultKeepLoaded = 600;
  Status set_assistant_keep_loaded(double seconds);
  double assistant_keep_loaded() const { return _assistant_keep; }
  // The helper's DRAFTER (speculative decoding, token for token the same
  // reply, faster): "mtp" -- its MTP head, or the one shipped apart -- by
  // default; "dflash" -- a DFlash 2 block drafter (catalog
  // engine.vpipe.dflash_drafter), held at `bits` 8 or 4 in memory. It
  // weighs 2.2 GB at 8 bits, 1.3 at 4, beside the 27B's 17 GB, so it is
  // an option, never Auto's. A helper without one, or one not installed,
  // decodes with MTP. Kept in assistant.json ("drafter", "drafter_bits").
  Status set_assistant_drafter(const std::string& kind, int bits);
  // What the page shows: {"choice", "auto" (the pick), "using",
  // "keep_loaded" (seconds), "drafter", "drafter_bits", "models":
  // [{"id", "name", "state", "fits", "disk_gb", "min_ram_gb", "rank",
  // "mtp", "drafter"?: {"id", "state"}, "dflash"?: {"id", "name",
  // "state", "disk_gb"}, "sampling"}]}.
  Json assistants() const;
  // Download a catalog model into the store's download root. A GATED
  // one (catalog `gated`: its publisher's license accepted on Hugging
  // Face first) takes the person's access token, `hf_token`: it goes to
  // vpipe's model-fetch in the job's params, in memory, and nowhere else
  // -- never written, logged or kept past the job (without one vpipe
  // reads $HF_TOKEN). Refused by Hugging Face, the job fails naming why
  // (kDownloadNeedsToken: none or not valid; kDownloadNotGranted: the
  // license not accepted by the token's account).
  Result<JobId> download_model(const std::string& model_id,
                               const std::string& hf_token = "");
  // A QUANTIZED VARIANT made here (catalog `quantize`): its source, which
  // must be installed (downloaded or linked), through vpipe's
  // model-quantize into <download root>/<its hf_path> -- written beside
  // it first and moved in whole, so a stopped one leaves nothing that
  // looks installed. Refused when it is installed already.
  Result<JobId> quantize_model(const std::string& model_id);
  void rescan_models();

  // ---- projects -----------------------------------------------------

  // A project is a DOCUMENT edited in a working copy (project/
  // workspace.h, DESIGN §5b): opened, its saved records are read into one
  // (or one left with unsaved changes is resumed -- project.opened says
  // "recovered"); created, it is saved at once.
  Result<ProjectId> create_project(const std::filesystem::path& package,
                                   std::string name);
  Result<ProjectId> open_project(const std::filesystem::path& package);
  // An UNTITLED project (the app's anonymous session): a working copy at
  // `dir` and no package. Save As names it.
  Result<ProjectId> create_untitled(const std::filesystem::path& dir,
                                    std::string name);
  // Closed: its working copy goes when it is clean, or `discard`.
  Status close_project(ProjectId, bool discard = false);

  // ---- saving, reverting, undoing (DESIGN §5b) ----------------------
  //
  // Save writes the working copy to its package (refused untitled); Save
  // As to a new one, from then on its package; Revert to Saved reads the
  // save back and clears the history (refused while a job of the project
  // runs). Undo / redo the project's history of commands -- each public
  // action below is one (command_) -- while its TASKS run too (DESIGN
  // §3a): a task's result joins its request's command while that is the
  // last, else it is a command of its own. Undoing the request of a task
  // still queued or running WITHDRAWS it -- cancelled, and its result,
  // should one still come, never committed. Each posts project.changed.
  Status save_project(ProjectId);
  Status save_project_as(ProjectId, const std::filesystem::path& package);
  Status revert_project(ProjectId);
  Status undo(ProjectId);
  Status redo(ProjectId);
  // {"dirty", "untitled", "package", "name", "busy",
  //  "undo": {"kind", "args"} | null, "redo": ... | null}
  Json project_state(ProjectId) const;
  // What its open found -- project.opened's: "recovered", "legacy",
  // "missing_extensions".
  Json open_report(ProjectId) const;
  // Its history of commands, oldest first: [{"seq", "kind", "args",
  // "asset", "layer", "at", "done"}] (done false: could be redone).
  Json undo_history(ProjectId) const;
  std::vector<ProjectId> open_projects() const;
  // Borrowed; valid until close_project.
  project::Project* project(ProjectId) const;
  // An EPHEMERAL project (the app's anonymous session) leaves no trace
  // outside its own package: what would go to the shared managed cache
  // (thumbnails) is kept inside the package instead, so deleting the
  // package removes the session from disk.
  Status set_ephemeral(ProjectId, bool);

  // Import files as source assets, in the background. By default video
  // is linked in place and everything else copied (project::Placement).
  Result<JobId> import_files(ProjectId,
                             std::vector<std::filesystem::path> files,
                             project::ImportOptions = {});

  // Check every linked original in the background: follow relocations,
  // turn edited originals into new versions, report missing ones. Runs
  // automatically when a project opens.
  Result<JobId> refresh_links(ProjectId);

  // A JPEG thumbnail of an asset's head version, from the managed cache
  // (made on first request; keyed by content, shared across projects).
  // Blocking on a miss; call off the main thread.
  // As it looks (a stack, a look of its own, drawn) -- or, `plain`, its
  // own file alone (a layer showing its own image).
  Result<std::filesystem::path> thumbnail(ProjectId, AssetId,
                                          int max_px = 256,
                                          bool plain = false);

  // ---- assistant ----------------------------------------------------

  Result<JobId> enhance_prompt(EnhancePromptRequest);
  // Posts the heuristic answer at once; with `use_model`, also the
  // assistant model's refinement if one is installed. (Keep it false
  // while the user types: it loads a multi-GB model.)
  Result<JobId> detect_intent(std::string text,
                              std::vector<assist::Attachment>,
                              bool use_model = true);

  // ---- generation ---------------------------------------------------

  // ---- prompts (DESIGN §10c) -------------------------------------------
  //
  // A PROMPT as an asset: `prompt` with its mentions (`inline`, as a
  // request's) made the tags of where their media sit in `row`; tags
  // that name nothing yet are kept (a captured prompt is a form whose
  // row is filled in later). `from`: the prompt it came from -- reused
  // for its own words, changed in place while nothing is made from it.
  Result<AssetId> capture_prompt(ProjectId, const std::string& prompt,
                                 const std::vector<std::optional<AssetId>>&
                                     inline_media,
                                 const std::vector<AssetId>& row,
                                 std::optional<AssetId> from = std::nullopt);
  // A prompt's words, changed in place -- refused (kPromptInUse) once
  // something has been made from it: it stays as it was, and a copy is
  // what changes.
  Status set_prompt_text(ProjectId, AssetId, const std::string& text);
  // Is it a prompt (a text asset captured as one)?
  static bool is_prompt(const project::Asset&);

  // Define a derived image asset for the request and build it.
  Result<JobId> generate_image(GenerateImageRequest);
  // Define a derived video asset for the request and build it.
  Result<JobId> generate_video(GenerateVideoRequest);
  // What a clip's references are called, as generate_video would name
  // them ({asset: "<Picture 1>"}, the order the model reads them), for
  // `model` ("" / "auto": Auto's video edit pick) -- the prompt row's
  // badges. Refused as generate_video would refuse them.
  Result<Json> video_reference_tags(ProjectId, const std::string& model,
                                    const std::vector<AssetId>& refs,
                                    std::optional<AssetId> continue_from);
  // A clip's GUIDE for a continuation (DESIGN §4e): its last `seconds`
  // (0: the catalog's `tail_frames`) -- rounded UP to a length `model`
  // takes (catalog `frame_grid`: 17n + 5 for H3, so 2 s is 56 frames at
  // 24 fps), down again where the clip is shorter -- on a timeline of
  // their own at the model's rate. A continuation reads a composition
  // WHOLE, so this is exactly what it carries on from (<Video 1>).
  // `model` as for video_reference_tags; `name` "" the clip's. One
  // command: {"asset", "frames", "seconds", "fps"}.
  Result<Json> continuation_guide(ProjectId, AssetId clip, double seconds,
                                  const std::string& model = "",
                                  std::string name = {});
  // A FRAME grabbed from a clip or a timeline: a still composition at its
  // size, one layer showing it from `frame` (its mark-in, in its own
  // frames: the frame a still shows of a clip). One command.
  Result<AssetId> grab_frame(ProjectId, AssetId asset, std::int64_t frame,
                             std::string name = {});
  // `model`'s prompt outline (assist::prompt_outline: its catalog
  // `prompting.outline`), written for a prompt row holding `row`, in
  // `language` ("": the UI's); "" when it has none. What an empty prompt
  // box starts from.
  Result<std::string> prompt_outline(const std::string& model,
                                     const std::vector<assist::RefKind>& row,
                                     std::string_view language = {}) const;
  // Define a derived audio asset -- a song, or speech when the model
  // speaks (MOSS-TTS) -- and build it.
  Result<JobId> generate_audio(GenerateAudioRequest);
  Result<JobId> upscale_layer(UpscaleRequest);
  // Write an asset out (ExportRequest). `job.finished` carries the
  // `path` written.
  Result<JobId> export_asset(ExportRequest);
  // Rebuild a stale (or never-built) derived asset from its recipe.
  Result<JobId> build_asset(ProjectId, AssetId);

  // The project's GENERATION HISTORY (project::HistoryEntry), oldest
  // first: each generation's result as it was made -- a picture, a clip,
  // a song -- and an edit's base as the model got it, recorded when the
  // result is committed. Each with the file of its picture (or movie,
  // or sound).
  struct HistoryState {
    project::HistoryEntry entry;
    std::filesystem::path file;
  };
  Result<std::vector<HistoryState>> history(ProjectId);

  Status cancel(JobId);
  const JobTable& jobs() const noexcept { return _jobs; }

  // ---- the TASK QUEUE (DESIGN §3a) -------------------------------------
  //
  // A TASK is a job that MAKES something -- a generation (a picture, a
  // clip, a song, speech), an upscale, an export -- as against one that
  // serves the person at once (an enhance, an import, a download). Its
  // asset exists from the request (a generation's or an upscale's, with
  // no version until the result is committed; an export makes a file,
  // none). The tasks of every project, in the order they run:
  // [{"job", "project", "asset" ("" an export), "source" (what an export
  // is of; an upscale's composition, with its "layer"), "kind" (generate
  // | upscale | export), "op",
  // "title", "state" (queued | running), "position" (0, 1, ...: how many
  // run before it on its runner -- 0 the one running, T0 in the app),
  // "runner" ("local": this Mac; a peer's, one day -- each runs one at a
  // time), "progress", "destination" (an export's file)}].
  Json tasks() const;

  void shutdown();

private:
  Controller() = default;

  struct OpenProject {
    std::unique_ptr<project::Workspace> ws;
    bool ephemeral = false;
    Json opened = Json::object();  // project.opened's
  };
  project::Workspace* workspace_(ProjectId) const;
  // The command a public action records into (UndoLog::command); a no-op
  // without the project.
  project::UndoLog::Scope command_(ProjectId, std::string kind,
                                   AssetId asset = {},
                                   std::string layer = "",
                                   Json args = Json::object());
  // A job's later writes (its commit, the app placing its result) join
  // the command it was submitted in.
  project::UndoLog::Scope join_(ProjectId, JobId);
  // The extensions whose models the records' recipes use.
  Json extensions_used_(const project::Project&) const;
  // A job of the project is queued or running.
  bool project_busy_(ProjectId) const;
  void post_state_(ProjectId);
  Result<ProjectId> adopt_(std::unique_ptr<project::Workspace>,
                           const project::WorkspaceReport&);

  void post_(std::string kind, JobId job, Json data = Json::object(),
             engine::TensorPtr tensor = nullptr);
  // Record (or, with empty `params`, remove) one modifier of an asset.
  Status set_modifier_(ProjectId, AssetId, const std::string& kind,
                       const std::string& layer, Json params);
  static Json modifier_of_(const project::Asset&, const std::string& kind,
                           const std::string& layer);
  Result<engine::ModelRef> resolve_model_(const models::ModelEntry&,
                                          bool want_preview) const;
  // A preset that needs a LoRA none of whose candidates is installed is
  // refused by name (kPresetLoraMissing) -- unless Custom names its own
  // LoRAs or turns the adapter off.
  Status check_preset_loras_(const models::ModelEntry& m,
                             std::string_view preference,
                             const Json& overrides) const;
  // Favor's settled options `t` into recipe `r`: its steps (`steps` when
  // the request names them), the adapter and branch they name by catalog
  // id, and all of them as "tuning".
  void apply_tuning_(project::Recipe& r, const models::ModelEntry& m,
                     const Json& t, int steps) const;
  Result<JobId> submit_build_(ProjectId, AssetId, std::string title);
  bool is_ephemeral_(ProjectId) const;
  // generate_audio's speech: `m` speaks, `words` its prompt as read.
  Result<JobId> generate_speech_(GenerateAudioRequest&,
                                 const models::ModelEntry& m,
                                 project::Project&, const std::string& words,
                                 const project::RecipeInput& prompt);
  // A sound to clone a voice from: one, or a clip that has one.
  bool voiced_(project::Project&, AssetId) const;
  bool can_edit_(const models::ModelEntry&) const;
  // A request's prompt against its row (capture_prompt's terms): the
  // POSITIONAL text a prompt asset holds; and its MENTION form -- every
  // mention and tag a U+FFFC, each with the medium it names (none:
  // unbound) -- which the models' tagging reads. `bound`: a tag that
  // names nothing is refused (kPromptRefUnbound).
  struct PromptForm {
    std::string                         positional;
    std::string                         text;
    std::vector<std::optional<AssetId>> inline_media;
  };
  Result<PromptForm> prompt_form_(
      project::Project&, const std::string& prompt,
      const std::vector<std::optional<AssetId>>& inline_media,
      const std::vector<AssetId>& row, bool bound);
  // The prompt asset for `positional` (capture_prompt), and its version.
  Result<project::RecipeInput> capture_prompt_(
      project::Project&, ProjectId, const std::string& positional,
      std::optional<AssetId> from);
  // A clip's REFERENCES (generate_video), in the order the model reads
  // them -- those read whole by their files first (pictures, untrimmed
  // sounds), then the spans (the clip continued, clips, trimmed sounds)
  // -- each {"asset", "role", "kind", "route": list | port, "audio" (a
  // clip's own sound), "trim", "tail_frames", "number"}: {"list",
  // "tags": {asset: "<Picture 1>"}, "sound": the first sound's asset}.
  // Held to the catalog's limits.
  Result<Json> video_references_(project::Project&,
                                 const GenerateVideoRequest&,
                                 const models::ModelEntry&, double fps);
  // The model a clip's references are for: `model`, or ("" / "auto")
  // Auto's video edit pick; none when neither is there.
  const models::ModelEntry* reference_model_(const std::string& model);
  // A clip's or a timeline's length in its own frames, and its rate.
  Result<std::pair<std::int64_t, Rational>> clip_frames_(project::Project&,
                                                         const project::Asset&);
  // `images`: pictures the assistant is shown ahead of the text.
  // `song`: a song's request, whose own sectioned lyrics the reply
  // keeps (assist::keep_song_lyrics); "" for anything else.
  // An upscale's layer, to show the result once it is built (by the
  // derived asset): the layer, and the size it was made from and at.
  struct UpscaleTarget {
    AssetId          composition;
    std::string      layer;
    media::PixelSize from;
    media::PixelSize to;
  };
  mutable std::mutex                 _upscale_mu;
  std::map<AssetId, UpscaleTarget>   _upscale_targets;
  // The recording running (capture_*): its project, composition, the
  // source's name.
  mutable std::mutex                   _camera_mu;
  std::unique_ptr<media::CameraCapture> _camera;
  ProjectId                            _camera_project;
  std::string                          _camera_id;
  // What was captured -- a recording, a still, a clip -- made a flat
  // asset of the project (`name`, in `folder`): its import, in the
  // command open.
  Result<AssetId> import_taken_(ProjectId, const std::filesystem::path& file,
                                std::string name, std::string folder);
  mutable std::mutex                   _capture_mu;
  std::unique_ptr<media::SoundCapture> _capture;
  ProjectId                            _capture_project;
  AssetId                              _capture_asset;
  std::string                          _capture_name;
  void apply_upscale_(ProjectId, AssetId made, JobId);
  // How a suggestion is put in the box, in what the person watches and
  // what lands: after a line Valtz writes itself (`lead`: H3's I2VA
  // instruction, fixed text), its model names made the row's tags
  // (`names`, assist::retag).
  struct ReplyForm {
    std::string                                      lead;
    std::vector<std::pair<std::string, std::string>> names;
    std::string apply(std::string prompt) const;
  };
  Result<JobId> submit_chat_(JobId id, std::string purpose,
                             std::string text,
                             std::vector<engine::JobInput> images = {},
                             int max_new_tokens = 512,
                             std::string song = {}, ReplyForm form = {});
  // A clip's request to its makers' guide (enhance_prompt): the mode,
  // its length, the references as the model names them (the pictures
  // shown), the prompt's mentions so named, and how the reply lands.
  void clip_request_(project::Project*, const models::ModelEntry&,
                     const EnhancePromptRequest&,
                     const std::vector<AssetId>& row,
                     assist::EnhanceRequest&,
                     std::vector<engine::JobInput>& images, ReplyForm&);
  // An edit's pictures, as the model is given them: the base (if it is
  // a picture) first, then the references that are pictures.
  std::vector<AssetId> edit_pictures_(project::Project&,
                                      const std::optional<AssetId>& base,
                                      const std::vector<AssetId>& refs);
  void on_build_event_(const engine::JobEvent&);
  void on_export_event_(const engine::JobEvent&);
  void on_chat_event_(const engine::JobEvent&, const std::string& purpose,
                      const std::string& song, const ReplyForm&);
  void finish_job_(JobId, JobState, std::string message);
  // Has the engine a graph for capability `c`? (Grows as graphs land.)
  bool engine_runs_(models::Capability c) const;
  // The last job of `r`'s model and operation that recorded its timing:
  // this session's, else the newest in `p` (a prior, job-timing.h).
  Json timing_prior_(project::Project& p, const project::Recipe& r);
  // The asset's canvas setting (unset when there is none).
  media::StackCanvas canvas_setting_(ProjectId, AssetId);
  // A composition's layers as the pictures a flatten draws: each layer's
  // file or in-memory drawing, with its look -- `look` in place of one
  // layer's, `only` one layer alone, `hidden` markup objects left out.
  // Anything else: itself. `depth`: how deep in nested compositions.
  // `page`: a still's page (the layers on it, their looks there).
  Result<std::vector<media::LayerPicture>> stack_pictures_(
      ProjectId, AssetId, const std::optional<LayerLook>& look,
      const std::optional<std::string>& only,
      const std::set<std::string>& hidden, int depth,
      std::int64_t page = 0);

  // ---- compositions' insides (compositions.cc) -----------------------
  // How deep compositions nest before a cycle is assumed.
  static constexpr int kMaxNesting = 32;
  // `src` may show on `comp`: the right kind, never comp itself or
  // anything that shows it.
  Status check_source_(project::Project&, const project::Asset& comp,
                       const project::Asset& src);
  Status remove_bottom_(project::Project&, ProjectId, const project::Asset&);
  // The size an asset shows at (a composition's canvas, else its frame);
  // a composition's own frame; its rate (one from before: its bottom
  // clip's).
  Result<media::PixelSize> shown_size_(project::Project&, AssetId,
                                       std::uint32_t version, int depth);
  Result<media::PixelSize> own_frame_(project::Project&,
                                      const project::Asset&, int depth);
  // A FRAMED composition resized (Canvas Size): its canvas made its frame
  // -- every layer kept where it lies -- so the frame IS its size. Nothing
  // to do unframed, or with no canvas set.
  Status fold_canvas_(ProjectId, project::Project&, AssetId);
  Rational rate_of_(project::Project&, const project::Asset&);
  // A timeline's length, in its frames.
  Result<std::int64_t> length_of_(ProjectId, AssetId, int depth);
  // A model with QUANTIZED VARIANTS is one model (the app lists it once):
  // the file run is the one its weights are held at -- Favor's 8-bit
  // weights (Fast, Med: tuning "w8_weights") its installed 8-bit pack,
  // else its source; bf16 (Fine) its source when installed, else the
  // pack, which loads as it sits. A tuning that does not say runs `m`.
  const models::ModelEntry& weights_variant_(const models::ModelEntry& m,
                                             const Json& tuning) const;
  // A clip put in a blank layer of a timeline is SEQUENCED: it starts
  // where the content ends -- a timeline is cut together end to end, not
  // stacked where the playhead happens to be -- and a length set shorter
  // grows to hold it. content_end_: the timed layers' latest end, in the
  // timeline's frames, whatever length is set (0: nothing timed).
  static bool sequenced_(const project::Asset& comp,
                         const project::Asset& src);
  Result<std::int64_t> content_end_(ProjectId, AssetId);
  Status grow_to_content_(ProjectId, project::Project&, AssetId comp);
  // Does it sound -- a sound, a clip with a soundtrack, a timeline with
  // anything in it that does?
  bool sounds_(project::Project&, const project::Asset&, int depth);
  // The markup a layer shows (its asset, its content sized), and that
  // content written back.
  Result<std::pair<AssetId, project::Markup>> markup_at_(
      project::Project&, const project::Asset& comp,
      const std::string& layer);
  Status put_markup_(ProjectId, project::Project&, AssetId comp,
                     AssetId markup, project::Markup);
  // The RENDER KEY of an asset (a version of it): its content, or a
  // composition's structure with what it shows, recursively.
  Result<ContentHash> render_key_(project::Project&, AssetId,
                                  std::uint32_t version, int depth);
  // A file under `key` in the managed cache (an ephemeral project's: in
  // its package), made by `make` the first time.
  Result<std::filesystem::path> cached_(
      ProjectId, const std::string& key,
      const std::function<Status(const std::filesystem::path&)>& make);
  Result<std::filesystem::path> rendered_(ProjectId, AssetId,
                                          std::uint32_t version, int depth);
  // A still's page (DESIGN §6a), drawn and cached as its rendering is --
  // its only page without pages; anything else, its rendering.
  Result<std::filesystem::path> rendered_page_(ProjectId, AssetId,
                                               std::int64_t page,
                                               int depth);
  // A flat picture of a drawn asset's page (a still's; else all of it)
  // -- flatten_asset's, in its command.
  Result<AssetId> flat_page_(ProjectId, const project::Asset&,
                             std::int64_t page, std::string name);
  // A picture of frame `frame` of a clip-like asset (the asset itself
  // for anything else).
  Result<std::filesystem::path> rendered_frame_(ProjectId, AssetId,
                                                std::uint32_t version,
                                                std::int64_t frame,
                                                int depth);
  // A timeline's mix (a WAV); empty when nothing in it sounds.
  Result<std::filesystem::path> mixed_(ProjectId, AssetId, int depth);
  Status layer_picture_(ProjectId, project::Project&,
                        const project::Asset& comp, const project::Layer&,
                        media::PixelSize frame,
                        const std::set<std::string>& hidden,
                        media::LayerPicture&, int depth);
  // What a timeline's layer shows, as a writer takes it.
  struct TimedSource {
    std::filesystem::path file;
    media::DrawnPicture   drawn;
    bool                  video = false;
    bool                  audio_only = false;
    double                seconds = 0;           // 0: untimed
    Rational              mark_rate{24, 1};      // its marks' rate
  };
  Result<TimedSource> timed_source_(ProjectId, project::Project&,
                                    const project::Layer&, bool for_job,
                                    media::PixelSize frame, int depth);
  Result<media::MovieStack> movie_stack_(
      ProjectId, AssetId, bool for_job,
      const std::optional<LiveTracks>& live, int depth);
  Result<media::SoundPlan> sound_plan_(ProjectId, AssetId, int depth);
  Result<AssetId> new_composition_(ProjectId, project::Project&,
                                   project::AssetClass,
                                   media::PixelSize size, Rational rate,
                                   std::string name, std::string folder);
  // A committed picture's states into the history: its base as the
  // model got it (an edit's), then the result.
  void record_history_(project::Project& p, const JobRecord& rec,
                       const project::AssetVersion& made);

  // Packages found, their backends handed to the engine, then what each
  // brings laid over the built-in catalog (Controller::create).
  void load_extensions_(engine::EngineConfig& ecfg);
  void take_extensions_();

  ControllerConfig                   _cfg;
  AppPaths                           _paths;
  bool                               _keep_working_copies = false;
  // A job's command (UndoLog seq), by job: what its commit joins.
  std::map<JobId, std::uint64_t>     _job_commands;
  mutable std::mutex                 _job_commands_mu;
  // Tasks whose request was undone: cancelled, their results dropped.
  std::set<JobId>                    _withdrawn;
  // The live tasks a command (`seq`) requested, withdrawn: undoing it
  // takes the request back.
  void withdraw_tasks_(ProjectId, std::uint64_t seq);
  bool withdrawn_(JobId) const;
  std::unique_ptr<cache::CacheStore> _cache;
  models::HardwareInfo               _hw;
  bool                               _assistant_mtp = true;
  std::string                        _assistant;
  std::string                        _assistant_choice;
  double                             _assistant_keep = kDefaultKeepLoaded;
  std::string                        _assistant_drafter = "mtp";
  int                                _assistant_drafter_bits = 8;
  models::Catalog                    _catalog;
  // Every package found at launch, as taken (fixed for the run).
  std::vector<ext::Extension>        _extensions;
  std::unique_ptr<models::ModelStore> _store;
  // Before the engine: it writes here until it is shut down.
  LogBook                            _logbook;
  std::unique_ptr<engine::Engine>    _engine;
  EventBus                           _bus;
  JobTable                           _jobs;
  std::string                        _host;

  // A generation's phase clock (job-timing.h), while it runs; and the
  // last timing of each model and operation ("op|model"), its prior.
  std::mutex                                     _timing_mu;
  std::unordered_map<JobId, JobTiming>           _timing;
  std::unordered_map<std::string, Json>          _priors;

  // Renderings made one at a time (cached_); a rendering asks for those
  // it is made of (a movie its mix, a nested composition) on the same
  // thread.
  std::recursive_mutex                           _render_mu;

  mutable std::mutex                             _projects_mu;
  std::unordered_map<ProjectId, OpenProject>     _projects;

  std::mutex                                     _bg_mu;
  std::vector<std::thread>                       _bg;
  std::atomic<bool>                              _shutting_down{false};
};

}

#endif
