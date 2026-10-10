// Builds vpipe pipeline specs (the JSON `load_pipeline` accepts) for
// Valtz jobs. Wiring follows vpipe's own reference graphs in
// docs/pipelines -- the text-to-image shape is identical across Z-Image,
// Krea-2 and Qwen-Image-2.1; a family's differences live in its
// catalog `engine.vpipe` block (config stage, scheduler, defaults).
//
// Data leaves a graph through `valtz-sink` stages (the valtz-vpipe
// plugin, see host-exchange.h); a BuiltGraph names the ones the engine
// reads.

#ifndef VALTZ_ENGINE_VPIPE_GRAPH_BUILDER_H
#define VALTZ_ENGINE_VPIPE_GRAPH_BUILDER_H

#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/engine/engine.h"
#include "valtz/media/adjust.h"
#include "valtz/media/keyframes.h"
#include "valtz/media/model-input.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace valtz::engine::vp {

// A picture an edit draws on (or a clip opens on), and the size it
// DISPLAYS at (EXIF orientation applied) -- measured by the engine, so
// the builder stays free of file I/O. The BASE is the picture being
// edited (first when present); the others are plain REFERENCES --
// components to draw from. An edit may have no base: then it composes
// from the references.
struct RefImage {
  std::filesystem::path path;
  media::PixelSize      size;
  bool                  has_alpha = false;
  bool                  base = false;
};

// A clip's REFERENCE (MiniMax H3 Ref2VA; DESIGN §4e), in the order the
// model reads it: read WHOLE by its file (vpipe's `references` list: a
// picture, a sound) or a SPAN on a reference port (a clip, a trimmed
// sound, a clip's tail) -- cut exactly by vpipe's own stages, from the
// file: load-video, video-to-rgb, temporal-slice and temporal-stack for
// the picture; load-audio, audio-to-pcm at the audio VAE's 32 kHz and
// temporal-stack for the sound, attached to its clip.
struct RefMedia {
  std::filesystem::path path;
  std::string           kind;          // "image" | "video" | "audio"
  bool                  list = true;   // else a span on a port
  bool                  audio = false; // a clip's own sound, attached
  int                   width = 0;     // a clip's frame
  int                   height = 0;
  double                fps = 24;
  // A clip's frames [first, first + count); count -1: to its end.
  std::int64_t          first = 0;
  std::int64_t          count = -1;
  // The sound's span, seconds (duration 0: to its end) -- a clip's, its
  // frames'; a sound's, its trim.
  double                start_s = 0;
  double                duration_s = 0;
};

// A picture the engine must feed after launch: decode `path` into a
// lease of `valtz-source` stage `stage` as F16 [channels, h, w] at `size`
// (its own size, capped for memory). The graph then fits it with
// Lanczos -- `image-resample` stage `stage`-fit -- to `fitted`, which
// is what BOTH the VAE and the conditioner read (through one list, in
// order: kRefList):
//   base        cropped to fill the output size;
//   reference   kept at its pixels, padded right and bottom to the
//               model's grid -- shrunk first only when it is larger
//               than the model's `reference_area`.
struct RefFeed {
  std::string           stage;
  std::filesystem::path path;
  media::PixelSize      size;
  int                   channels = 3;
  bool                  base = false;
  media::PixelSize      fitted;
  // The base's pre-generation adjustments (recipe param "base_adjust"),
  // laid on its development as it is decoded: what the stage showed --
  // or an export's image modifier (job param "adjust").
  media::Adjustments    adjust;
  // Decode in extended linear sRGB, unclamped (media::Space::Linear) and
  // tag the beat so: a file that keeps light (OpenEXR).
  bool                  linear = false;
  // The picture's crop (recipe param "base_crop", or an export's image
  // modifier "crop"): laid on after the adjustments, so what is fed is
  // the crop's CANVAS -- `size` is that shape.
  media::Crop           crop;
};

// An edit's two lists: the fitted pictures (vpipe's tensor-list) and the
// latents vae-encode makes from them, read by the conditioner and the
// DiT through their list ports.
inline constexpr char kRefList[] = "refs";
inline constexpr char kRefLatents[] = "refs-latents";

// The base's EXIF, on its way to the result: a valtz-source whose one
// beat is {"exif_tiff_b64": ...} (empty when the base has none), read by
// save-image's `metadata` port -- which writes it into the file and sets
// Software to "Valtz <version> (Vpipe ... with <model>)".
inline constexpr char kExifSource[] = "base-exif";
inline constexpr char kExifKey[] = "exif_tiff_b64";

// A clip the engine must feed frame by frame -- an export of a clip with
// a look: its frames [first, first + count) decoded by Valtz, each with
// its adjustments and crop at its frame (media::decode_movie), into
// leases of valtz-source `stage` as F16 [3, h, w] at `size`, tagged with
// `tags` (the clip's colour and rate).
struct MovieFeed {
  std::string             stage;
  std::filesystem::path   path;
  media::KeyedAdjustments adjust;
  media::KeyedCrop        crop;
  // A clip with a layer stack, a canvas or a timeline of its own: drawn
  // whole, frame by frame (media::decode_movie_stack), in place of the
  // look above.
  std::optional<media::MovieStack> stack;
  Rational                rate{24, 1};
  std::int64_t            first = 0;
  std::int64_t            count = -1;
  media::PixelSize        size;
  Json                    tags = Json::object();
  // The colour to draw a stack in -- a project's output, an export of the
  // project -- with `tags` to match; none: its bottom clip's.
  std::optional<media::ColorInfo> color;
  // 8-bit for a model that reads bytes (an upscaler's source).
  media::Sample           sample = media::Sample::F16;
  // FED BY WHAT COMES OUT (an upscaler, DESIGN §4f): the graph takes the
  // clip in `groups` groups of `group` frames, each starting `overlap`
  // frames before the last one ended -- the feeder sends those again, so
  // every group is whole and nothing is left over at the end -- and
  // gives group - overlap frames per group past `tap` (a valtz-tap). A
  // group is fed only once the groups before it have come out -- but up
  // to `ahead` of them, while the memory left (reclaimable RAM, sensed at
  // each group) holds twice `group_bytes` per group ahead and a reserve:
  // the next group's frames, rows and decode then overlap the last's, and
  // no more is ever upscaled inside the graph than that. The last group
  // is filled with the clip's last frame; the tap cuts what those make.
  // group 0: not gated.
  struct Gate {
    std::string   tap;
    int           group = 0;
    int           overlap = 0;
    int           ahead = 0;
    std::int64_t  groups = 0;
    std::uint64_t group_bytes = 0;
  };
  Gate                    gate;
};

// A clip the engine must READ SPARSE into valtz-source `stage` (a video
// summary, DESIGN §4i): the frame at every `every` seconds, upright,
// sRGB, planar u8 [3, h, w] at `size` (media::sample_movie), each beat's
// sideband its time in the clip (`pts_us`).
struct SampleFeed {
  std::string           stage;
  std::filesystem::path path;
  double                every = 1;
  media::PixelSize      size;
};

// Sink stage ids; empty when the graph has no such output.
struct BuiltGraph {
  Json                  spec;
  // An export's frames, as many as its writer is told to expect (its
  // progress); for a clip vpipe reads itself, where its preview is read
  // from: `preview_from`, frame n at preview_start + n / preview_fps s.
  std::int64_t          frames = 0;
  std::filesystem::path preview_from;
  double                preview_start = 0;
  double                preview_fps = 24;
  std::string           preview_sink;  // live TAE previews (latest only)
  std::string           text_sink;     // streamed text chunks
  std::string           result_sink;   // the final text result
  std::string           score_sink;    // a song's score (ABC text)
  // A sound transcribed (DESIGN §4h): its lines of speech, and the
  // tagger's windows of sound events.
  std::string           transcript_sink;
  std::string           events_sink;
  // A clip summarized (DESIGN §4i): a beat a scene, then the whole's.
  std::string           summary_sink;
  std::optional<SampleFeed> samples;
  std::filesystem::path output;        // file the graph writes
  // A still's pages exported (DESIGN §6a): every file, `output` the
  // first; empty for anything else.
  std::vector<std::filesystem::path> pages;
  std::vector<RefFeed>  references;    // edit inputs, in port order
  std::filesystem::path exif_from;     // the base, when kExifSource is
                                       // wired; else empty
  int                   width = 0;     // the image the graph makes
  int                   height = 0;
  // A clip's two halves, which the graph writes apart (avf-save-video
  // is video only) and the engine joins into `output`
  // (media::add_soundtrack). Empty for anything but a movie. The
  // picture is always scratch; the sound is too unless
  // `audio_is_source` (an export takes it from the file it reads).
  std::filesystem::path video_part;
  std::filesystem::path audio_part;
  bool                  audio_is_source = false;
  // A trimmed export's sound: the span of the source to take, seconds
  // (duration 0: all of it).
  double                audio_start = 0;
  double                audio_duration = 0;
  std::optional<MovieFeed> movie;  // a clip fed frame by frame
};

Result<BuiltGraph> build_text_to_image(const JobSpec&);
// upscale-video (DESIGN §4f): a clip -- input "source", its span `first`,
// `count` -- restored at `width` x `height` by FlashVSR, as vpipe's own
// graph does it (docs/pipelines/flashvsr-upscale-*): resampled to the
// model's grid, stacked `group` frames at a time overlapping by
// `overlap` (the feeder sends the overlap again), projected, denoised,
// decoded, fitted to the size. The frames
// go in 8-bit through a valtz-source GATED by a valtz-tap before the
// writer (MovieFeed::Gate); the tap cuts the padded tail. The sound, the
// source's own span, is joined after.
Result<BuiltGraph> build_upscale_video(const JobSpec&);
// upscale-image (DESIGN §4f): a picture -- input "source" -- restored at
// `width` x `height` by VOSR, as vpipe's own graph does it
// (docs/pipelines/vosr-2-restore): resampled bicubic to its grid (16),
// read by DINOv2 (ModelRef::encoder) and the VAE, restored in one step,
// decoded in F16, fitted to the size, written as a 16-bit PNG. The
// picture goes in through a valtz-source, F16 at its own size.
Result<BuiltGraph> build_upscale_image(const JobSpec&);
// Sizes (DESIGN §4b / the catalog's `engine.vpipe.edit` block): the
// output defaults to the BASE's shape at the model's default area (the
// model's default size when there is no base), aligned to `vpipe.align`;
// see RefFeed for how each picture is fitted.
Result<BuiltGraph> build_image_edit(const JobSpec&,
                                    const std::vector<RefImage>&);
// generate-video (DESIGN §4c): a prompt -- and, when the family takes
// one (`vpipe.first_frame`), a picture to open on, cropped to fill the
// frame -- into a clip and its soundtrack. The family's config stage
// carries its shifts, the Turbo LoRA (job.model.lora) and the preview
// TAE; the clip is decoded in F16 and written as HEVC Main10, the sound
// as a WAV, joined afterwards. Width and height go up to `vpipe.align`,
// as vpipe takes them; the frame count is the caller's (the controller
// rounds it to what the model makes).
//
// With REFERENCES (Ref2VA: the catalog's `vpipe.references` block), the
// prompt and the references go through a `video-ref-encoder` in place of
// the diffusion-conditioner -- its conditioning to generate-video's port
// 0, its reference video and audio rows to ports 7 and 8 -- and, with
// param "reference_sound", the clip's soundtrack is the first sound
// reference's own (joined over the clip's length), not a generated one.
Result<BuiltGraph> build_video(const JobSpec&,
                               const std::optional<RefImage>& first,
                               const std::vector<RefMedia>& refs = {});
// generate-audio (DESIGN §4d): a song from words, in the shapes of
// vpipe's YuE2 graphs (docs/pipelines/yue2-text-to-music,
// yue2-songs-from-lyrics). The style, the score's plan, the seed and the
// flow matching's steps and tiers are generate-audio's config; LYRICS go
// in as a prompt beat (text-prompt, one beat: one song), as the
// songs-from-lyrics graph feeds them -- without lyrics the port is
// unwired and the stage makes one song from its config. The latents are
// decoded by the model's decoder (job.model.vae) and written as a WAV;
// the score the song followed comes back through a sink.
Result<BuiltGraph> build_audio(const JobSpec&);
// generate-speech: the words spoken by MOSS-TTS (vpipe's text-to-speech,
// the moss-tts-v1.5-speak graph) -- the prompt's fields its config, the
// codec job.model.vae, a voice to clone read from its file (input
// "voice") into its reference port -- written as a WAV.
Result<BuiltGraph> build_speech(const JobSpec&);
// quantize-model: vpipe's model-quantize, job.model.dir into params
// "output" ("bits", "group_size").
Result<BuiltGraph> build_quantize_model(const JobSpec&);
Result<BuiltGraph> build_chat(const JobSpec&);
// A sound TRANSCRIBED (DESIGN §4h) from `wav` -- one channel at 16 kHz,
// the engine's (media::mono_sound): its speech line by line, and its
// sound events when the job names a tagger.
Result<BuiltGraph> build_transcribe(const JobSpec&,
                                    const std::filesystem::path& wav);
// summarize-video (DESIGN §4i): the clip -- input "source" -- read sparse
// at `size` (a frame every param `every` s: g.samples) into Valtz's
// valtz-video-summary, which cuts it into scenes and has the model
// (job.model: the helper, loaded as its chat loads it -- `mtp_model`,
// `draft_model`, `draft_bits`, `keep_loaded`; its `sampling` on a
// sampler-select) tell each in `language`, then the whole. Its beats
// reach g.summary_sink.
Result<BuiltGraph> build_summarize_video(const JobSpec&,
                                         media::PixelSize size);
Result<BuiltGraph> build_fetch_model(const JobSpec&);
// export-media: the job's one input file read by vpipe's own Apple-native
// readers and written by its writers -- a still through load-image (F16,
// its colour as declared, its EXIF carried) and save-image; a movie
// through avf-load-video and avf-save-video, its soundtrack joined back
// afterwards (those stages are video only). Files in, files out: no
// pixel crosses into Valtz. The asset's image modifiers go into the file
// as it is made (params "adjust", "crop": the still decoded through a
// valtz-source; "trim": the movie read from its mark-in to its
// mark-out, frame for frame, and its sound with it).
Result<BuiltGraph> build_export(const JobSpec&);

}

#endif
