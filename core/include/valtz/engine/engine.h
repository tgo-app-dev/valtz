// The execution-engine seam.
//
// An Engine runs JOBS. A job is described entirely as data -- an
// operation, a resolved model, parameters, input FILES and an output
// directory -- never as pointers into the controller, so the same job can
// run in-process on libvpipe today, in a crash-isolated helper process
// tomorrow, and on a peer Mac over the network later. Each of those is
// just another Engine implementation behind this interface.
//
// Results come back as a stream of JobEvents on a sink the caller
// provides: progress, live TAE previews, streamed LLM text, output files,
// and exactly one terminal event (Finished, Failed or Cancelled). Output
// files are written under JobSpec::output_dir, which the controller
// points at the project's blob tmp/ so a finished build is adopted into
// the blob store with a rename.

#ifndef VALTZ_ENGINE_ENGINE_H
#define VALTZ_ENGINE_ENGINE_H

#include "valtz/base/hash.h"
#include "valtz/base/id.h"
#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/engine/tensor.h"
#include "valtz/media/format.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::engine {

// Operations.
inline constexpr std::string_view kOpGenerateImage = "generate-image";
// An image from a prompt and one or more reference pictures (inputs with
// role "reference"); the family's catalog `engine.vpipe.edit` block says
// how its graph takes them.
inline constexpr std::string_view kOpEditImage = "edit-image";
// A clip and its soundtrack from a prompt (MiniMax H3 FL2VA), optionally
// opening on a picture (input role "first"). Params: prompt, width,
// height, frames (already what the model makes: 17n + 5 for H3), fps,
// steps, seed, and "lora" (the catalog id of the run-time adapter in
// ModelRef::lora) with "lora_scale". The output is one .mp4: HEVC
// Main10 and AAC.
inline constexpr std::string_view kOpGenerateVideo = "generate-video";
// A SONG from words (YuE2): its style and, optionally, its lyrics --
// params "style", "lyrics" (sectioned: [Verse], [Chorus], ...), "cot"
// (the score it plans first: "full", "melody" or "off"), "abc" (a score
// to follow instead), "max_seconds" (0: the model ends it, up to its
// six minutes), "steps" (flow matching), "seed". The decoder is
// ModelRef::vae. The output is a WAV, 16-bit, the decoder's rate (48 kHz
// stereo); the Output event's data carries the "score" it followed
// (ABC), unless it planned none.
inline constexpr std::string_view kOpGenerateAudio = "generate-audio";
// SPEECH from text (MOSS-TTS): params "text" (the words), the prompt's
// whole-utterance fields "instruction", "quality", "sound_event",
// "ambient_sound", "language" ("" unset), "duration_tokens" (about how
// long, 12.5 a second; 0: the model decides), "max_new_tokens", "seed",
// "lm_quant" (how an unquantized LM is held); input "voice" (optional: a
// sound whose voice it speaks in, "voice_start" / "voice_seconds" its
// span). The codec is ModelRef::vae. The output is a WAV, 16-bit, 24 kHz
// mono.
inline constexpr std::string_view kOpGenerateSpeech = "generate-speech";
// A model QUANTIZED (vpipe's model-quantize; catalog `quantize`): the
// source checkpoint is ModelRef::dir, params "output" (the folder made),
// "bits", "group_size". No output file: the folder is the result.
inline constexpr std::string_view kOpQuantizeModel = "quantize-model";
// A clip RESTORED at a larger size (FlashVSR, DESIGN §4f): input
// "source" (a clip), params `width`, `height` (the size made), `first`,
// `count` (its span, in its frames), and the catalog's `upscale` block
// (`group`, `overlap`, `ahead`: how it is fed). The output is an .mp4,
// HEVC, with the source's sound over the span.
inline constexpr std::string_view kOpUpscaleVideo = "upscale-video";
// A picture RESTORED at a larger size (VOSR, DINOv2 its eyes:
// ModelRef::encoder): input "source", params `width`, `height`. The
// output is a 16-bit PNG.
inline constexpr std::string_view kOpUpscaleImage = "upscale-image";
inline constexpr std::string_view kOpChat = "chat";
// A sound -- a clip's -- transcribed: its speech and the sound events
// heard (DESIGN §4h), a summary written as a text file.
inline constexpr std::string_view kOpTranscribeAudio = "transcribe-audio";
inline constexpr std::string_view kOpFetchModel = "fetch-model";
// One input (role "source") written again in another format: a still as
// a 16-bit PNG / TIFF, OpenEXR, or 8-bit; a movie as ProRes 4444 (with
// its alpha) / 422 HQ, HEVC Main10 (keeping HDR) or H.264 -- as deep as
// the source and tagged with its colour; a sound as WAV or AAC, its trim
// (param "trim") cut to the sample. Param `format`: one of
// export_formats(). The output is the file, in `output_dir`.
inline constexpr std::string_view kOpExportMedia = "export-media";

// A format export-media writes: the name a request uses, the file's
// extension, whether it is for movies or for sounds (else stills), and
// an English description (valtzctl's list; the app labels its own).
struct ExportFormat {
  std::string_view name;
  std::string_view extension;
  bool             video;
  std::string_view description;
  bool             sound = false;
};
std::span<const ExportFormat> export_formats();
const ExportFormat* export_format(std::string_view name);

// How a movie export is ENCODED (export-media's param "video"): H.264's
// and HEVC's rate and structure -- what a delivery spec asks for -- and
// ProRes's flavour. Each left 0, -1 or "" is the encoder's own choice.
struct VideoEncoding {
  std::int64_t bitrate = 0;       // average bits per second
  std::int64_t max_bitrate = 0;   // the most bits in any one second
  double       quality = 0;       // 0..1: a rate of its own, no bitrate
  double       keyframe_seconds = 0;  // the most from one keyframe to the
                                      // next
  int          b_frames = -1;     // 1 allowed, 0 not
  // H.264: baseline | main | high (its level 3.0 .. 5.2, entropy cabac |
  // cavlc). HEVC: main10 (hevc10's own) | main (8-bit).
  std::string  profile;
  std::string  level;
  std::string  entropy;
  // ProRes: 4444 | 4444xq for prores4444; 422hq | 422 | 422lt | 422proxy
  // for prores422hq.
  std::string  prores;

  bool empty() const;
};
Json to_json(const VideoEncoding& e);
VideoEncoding video_encoding_from_json(const Json& j);
// Why `e` cannot be written as `format` -- a setting of another codec, a
// profile at a level H.264 has not -- or "" when it can.
std::string video_encoding_problem(const VideoEncoding& e,
                                   std::string_view format);
// The codec vpipe's avf-save-video writes `format` with, `e`'s ProRes
// flavour or HEVC profile taken.
std::string video_codec(std::string_view format, const VideoEncoding& e);

// A model resolved on the machine that runs the job.
struct ModelRef {
  std::string           id;       // catalog id
  std::string           family;
  std::filesystem::path dir;      // checkpoint directory
  // The TAE that decodes live previews, if previewing: its file, or a
  // folder holding one.
  std::filesystem::path preview;
  // A run-time adapter the recipe applies (param "lora": the catalog id
  // of MiniMax H3's Turbo LoRA or HyperFlow), as its file; empty when
  // none.
  std::filesystem::path lora;
  // The second LoRA slot (param "lora2_file", "lora2_scale": a style or
  // identity adapter of one's own); empty when none.
  std::filesystem::path lora2;
  // Community checkpoints in place of the model's own parts (params
  // "dit_file", "vae_file"): a .safetensors or a folder; empty when none.
  // A generator decoded by a checkpoint of its own (YuE2: catalog
  // `decoder`, param "decoder") is given it here.
  std::filesystem::path dit;
  std::filesystem::path vae;
  // A second checkpoint beside the model's (param "branch": H3's VDN
  // linear attention branch), as its folder; empty when none.
  std::filesystem::path branch;
  // A vision tower the conditioner reads (VOSR's DINOv2: param "encoder",
  // a catalog id), as its folder; empty when none.
  std::filesystem::path encoder;
  Json                  engine = Json::object();  // catalog engine block
};

struct JobInput {
  std::string           role;  // "reference", "init_image", ...
  std::filesystem::path path;
  ContentHash           hash;
  media::MediaInfo      info;
};

struct JobSpec {
  JobId                 id;
  std::string           op;
  ModelRef              model;
  Json                  params = Json::object();
  std::vector<JobInput> inputs;
  std::filesystem::path output_dir;
};

// What a running job is doing: a Progress event's PHASE (data "phase").
// An engine names the phase it can see and counts it as finely as it can
// (a denoise per transformer block, a decode per tile, a download per
// byte); the app words it. PREPARE is everything before the first count
// -- loading models, reading the prompt -- and FINISH everything after
// the last: writing the result.
inline constexpr std::string_view kPhasePrepare = "prepare";
// The pictures the job draws on, encoded.
inline constexpr std::string_view kPhaseReferences = "references";
// A song's SCORE, planned before it is sung (ABC), and the SONG itself
// written, a token per 40 ms: uncounted -- the model decides when each
// ends -- with data "made", what it has so far (the score's tokens, the
// song's seconds). Its denoise follows: the flow matching, counted.
inline constexpr std::string_view kPhaseScore = "score";
inline constexpr std::string_view kPhaseSong = "song";
// SPEECH spoken, a frame per 80 ms: counted against the length asked
// for, else uncounted with data "made", its seconds so far. Its codec's
// decode follows (SOUND).
inline constexpr std::string_view kPhaseSpeech = "speech";
inline constexpr std::string_view kPhaseDenoise = "denoise";
// The picture, or the clip's frames, out of the latent.
inline constexpr std::string_view kPhaseDecode = "decode";
// A clip's soundtrack out of its latent.
inline constexpr std::string_view kPhaseSound = "sound";
inline constexpr std::string_view kPhaseRestore = "restore";
// A model's files; data "label" names the one being counted (none
// between them: vpipe counts only the big ones).
inline constexpr std::string_view kPhaseDownload = "download";
// An export's frames, written: counted by vpipe's writer, with Preview
// events of the frames going in -- a small picture twice a second, data
// {"frame": the timeline's frame} -- so a long clip's export shows on the
// stage as a generation does.
inline constexpr std::string_view kPhaseExport = "export";
inline constexpr std::string_view kPhaseFinish = "finish";
// Anything else; data "label" is the engine's own words.
inline constexpr std::string_view kPhaseWork = "work";

enum class JobEventKind : std::uint8_t {
  Started,
  Progress,   // the phase: data {"phase", "done", "total" (0 =
              // indeterminate), "detail", "label", "elapsed" (s, the
              // phase's), "estimate" (0..1: the count and the way into
              // its next unit at the recent pace; -1 uncounted), "rate"
              // (of the whole per s; 0 unknown), "made" (an uncounted
              // phase's own measure; absent when none)}; progress =
              // estimate.
              // At least once a second while a phase counts.
  Preview,    // tensor: a TAE decode of the in-flight latent, planar
              // [C,H,W] (or [F,C,H,W] for video: data carries "frames"
              // and "fps"), straight alpha, sRGB
  Text,       // a chunk of streamed LLM output
  Output,     // output file (+ info), or data for text results
  Finished,   // terminal: success
  Failed,     // terminal: error / message
  Cancelled,  // terminal
};

const char* to_str(JobEventKind);

struct JobEvent {
  JobId                         job;
  JobEventKind                  kind = JobEventKind::Progress;
  float                         progress = -1.0f;  // 0..1, -1 unknown
  std::int32_t                  step = 0;
  std::int32_t                  steps = 0;
  std::string                   text;
  TensorPtr                     tensor;
  std::filesystem::path         output;
  media::MediaInfo              output_info;
  Json                          data;
  Code                          error = Code::Ok;

  bool terminal() const noexcept
  {
    return kind == JobEventKind::Finished || kind == JobEventKind::Failed ||
           kind == JobEventKind::Cancelled;
  }
};

// Called from engine threads, in order per job.
using JobSink = std::function<void(const JobEvent&)>;

class Engine {
public:
  virtual ~Engine() = default;

  // e.g. "vpipe 0.1 (55b66ae2)", recorded in every version it builds.
  virtual std::string description() const = 0;
  virtual bool available() const = 0;
  virtual bool supports(std::string_view op) const = 0;

  // Queue a job. Returns once queued; the sink receives its events.
  virtual Status submit(JobSpec, JobSink) = 0;
  // Ask a queued or running job to stop. Its sink still receives a
  // terminal event.
  virtual Status cancel(JobId) = 0;
  // Stop everything and join engine threads. Idempotent.
  virtual void shutdown() = 0;

  // THE MACHINE, as the engine's own front ends read it (vpipe's web UI
  // status bar and its macOS app's thermal row: vpipe/system-monitor.h),
  // so the status bar shows the same numbers, read the same way. Empty
  // objects from an engine that cannot read them.
  //
  // Its live load: {"gpu_util_pct", "ane_util_pct", "phys_footprint_bytes"
  // (this process: the engine runs in it, models and all),
  // "phys_total_bytes", ...} (the controller adds "sys_used_bytes", the
  // whole Mac's). The ANE's figure is a delta against the
  // previous call: poll at a steady pace. Thread-safe.
  virtual Json machine_status() { return Json::object(); }
  // Whether the GPU is being held back now: {"verdict": unknown | idle |
  // normal | warm | throttled, "clock_mhz", "ceiling_mhz",
  // "gpu_active_pct", "temp_c", "os_thermal_state"}. Samples for
  // `window_ms` -- blocks for it; call it off the UI's thread.
  virtual Json gpu_thermal(int window_ms)
  {
    (void)window_ms;
    return Json::object();
  }

  // The EXTENSIONS' backends (EngineConfig::backends), as this engine
  // took them: [{"extension", "file", "state": "loaded" | "refused" (kept
  // out of the session) | "not-loaded" (vpipe did not register its
  // stages), "why", "args"}]. Settled when the engine starts: a plugin
  // loads once per process.
  virtual Json backends() const { return Json::array(); }
};

// A vpipe plugin an EXTENSION carries (ext/extension.h, DESIGN §8a): its
// backend, loaded into the engine's session beside Valtz's own. The
// engine checks it first against the vpipe it runs on -- the plugin ABI
// window and the host features the plugin requires -- so a plugin vpipe
// would refuse is named, and kept out of the session; vpipe makes the
// final check, from the file. Then it asks for each stage the plugin says
// it registers, as it does for valtz-vpipe.so: vpipe reports a failed
// load only in its log.
struct BackendPlugin {
  std::string              extension;  // the package's id
  std::filesystem::path    file;
  int                      abi = 0;    // the vpipe plugin ABI it targets
  std::vector<std::string> required;   // vpipe host features it needs
  std::vector<std::string> stages;     // stage types it registers
};

struct EngineConfig {
  std::filesystem::path state_dir;  // engine-private state (vpipe's LMDB)
  std::filesystem::path temp_dir;
  std::string           log_level = "info";
  std::string           language;   // UI language (BCP-47); "" = English
  // valtz-vpipe.so, the plugin data crosses through; empty = find it
  // (the app bundle's PlugIns, beside the executable, the build tree).
  std::filesystem::path vpipe_plugin;
  // Every line the engine reports -- its stages' errors, warnings and
  // info, its log at or above `log_level` -- as it reports it, on its
  // own threads: `level` 0 error, 1 warn, 2 info, 3..6 the log's
  // normal, verbose, debug, always. Quick and thread-safe.
  std::function<void(int level, std::string_view text)> on_log;
  // The extensions' vpipe plugins (Engine::backends says how each went).
  std::vector<BackendPlugin> backends;
};

// An engine that runs nothing; used when Valtz is built without libvpipe.
std::unique_ptr<Engine> make_null_engine();
// The in-process libvpipe engine (valid only when VALTZ_HAVE_VPIPE).
std::unique_ptr<Engine> make_vpipe_engine(const EngineConfig&);
// The best engine this build has.
std::unique_ptr<Engine> make_default_engine(const EngineConfig&);

}

#endif
