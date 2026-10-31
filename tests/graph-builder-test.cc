// The vpipe graph builder: how edit graphs are wired and sized for each
// family, straight from the built-in catalog. No engine, no models.

#include "testing.h"

#include "engine/vpipe/encoding-cache.h"
#include "engine/vpipe/graph-builder.h"
#include "engine/vpipe/job-progress.h"
#include "valtz/cache/cache-store.h"
#include "valtz/models/catalog.h"
#include "valtz-vpipe/exchange.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstring>
#include <format>
#include <fstream>
#include <set>

using namespace valtz;
using namespace valtz::engine;
namespace ex = valtz::exchange;
namespace fs = std::filesystem;

namespace {

JobSpec
job_for(const std::string& model, std::string_view op)
{
  static auto cat = models::Catalog::builtin();
  JobSpec j;
  j.id = JobId::make();
  j.op = std::string(op);
  if (const auto* e = cat.ok() ? cat->find(model) : nullptr) {
    j.model.id = e->id;
    j.model.family = e->family;
    j.model.engine = e->engine;
  }
  j.model.dir = "/models/" + model;
  j.params = {{"prompt", "a scarf"}, {"steps", 20}, {"seed", 7}};
  j.output_dir = "/tmp/out";
  return j;
}

const Json*
find_stage(const Json& spec, const std::string& id)
{
  for (const auto& s : spec["stages"]) {
    if (jget<std::string>(s, "id", "") == id) {
      return &s;
    }
  }
  return nullptr;
}

std::string
src_of(const Json* stage, std::size_t port)
{
  if (!stage || !(*stage)["iports"].is_array() ||
      port >= (*stage)["iports"].size()) {
    return "<none>";
  }
  return jget<std::string>((*stage)["iports"][port], "src", "");
}

int
oport_of(const Json* stage, std::size_t port)
{
  if (!stage || !(*stage)["iports"].is_array() ||
      port >= (*stage)["iports"].size()) {
    return -1;
  }
  return jget((*stage)["iports"][port], "oport", -1);
}

// The fitted pictures the list stage gathers, in list order.
std::vector<std::string>
list_items(const Json& spec)
{
  std::vector<std::string> out;
  const Json* l = find_stage(spec, vp::kRefList);
  if (!l) {
    return out;
  }
  for (const auto& e : (*l)["iports"]) {
    out.push_back(jget<std::string>(e, "src", ""));
  }
  return out;
}

// One list, both sides: the conditioner's ref_images (6) and one
// vae-encode whose latents list (oport 1) is generate-image's
// ref_latents (8). `to_cond`: the family's text encoder sees pictures.
void
check_lists(const Json& spec, bool to_cond)
{
  const Json* lst = find_stage(spec, vp::kRefList);
  REQUIRE(lst);
  CHECK(jget<std::string>(*lst, "type", "") == "tensor-list");
  const Json* enc = find_stage(spec, vp::kRefLatents);
  REQUIRE(enc);
  CHECK(jget<std::string>(*enc, "type", "") == "vae-encode");
  CHECK(src_of(enc, 0).empty());                 // no single image
  CHECK(src_of(enc, 2) == vp::kRefList);          // the list
  const Json* gen = find_stage(spec, "generate-image");
  CHECK(src_of(gen, 8) == vp::kRefLatents);
  CHECK(oport_of(gen, 8) == 1);
  CHECK(src_of(gen, 5).empty());                  // the single ports idle
  CHECK(src_of(gen, 6).empty());
  const Json* cond = find_stage(spec, "diffusion-conditioner");
  CHECK(src_of(cond, 3).empty());
  CHECK(src_of(cond, 4).empty());
  if (to_cond) {
    CHECK(src_of(cond, 6) == vp::kRefList);
  } else {
    CHECK(src_of(cond, 6) != vp::kRefList);
  }
}

Json
config_of(const Json* stage)
{
  return stage ? jget((*stage), "config", Json::object()) : Json::object();
}

const Json*
config_stage(const Json& spec)
{
  return find_stage(spec, "qwen-image-21-model-config");
}

}

// Qwen-Image 2.1, editing a BASE: the picture is fed at its own size and
// ONE Lanczos resample -- cropped to the output's frame -- feeds both the
// VLM conditioner and vae-encode; the output follows the base's shape at
// ~1 MP on a 32-pixel grid. The config stage's vision bounds cover the
// fitted picture, so the tower reads exactly what the VAE reads.
TEST(graph_builder, qwen_base_is_resampled_once_for_both_encoders)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  REQUIRE(!job.model.engine.empty());
  auto g = vp::build_image_edit(job, {{"/in/photo.heic", {4032, 3024},
                                       false, /*base=*/true}});
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 1);
  const auto& f = g->references[0];
  CHECK(f.base);
  CHECK(f.size.width == 4032);            // fed at its own size
  CHECK(f.size.height == 3024);
  CHECK(g->width == 1152);
  CHECK(g->height == 864);
  CHECK(f.fitted.width == g->width);       // fitted to the output frame
  CHECK(f.fitted.height == g->height);
  CHECK(f.channels == 3);

  const Json& spec = g->spec;
  const Json* src = find_stage(spec, f.stage);
  REQUIRE(src);
  CHECK(jget<std::string>(*src, "type", "") == exchange::kSourceType);
  const Json* fit = find_stage(spec, f.stage + "-fit");
  REQUIRE(fit);
  CHECK(jget<std::string>(*fit, "type", "") == "image-resample");
  CHECK(src_of(fit, 0) == f.stage);
  const Json fc = config_of(fit);
  CHECK(jget(fc, "width", 0) == 1152);
  CHECK(jget(fc, "height", 0) == 864);
  CHECK(jget<std::string>(fc, "fit", "") == "crop");
  CHECK(jget<std::string>(fc, "algorithm", "") == "lanczos");
  // The same fitted picture into both encoders, through one list.
  CHECK(list_items(spec) == std::vector<std::string>{f.stage + "-fit"});
  check_lists(spec, /*to_cond=*/true);

  const Json cc = config_of(config_stage(spec));
  CHECK(jget(cc, "vl_max_pixels", 0) >= 1152 * 864);
  CHECK(jget(cc, "vl_pixel_budget", 0) >= 1152 * 864);
  CHECK(jget(cc, "vl_min_pixels", 1 << 30) <= 1152 * 864);
}

// A size asked for wins over the base's shape (the app always asks): the
// base fills that frame, centre-cropped, and the size is put on the grid.
TEST(graph_builder, qwen_edit_follows_the_size_asked_for)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  job.params["width"] = 1024;
  job.params["height"] = 1000;  // -> 992 on the 32-pixel grid
  auto g = vp::build_image_edit(job, {{"/in/photo.png", {1600, 1200},
                                       false, /*base=*/true}});
  REQUIRE_OK(g);
  CHECK(g->width == 1024);
  CHECK(g->height == 992);
  const Json fc = config_of(find_stage(g->spec, "ref-0-fit"));
  CHECK(jget(fc, "width", 0) == 1024);
  CHECK(jget(fc, "height", 0) == 992);
  CHECK(jget<std::string>(fc, "fit", "") == "crop");
}

// The base's EXIF reaches the result: a source the engine fills, into
// save-image's metadata port; and Software names Valtz. Without a base
// there is nothing to carry, and the port stays unwired.
TEST(graph_builder, base_exif_is_linked_to_the_result)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  auto g = vp::build_image_edit(job, {{"/in/photo.jpg", {1600, 1200},
                                       false, /*base=*/true}});
  REQUIRE_OK(g);
  CHECK(g->exif_from == "/in/photo.jpg");
  const Json* src = find_stage(g->spec, vp::kExifSource);
  REQUIRE(src);
  CHECK(jget<std::string>(*src, "type", "") == exchange::kSourceType);
  const Json* save = find_stage(g->spec, "save-image");
  REQUIRE(save);
  CHECK(src_of(save, 0) == "vae-decode");
  CHECK(src_of(save, 1) == vp::kExifSource);
  CHECK(jget<std::string>(config_of(save), "software_host", "")
            .starts_with("Valtz "));

  auto compose = vp::build_image_edit(job, {{"/in/fox.png", {512, 512},
                                             false, /*base=*/false}});
  REQUIRE_OK(compose);
  CHECK(compose->exif_from.empty());
  CHECK(!find_stage(compose->spec, vp::kExifSource));
  const Json* save2 = find_stage(compose->spec, "save-image");
  REQUIRE(save2);
  CHECK(jget(*save2, "iports", Json::array()).size() == 1);
}

// A result keeps the decoder's precision: F16 out of vae-decode, a
// 16-bit PNG out of save-image.
TEST(graph_builder, results_are_sixteen_bit)
{
  auto g = vp::build_text_to_image(job_for("qwen-image-2.1",
                                           kOpGenerateImage));
  REQUIRE_OK(g);
  const Json* dec = find_stage(g->spec, "vae-decode");
  REQUIRE(dec);
  CHECK(jget<std::string>(config_of(dec), "dtype", "") == "f16");
  const Json* save = find_stage(g->spec, "save-image");
  REQUIRE(save);
  CHECK(jget(config_of(save), "bit_depth", 0) == 16);
}

// A picture's models kept BUILT between jobs (DESIGN §4n): the engine's
// "keep_models" reaches the stages that build them -- the conditioner,
// generate-image, vae-decode -- as vpipe's `keep_loaded`; without it they
// are let go with the graph, as before.
TEST(graph_builder, a_pictures_models_are_kept_warm_as_asked)
{
  auto job = job_for("qwen-image-2.1", kOpGenerateImage);
  job.params["keep_models"] = 600.0;
  auto g = vp::build_text_to_image(job);
  REQUIRE_OK(g);
  for (const char* id : {"diffusion-conditioner", "generate-image",
                         "vae-decode"}) {
    const Json* st = find_stage(g->spec, id);
    REQUIRE(st);
    CHECK(jget(config_of(st), "keep_loaded", 0.0) == 600.0);
  }
  auto cold = vp::build_text_to_image(job_for("qwen-image-2.1",
                                              kOpGenerateImage));
  REQUIRE_OK(cold);
  const Json* gen = find_stage(cold->spec, "generate-image");
  REQUIRE(gen);
  CHECK(jget(config_of(gen), "keep_loaded", -1.0) == 0.0);
}

// export-media: files in, files out. A still is read by load-image
// (F16, ImageIO; a camera RAW developed in the camera's look, or linear
// for OpenEXR) and written by save-image, its EXIF wired across -- a
// JPEG at the quality asked, vpipe's own when none is; a movie by
// avf-load-video and avf-save-video; a format for the other kind is
// refused.
// A gated repo's token reaches the fetch stage's config -- the one place
// it goes -- and a download without one has none.
TEST(graph_builder, a_fetch_carries_its_token)
{
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpFetchModel);
  job.params = {{"hf_path", "krea/Krea-2-Turbo"}, {"base_path", "/m"},
                {"hf_token", "hf_abc"}};
  auto g = vp::build_fetch_model(job);
  REQUIRE_OK(g);
  const Json* f = find_stage(g->spec, "model-fetch");
  REQUIRE(f);
  CHECK(jget<std::string>((*f)["config"], "hf_token", "") == "hf_abc");
  job.params.erase("hf_token");
  auto plain = vp::build_fetch_model(job);
  REQUIRE_OK(plain);
  CHECK(!find_stage(plain->spec, "model-fetch")->at("config")
             .contains("hf_token"));
  CHECK(!find_stage(plain->spec, "model-fetch")->at("config")
             .contains("source"));
  // From a source named (Controller::download_model): vpipe's to read.
  job.params["source"] = "modelscope";
  auto mirror = vp::build_fetch_model(job);
  REQUIRE_OK(mirror);
  CHECK(jget<std::string>(find_stage(mirror->spec, "model-fetch")
                              ->at("config"), "source", "") == "modelscope");
}

// A transcription (DESIGN §4h): the sound as one channel at 16 kHz, the
// voice detector's spans and the PCM into the speech model, the tagger
// beside them only when it is installed; refused without the detector.
TEST(graph_builder, a_transcription_hears_one_channel)
{
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpTranscribeAudio);
  job.model.dir = "/m/Qwen/Qwen3-ASR-0.6B";
  job.params = {{"vad_path", "/m/silero.mlpackage"},
                {"tagger_path", "/m/beats.mlpackage"},
                {"language", "English"}};
  auto g = vp::build_transcribe(job, "/tmp/x-mono.wav");
  REQUIRE_OK(g);
  const Json* load = find_stage(g->spec, "load");
  const Json* pcm = find_stage(g->spec, "pcm");
  const Json* asr = find_stage(g->spec, "asr");
  const Json* tags = find_stage(g->spec, "tags");
  REQUIRE(load && pcm && asr && tags);
  CHECK(jget<std::string>(config_of(load), "input_url", "") ==
        "/tmp/x-mono.wav");
  CHECK(jget(config_of(pcm), "channels", 0) == 1);
  CHECK(jget(config_of(pcm), "output_sample_rate", 0) == 16000);
  // Its 32 ms chunks wait for the detector: a long sound is heard whole.
  CHECK(jget(config_of(pcm), "oport_capacity", 0) > 0);
  CHECK(src_of(asr, 0) == "pcm");
  CHECK(src_of(asr, 1) == "voice");
  CHECK(jget<std::string>(config_of(asr), "hf_dir", "") ==
        "/m/Qwen/Qwen3-ASR-0.6B");
  CHECK(jget<std::string>(config_of(asr), "language_hint", "") ==
        "English");
  CHECK(jget<std::string>(config_of(tags), "model_kind", "") == "beats");
  CHECK(g->transcript_sink == "transcript-sink");
  CHECK(g->events_sink == "events-sink");

  job.params.erase("tagger_path");
  job.params.erase("language");
  auto plain = vp::build_transcribe(job, "/tmp/x-mono.wav");
  REQUIRE_OK(plain);
  CHECK(!find_stage(plain->spec, "tags"));
  CHECK(plain->events_sink.empty());
  CHECK(!config_of(find_stage(plain->spec, "asr")).contains("language_hint"));

  job.params.erase("vad_path");
  CHECK(!vp::build_transcribe(job, "/tmp/x-mono.wav").ok());
}

// A song SEPARATED (DESIGN §4j): the engine's two-channel WAV read at the
// model's rate, through vpipe's audio-separate -- the checkpoint by its
// folder and its file -- each output to a sink of its own, every beat
// kept (a source is written as it streams out).
TEST(graph_builder, a_song_is_separated_into_two_sinks)
{
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpSeparateAudio);
  job.model.dir = "/m/pcunwa/BS-Roformer-Resurrection";
  job.model.engine = {{"vpipe", {{"separate", {{"rate", 44100},
                                               {"stem", "vocals"},
                                               {"residual",
                                                "accompaniment"}}}}}};
  job.params = {{"checkpoint", "BS-Roformer-Resurrection.ckpt"}};
  auto g = vp::build_separate(job, "/tmp/x-heard.wav");
  REQUIRE_OK(g);
  const Json* load = find_stage(g->spec, "load");
  const Json* pcm = find_stage(g->spec, "pcm");
  const Json* sep = find_stage(g->spec, "separate");
  const Json* stem = find_stage(g->spec, g->stem_sink);
  const Json* rest = find_stage(g->spec, g->residual_sink);
  REQUIRE(load && pcm && sep && stem && rest);
  CHECK(jget<std::string>(config_of(load), "input_url", "") ==
        "/tmp/x-heard.wav");
  CHECK(jget(config_of(pcm), "channels", 0) == 2);
  CHECK(jget(config_of(pcm), "output_sample_rate", 0) == 44100);
  CHECK(jget(config_of(pcm), "flush_on_eos", false));
  CHECK(src_of(sep, 0) == "pcm");
  CHECK(jget<std::string>(config_of(sep), "hf_dir", "") ==
        "/m/pcunwa/BS-Roformer-Resurrection");
  CHECK(jget<std::string>(config_of(sep), "checkpoint", "") ==
        "BS-Roformer-Resurrection.ckpt");
  CHECK(src_of(stem, 0) == "separate");
  CHECK(src_of(rest, 0) == "separate");
  CHECK((*stem)["iports"][0]["oport"] == 0);
  CHECK((*rest)["iports"][0]["oport"] == 1);
  CHECK(jget<std::string>(config_of(stem), "policy", "") == "queue");
  CHECK(!vp::build_separate(job, "").ok());
}

// A video summary (DESIGN §4i): the clip read sparse into a valtz-source
// at the size given, the helper's model loaded as its chat loads it (its
// drafters, its warm hold), its sampler on the stage's port, Valtz's own
// words for the scenes and the whole.
// FlashVSR, as vpipe restores a long source now: ONE STREAM across the
// groups (or overlapping clips it cuts itself), so the groups are
// consecutive -- 24, no overlap, none sent twice, the last short -- the
// denoiser is planned for the biggest beat (a clip of 41, or a stream's
// opening chunks), and the feed allows for what the encoder holds back
// before anything comes out.
TEST(graph_builder, an_upscale_streams_its_groups)
{
  JobSpec job = job_for("flashvsr", kOpUpscaleVideo);
  job.params = {{"width", 1920}, {"height", 1088}};
  JobInput in;
  in.role = "source";
  in.path = "/tmp/clip.mp4";
  in.info.type = media::MediaType::Video;
  in.info.frame.width = 960;
  in.info.frame.height = 544;
  in.info.frame_rate = {24, 1};
  in.info.frame_count = 100;
  job.inputs.push_back(in);
  auto g = vp::build_upscale_video(job);
  REQUIRE_OK(g);
  const Json* stack = find_stage(g->spec, "stack");
  const Json* enc = find_stage(g->spec, "encode");
  const Json* gen = find_stage(g->spec, "generate");
  const Json* tap = find_stage(g->spec, "tap");
  REQUIRE(stack && enc && gen && tap);
  CHECK(jget(config_of(stack), "group_size", 0) == 24);
  CHECK(jget(config_of(stack), "overlap", -1) == 0);
  CHECK(jget(config_of(enc), "clip_frames", 0) == 41);
  CHECK(jget(config_of(enc), "blend_frames", 0) == 8);
  CHECK(!config_of(enc).contains("continuity"));  // vpipe decides
  CHECK(jget(config_of(gen), "frames", 0) == 41);
  CHECK(jget(config_of(gen), "width", 0) == 1920);
  CHECK(jget(config_of(gen), "height", 0) == 1152);
  CHECK(jget(config_of(tap), ex::kTapMaxFrames, 0) == 100);
  REQUIRE(g->movie);
  CHECK(g->movie->gate.group == 24);
  CHECK(g->movie->gate.lag == 40);
  CHECK(g->movie->count == 100);
  // A bigger group: a stream's beat is its six chunks, more than a clip.
  job.params["group"] = 48;
  auto big = vp::build_upscale_video(job);
  REQUIRE_OK(big);
  CHECK(jget(config_of(find_stage(big->spec, "generate")), "frames", 0) ==
        48);
  // Nothing asked, nothing laid on: vpipe's own defaults.
  CHECK(!config_of(gen).contains("i8_gemm"));
  // Settings › Upscaling's arithmetic (the recipe's tuning) on the
  // denoiser, where FlashVSR's load reads it.
  job.params["tuning"] = {{"i8_gemm", true}, {"sage_attn", true},
                          {"sol_attn", false}};
  auto fast = vp::build_upscale_video(job);
  REQUIRE_OK(fast);
  const Json fg = config_of(find_stage(fast->spec, "generate"));
  CHECK(jget(fg, "i8_gemm", false) && jget(fg, "sage_attn", false));
  CHECK(!jget(fg, "sol_attn", true));
}

// The clips Valtz makes are kept in the project's INTERMEDIATE format
// (OutputSettings, the job's "intermediate"): each ProRes in a .mov, its
// sound joined as PCM; HEVC Main10 in an .mp4 at the clip's quality, its
// sound AAC; one not known -- a newer Valtz's -- ProRes 422 HQ. A
// generated clip and an upscaled one alike.
TEST(graph_builder, clips_are_kept_in_the_projects_intermediate_format)
{
  JobSpec clip = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  clip.params = {{"prompt", "a fox"}, {"width", 832}, {"height", 480},
                 {"frames", 39}, {"fps", 24.0}};
  JobSpec up = job_for("flashvsr", kOpUpscaleVideo);
  up.params = {{"width", 1920}, {"height", 1088}};
  JobInput in;
  in.role = "source";
  in.path = "/tmp/clip.mov";
  in.info.type = media::MediaType::Video;
  in.info.frame.width = 960;
  in.info.frame.height = 544;
  in.info.frame_rate = {24, 1};
  in.info.frame_count = 48;
  in.info.has_audio = true;
  up.inputs.push_back(in);
  struct Case {
    const char* asked;
    const char* codec;
    const char* ext;
    media::SoundCodec sound;
  };
  for (const Case& c : {
           Case{"prores4444xq", "prores4444xq", ".mov",
                media::SoundCodec::Pcm},
           Case{"prores4444", "prores4444", ".mov", media::SoundCodec::Pcm},
           Case{"prores422", "prores422", ".mov", media::SoundCodec::Pcm},
           Case{"hevc", "hevc", ".mp4", media::SoundCodec::Aac},
           Case{"dnxhr", "prores422hq", ".mov", media::SoundCodec::Pcm}}) {
    clip.params["intermediate"] = c.asked;
    auto g = vp::build_video(clip, std::nullopt);
    REQUIRE_OK(g);
    CHECK(g->output.extension() == c.ext);
    CHECK(g->video_part.extension() == c.ext);
    CHECK(g->sound == c.sound);
    const Json save = config_of(find_stage(g->spec, "save-video"));
    CHECK(jget<std::string>(save, "codec", "") == c.codec);
    CHECK(save.contains("quality") == (c.sound == media::SoundCodec::Aac));

    up.params["intermediate"] = c.asked;
    auto u = vp::build_upscale_video(up);
    REQUIRE_OK(u);
    CHECK(u->output.extension() == c.ext);
    CHECK(u->video_part.extension() == c.ext);
    CHECK(u->sound == c.sound);
    CHECK(u->audio_is_source);
    const Json write = config_of(find_stage(u->spec, "write"));
    CHECK(jget<std::string>(write, "codec", "") == c.codec);
    // Opaque: no alpha asked of the writer, so no plane in a 4444.
    CHECK(jget<std::string>(write, "alpha", "") == "drop");
    CHECK(jget<std::string>(save, "alpha", "") == "drop");
  }
}

// FlashVSR+ (Turbo, Base) restores in its own cadence -- a stream's
// opening chunk of 17 source frames, then 16 a chunk; clips of 49 -- so
// the denoiser is planned for a clip of 49 and the feed allows for 48
// held back; and its tiny decoder reads the source pictures beside each
// latent, on the encoder's second output wired to the decode's third
// input. v1.1's decode stays as vpipe's v1.1 graph has it: two inputs.
TEST(graph_builder, flashvsr_plus_restores_in_its_own_cadence)
{
  for (const char* id : {"flashvsr-plus-turbo", "flashvsr-plus-base"}) {
    JobSpec job = job_for(id, kOpUpscaleVideo);
    job.model.dir = std::string("/m/ruchangyao/FlashVSR_plus/") +
                    (std::string_view(id).ends_with("turbo") ? "turbo"
                                                             : "base");
    job.params = {{"width", 1280}, {"height", 704}};
    JobInput in;
    in.role = "source";
    in.path = "/tmp/clip.mp4";
    in.info.type = media::MediaType::Video;
    in.info.frame.width = 640;
    in.info.frame.height = 352;
    in.info.frame_rate = {24, 1};
    in.info.frame_count = 56;
    job.inputs.push_back(in);
    auto g = vp::build_upscale_video(job);
    REQUIRE_OK(g);
    const Json* sel = find_stage(g->spec, "model-select");
    const Json* enc = find_stage(g->spec, "encode");
    const Json* gen = find_stage(g->spec, "generate");
    const Json* dec = find_stage(g->spec, "decode");
    REQUIRE(sel && enc && gen && dec);
    CHECK(jget<std::string>(config_of(sel), "hf_dir", "") ==
          job.model.dir.string());
    CHECK(jget(config_of(enc), "clip_frames", 0) == 49);
    CHECK(jget(config_of(enc), "blend_frames", 0) == 8);
    CHECK(jget(config_of(gen), "frames", 0) == 49);
    CHECK(jget(config_of(gen), "width", 0) == 1280);
    CHECK(jget(config_of(gen), "height", 0) == 768);   // the 128 grid
    REQUIRE(g->movie);
    CHECK(g->movie->gate.group == 24);
    CHECK(g->movie->gate.lag == 48);
    REQUIRE((*dec)["iports"].size() == 3);
    CHECK(src_of(dec, 0) == "generate");
    CHECK(src_of(dec, 2) == "encode");
    CHECK(oport_of(dec, 2) == 1);
    // Nothing settled, nothing laid on: the release's own colour fix.
    CHECK(!config_of(enc).contains("color_fix"));
    // Settled (Settings › Upscaling, the recipe's tuning): on the encoder,
    // the stage the catalog's option names -- not on the denoiser.
    job.params["tuning"] = {{"color_fix", true}, {"i8_gemm", true}};
    auto fixed = vp::build_upscale_video(job);
    REQUIRE_OK(fixed);
    CHECK(jget(config_of(find_stage(fixed->spec, "encode")), "color_fix",
               false));
    CHECK(!config_of(find_stage(fixed->spec, "generate"))
               .contains("color_fix"));
    job.params.erase("tuning");
    // A group of 40: a stream's later beat is its 48 frames (three
    // chunks of 16), still under a clip.
    job.params["group"] = 40;
    auto big = vp::build_upscale_video(job);
    REQUIRE_OK(big);
    CHECK(jget(config_of(find_stage(big->spec, "generate")), "frames",
               0) == 49);
    job.params["group"] = 64;
    auto bigger = vp::build_upscale_video(job);
    REQUIRE_OK(bigger);
    CHECK(jget(config_of(find_stage(bigger->spec, "generate")), "frames",
               0) == 64);
  }
  JobSpec v11 = job_for("flashvsr", kOpUpscaleVideo);
  v11.params = {{"width", 1280}, {"height", 704}};
  JobInput in;
  in.role = "source";
  in.path = "/tmp/clip.mp4";
  in.info.type = media::MediaType::Video;
  in.info.frame.width = 640;
  in.info.frame.height = 352;
  in.info.frame_rate = {24, 1};
  in.info.frame_count = 56;
  v11.inputs.push_back(in);
  auto g = vp::build_upscale_video(v11);
  REQUIRE_OK(g);
  CHECK((*find_stage(g->spec, "decode"))["iports"].size() == 2);
}

// VOSR's restorer takes the same arithmetic, Sol among it, on its
// generate stage -- the one step it was distilled for left to vpipe.
TEST(graph_builder, a_picture_upscale_runs_as_tuned)
{
  JobSpec job = job_for("vosr", kOpUpscaleImage);
  job.params = {{"width", 1280}, {"height", 704}, {"seed", 3},
                {"tuning", {{"i8_gemm", true}, {"sage_attn", false},
                            {"sol_attn", true}, {"sol_tau", 1.5}}}};
  // It reads the picture's own size from its file: a 1 x 1 PNG.
  const fs::path png = test::temp_dir("gb-upscale") / "small.png";
  {
    static const unsigned char k1x1[] = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00,
        0x0d, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4, 0x89,
        0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63,
        0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4,
        0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60,
        0x82};
    std::ofstream(png, std::ios::binary)
        .write(reinterpret_cast<const char*>(k1x1), sizeof(k1x1));
  }
  JobInput in;
  in.role = "source";
  in.path = png;
  in.info.type = media::MediaType::Image;
  in.info.frame.width = 1;
  in.info.frame.height = 1;
  job.inputs.push_back(in);
  auto g = vp::build_upscale_image(job);
  REQUIRE_OK(g);
  const Json gen = config_of(find_stage(g->spec, "generate"));
  CHECK(jget(gen, "seed", 0) == 3);
  CHECK(jget(gen, "i8_gemm", false) && jget(gen, "sol_attn", false));
  CHECK(!jget(gen, "sage_attn", true));
  CHECK(jget(gen, "sol_tau", 0.0) == 1.5);
  CHECK(!gen.contains("steps"));
}

TEST(graph_builder, a_video_summary_reads_sparse)
{
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpSummarizeVideo);
  job.model.dir = "/m/mlx-community/Qwen3.8-27B-4bit";
  JobInput in;
  in.role = "source";
  in.path = "/tmp/clip.mov";
  in.info.type = media::MediaType::Video;
  job.inputs.push_back(in);
  job.params = {{"every", 2.0},
                {"language", "Simplified Chinese"},
                {"keep_loaded", 600.0},
                {"mtp_model", "/m/mtp"},
                {"draft_model", "/m/dflash"},
                {"draft_bits", 4},
                {"context_tokens", 262144},
                {"sampling", {{"temperature", 0.7}, {"top_k", 20}}}};
  auto g = vp::build_summarize_video(job, {544, 320});
  REQUIRE_OK(g);
  REQUIRE(g->samples);
  CHECK(g->samples->stage == "frames");
  CHECK(g->samples->path == fs::path("/tmp/clip.mov"));
  CHECK(g->samples->every == 2.0);
  CHECK(g->samples->size.width == 544);
  CHECK(g->samples->size.height == 320);
  const Json* frames = find_stage(g->spec, "frames");
  const Json* sum = find_stage(g->spec, "summary");
  const Json* smp = find_stage(g->spec, "sampler");
  REQUIRE(frames && sum && smp);
  CHECK(jget<std::string>(*frames, "type", "") == ex::kSourceType);
  CHECK(jget<std::string>(*sum, "type", "") == ex::kSummaryType);
  CHECK(src_of(sum, 0) == "frames");
  CHECK(src_of(sum, 1) == "sampler");
  const Json cfg = config_of(sum);
  CHECK(jget<std::string>(cfg, ex::kSummaryModel, "") ==
        "/m/mlx-community/Qwen3.8-27B-4bit");
  CHECK(jget(cfg, ex::kSummaryEvery, 0.0) == 2.0);
  CHECK(jget<std::string>(cfg, ex::kSummaryLanguage, "") ==
        "Simplified Chinese");
  CHECK(jget<std::string>(cfg, ex::kSummaryMtpModel, "") == "/m/mtp");
  CHECK(jget<std::string>(cfg, ex::kSummaryDraftModel, "") == "/m/dflash");
  CHECK(jget(cfg, ex::kSummaryDraftBits, 0) == 4);
  CHECK(jget(cfg, ex::kSummaryKeepLoaded, 0.0) == 600.0);
  CHECK(jget<std::string>(cfg, ex::kSummaryScenePrompt, "")
            .find("{start}") != std::string::npos);
  CHECK(jget<std::string>(cfg, ex::kSummaryOverallPrompt, "")
            .find("{scenes}") != std::string::npos);
  CHECK(jget(config_of(smp), "top_k", 0) == 20);
  // bf16 unless the helper computes in another (Ternary-Bonsai's f16),
  // as its chat does: one model warm for both.
  CHECK(!cfg.contains(ex::kSummaryComputeDtype));
  {
    JobSpec f16 = job;
    f16.params["compute_dtype"] = "f16";
    auto fg = vp::build_summarize_video(f16, {544, 320});
    REQUIRE_OK(fg);
    CHECK(jget<std::string>(config_of(find_stage(fg->spec, "summary")),
                            ex::kSummaryComputeDtype, "") == "f16");
    JobSpec chat = f16;
    chat.op = std::string(kOpChat);
    chat.params["text"] = "a fox";
    auto cg = vp::build_chat(chat);
    REQUIRE_OK(cg);
    CHECK(jget<std::string>(config_of(find_stage(cg->spec, "text-chat")),
                            "compute_dtype", "") == "f16");
    chat.params.erase("compute_dtype");
    cg = vp::build_chat(chat);
    REQUIRE_OK(cg);
    CHECK(!config_of(find_stage(cg->spec, "text-chat"))
               .contains("compute_dtype"));
  }
  // Its pages as the helper's chat has them: one model warm for both --
  // the model's whole context (its config's, the job's), 16 tokens a
  // page; 65,536 tokens where it says none.
  {
    JobSpec chat = job;
    chat.op = std::string(kOpChat);
    chat.params = {{"text", "a fox"}, {"context_tokens", 262144}};
    auto cg = vp::build_chat(chat);
    REQUIRE_OK(cg);
    const Json cc = config_of(find_stage(cg->spec, "text-chat"));
    CHECK(jget(cfg, ex::kSummaryPageTokens, 0) == jget(cc, "page_tokens", -1));
    CHECK(jget(cfg, ex::kSummaryMaxPages, 0) == jget(cc, "max_pages", -1));
    CHECK(jget(cc, "page_tokens", 0) * jget(cc, "max_pages", 0) == 262144);
    chat.params["context_tokens"] = 40001;
    cg = vp::build_chat(chat);
    REQUIRE_OK(cg);
    CHECK(jget(config_of(find_stage(cg->spec, "text-chat")), "max_pages",
               0) == 2501);
    chat.params.erase("context_tokens");
    cg = vp::build_chat(chat);
    REQUIRE_OK(cg);
    CHECK(jget(config_of(find_stage(cg->spec, "text-chat")), "max_pages",
               0) == 4096);
    // An agent's turn the same: its long session runs to the model's
    // limit.
    JobSpec turn = job;
    turn.op = std::string(kOpAgentTurn);
    turn.params = {{"context_tokens", 262144},
                   {"conversation",
                    {{"messages", Json::array({{{"role", "user"},
                                                {"content", "a deck"}}})}}}};
    auto tg = vp::build_agent_turn(turn);
    REQUIRE_OK(tg);
    const Json tc = config_of(find_stage(tg->spec, "text-chat"));
    CHECK(jget(tc, "page_tokens", 0) == jget(cc, "page_tokens", -1));
    CHECK(jget(tc, "max_pages", 0) == 16384);
    // No limit on what it holds unless the agent's Memory sets one; then
    // vpipe's memory_limit_mb, a summary's stage alike (DESIGN §15 0zzd).
    CHECK(!tc.contains("memory_limit_mb"));
    CHECK(!cfg.contains(ex::kSummaryMemoryLimitMb));
    turn.params["memory_limit_mb"] = 65536;
    tg = vp::build_agent_turn(turn);
    REQUIRE_OK(tg);
    CHECK(jget(config_of(find_stage(tg->spec, "text-chat")),
               "memory_limit_mb", 0) == 65536);
    chat.params["memory_limit_mb"] = 131072;
    cg = vp::build_chat(chat);
    REQUIRE_OK(cg);
    CHECK(jget(config_of(find_stage(cg->spec, "text-chat")),
               "memory_limit_mb", 0) == 131072);
    JobSpec held = job;
    held.params["memory_limit_mb"] = 196608;
    auto hg = vp::build_summarize_video(held, {544, 320});
    REQUIRE_OK(hg);
    CHECK(jget(config_of(find_stage(hg->spec, "summary")),
               ex::kSummaryMemoryLimitMb, 0) == 196608);
  }
  CHECK(g->summary_sink == "summary-sink");
  REQUIRE(find_stage(g->spec, g->summary_sink));
  CHECK(src_of(find_stage(g->spec, g->summary_sink), 0) == "summary");

  // A scene: 24 frames at most, two at least -- at eight a second, a
  // second's eight; large frames on a short context, fewer a scene (two
  // fifths of 32,768 tokens at 720 a frame: 18).
  CHECK(jget(cfg, ex::kSummarySceneFrames, 0) == 24);
  CHECK(jget(cfg, ex::kSummaryMinFrames, 0) == 2);
  {
    JobSpec dense = job;
    dense.params["every"] = 0.125;
    auto dg = vp::build_summarize_video(dense, {576, 320});
    REQUIRE_OK(dg);
    CHECK(dg->samples->every == 0.125);
    const Json dc = config_of(find_stage(dg->spec, "summary"));
    CHECK(jget(dc, ex::kSummaryMinFrames, 0) == 8);
    CHECK(jget(dc, ex::kSummarySceneFrames, 0) == 24);
    dense.params["every"] = 0.25;
    dense.params["context_tokens"] = 32768;
    dg = vp::build_summarize_video(dense, {1152, 640});
    REQUIRE_OK(dg);
    const Json hc = config_of(find_stage(dg->spec, "summary"));
    CHECK(jget(hc, ex::kSummaryMinFrames, 0) == 4);
    CHECK(jget(hc, ex::kSummarySceneFrames, 0) == 18);
    // Portrait: the size as sampled.
    dg = vp::build_summarize_video(dense, {320, 576});
    REQUIRE_OK(dg);
    CHECK(dg->samples->size.width == 320);
    CHECK(dg->samples->size.height == 576);
  }

  // Greedy with no sampler; no clip, no frame size: refused.
  job.params.erase("sampling");
  auto greedy = vp::build_summarize_video(job, {544, 320});
  REQUIRE_OK(greedy);
  CHECK(!find_stage(greedy->spec, "sampler"));
  CHECK(!vp::build_summarize_video(job, {0, 0}).ok());
  job.inputs.clear();
  CHECK(!vp::build_summarize_video(job, {544, 320}).ok());
}

TEST(graph_builder, export_graphs)
{
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpExportMedia);
  job.output_dir = "/tmp/out";
  job.params = {{"format", "png16"}};
  JobInput still;
  still.role = "source";
  still.path = "/in/photo.jpg";
  still.info.type = media::MediaType::Image;
  job.inputs = {still};
  auto g = vp::build_export(job);
  REQUIRE_OK(g);
  CHECK(g->output.extension() == ".png");
  const Json* read = find_stage(g->spec, vp::kExportRead);
  const Json* write = find_stage(g->spec, vp::kExportWrite);
  REQUIRE(read && write);
  CHECK(jget<std::string>(*read, "type", "") == "load-image");
  CHECK(jget<std::string>(config_of(read), "dtype", "") == "f16");
  CHECK(jget<std::string>(config_of(read), "alpha", "") == "drop");
  CHECK(jget<std::string>(config_of(read), "raw", "") == "rendered");
  CHECK(jget(config_of(write), "bit_depth", 0) == 16);
  CHECK(src_of(write, 1) == vp::kExportRead);   // the EXIF
  CHECK(!config_of(write).contains("quality"));

  job.params = {{"format", "jpeg"}, {"quality", 60}};
  auto j = vp::build_export(job);
  REQUIRE_OK(j);
  CHECK(j->output.extension() == ".jpg");
  const Json& jw = config_of(find_stage(j->spec, vp::kExportWrite));
  CHECK(jget<std::string>(jw, "format", "") == "jpeg");
  CHECK(jget(jw, "quality", 0) == 60);

  job.params = {{"format", "exr"}};
  auto e = vp::build_export(job);
  REQUIRE_OK(e);
  CHECK(e->output.extension() == ".exr");
  CHECK(jget<std::string>(config_of(find_stage(e->spec, vp::kExportRead)),
                          "raw", "") == "linear");
  CHECK(jget(*find_stage(e->spec, vp::kExportWrite), "iports",
             Json::array()).size() == 1);

  job.params = {{"format", "hevc10"}};
  CHECK(!vp::build_export(job).ok());  // a picture is not a video

  JobInput movie = still;
  movie.path = "/in/clip.mov";
  movie.info.type = media::MediaType::Video;
  movie.info.frame.alpha = media::AlphaMode::Straight;
  job.inputs = {movie};
  auto v = vp::build_export(job);
  REQUIRE_OK(v);
  CHECK(v->output.extension() == ".mov");
  const Json* vr = find_stage(v->spec, vp::kExportRead);
  const Json* vw = find_stage(v->spec, vp::kExportWrite);
  REQUIRE(vr && vw);
  CHECK(jget<std::string>(*vr, "type", "") == "avf-load-video");
  CHECK(jget<std::string>(config_of(vr), "alpha", "") == "keep");
  CHECK(jget<std::string>(config_of(vw), "codec", "") == "hevc");
  CHECK(!config_of(vw).contains("bitrate"));
  // Its writer's report named apart: an export runs beside a generation
  // and follows only its own (job-progress.h).
  CHECK(jget<std::string>(config_of(vw), "progress", "") ==
        vp::kExportReport);

  // Encoded as asked (engine::VideoEncoding): H.264's rate, keyframes in
  // frames at the clip's rate, no B-frames, profile, level, entropy.
  movie.info.frame_rate = {24, 1};
  job.inputs = {movie};
  VideoEncoding enc;
  enc.bitrate = 8'000'000;
  enc.max_bitrate = 12'000'000;
  enc.keyframe_seconds = 2;
  enc.b_frames = 0;
  enc.profile = "main";
  enc.level = "4.1";
  enc.entropy = "cavlc";
  CHECK(video_encoding_problem(enc, "h264").empty());
  job.params = {{"format", "h264"}, {"video", to_json(enc)}};
  auto h = vp::build_export(job);
  REQUIRE_OK(h);
  const Json& hw = config_of(find_stage(h->spec, vp::kExportWrite));
  CHECK(jget<std::string>(hw, "codec", "") == "h264");
  CHECK(jget<std::int64_t>(hw, "bitrate", 0) == 8'000'000);
  CHECK(jget<std::int64_t>(hw, "max_bitrate", 0) == 12'000'000);
  CHECK(jget(hw, "keyframe_interval", 0) == 48);
  CHECK(jget<std::string>(hw, "frame_reordering", "") == "off");
  CHECK(jget<std::string>(hw, "profile", "") == "main");
  CHECK(jget<std::string>(hw, "level", "") == "4.1");
  CHECK(jget<std::string>(hw, "entropy", "") == "cavlc");
  // A ProRes flavour, HEVC at 8 bits: the codec written.
  VideoEncoding lt;
  lt.prores = "422lt";
  CHECK(video_codec("prores422hq", lt) == "prores422lt");
  CHECK(!video_encoding_problem(lt, "prores4444").empty());
  VideoEncoding main8;
  main8.profile = "main";
  CHECK(video_codec("hevc10", main8) == "hevc8");
  // What the codec has not: refused, said why.
  VideoEncoding bad = enc;
  bad.profile = "baseline";
  bad.entropy = "cabac";
  CHECK(!video_encoding_problem(bad, "h264").empty());
  VideoEncoding rate;
  rate.bitrate = 5'000'000;
  CHECK(!video_encoding_problem(rate, "prores4444").empty());
  // Through JSON and back.
  const VideoEncoding back = video_encoding_from_json(to_json(enc));
  CHECK(back.bitrate == enc.bitrate && back.b_frames == 0 &&
        back.level == "4.1" && back.keyframe_seconds == 2);
}

// A plain reference keeps its pixels: no resample to the output size,
// only padding right and bottom to the 32-pixel grid (manual, scale 1).
// A small one lowers the tower's minimum so it is not enlarged either.
TEST(graph_builder, qwen_reference_is_padded_not_resized)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  auto g = vp::build_image_edit(
      job, {{"/in/base.png", {1024, 1024}, false, true},
            {"/in/logo.png", {200, 150}, true, false}});
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 2);
  const auto& r = g->references[1];
  CHECK(!r.base);
  CHECK(r.fitted.width == 224);
  CHECK(r.fitted.height == 160);
  CHECK(r.channels == 3);
  const Json fc = config_of(find_stage(g->spec, r.stage + "-fit"));
  CHECK(jget<std::string>(fc, "fit", "") == "manual");
  CHECK(jget(fc, "scale", 0.0) == 1.0);
  CHECK(jget(fc, "width", 0) == 224);
  CHECK(jget(fc, "height", 0) == 160);
  // Base first, then the reference, in one list.
  CHECK(list_items(g->spec) ==
        (std::vector<std::string>{"ref-0-fit", r.stage + "-fit"}));
  check_lists(g->spec, /*to_cond=*/true);
  const Json cc = config_of(config_stage(g->spec));
  CHECK(jget(cc, "vl_min_pixels", 1 << 30) <= 224 * 160);
  CHECK(jget(cc, "vl_max_pixels", 0) >= 1024 * 1024);
}

// A reference larger than the model's reference area is shrunk (same
// aspect) before padding; the vision budget follows the padded size.
TEST(graph_builder, qwen_oversized_reference_is_shrunk_then_padded)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  auto g = vp::build_image_edit(job, {{"/in/big.jpg", {4000, 3000},
                                       false, false}});
  REQUIRE_OK(g);
  const auto& r = g->references[0];
  CHECK(r.fitted.width % 32 == 0);
  CHECK(r.fitted.height % 32 == 0);
  CHECK(r.fitted.width < 4000);
  CHECK(r.fitted.width >= 1152);   // ~1 MP at 4:3, before padding
  const Json fc = config_of(find_stage(g->spec, r.stage + "-fit"));
  CHECK(jget<std::string>(fc, "fit", "") == "manual");
  CHECK(jget(fc, "scale", 1.0) < 1.0);
  const Json cc = config_of(config_stage(g->spec));
  CHECK(jget(cc, "vl_pixel_budget", 0) >=
        static_cast<std::int64_t>(r.fitted.width) * r.fitted.height);
}

// No base: a new picture composed from the references, at the model's
// own size (or the one asked for) -- not at a reference's shape.
TEST(graph_builder, qwen_compose_without_a_base)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  job.params["width"] = 1280;
  job.params["height"] = 720;
  auto g = vp::build_image_edit(job, {{"/in/a.png", {800, 800}, false, false},
                                      {"/in/b.png", {640, 480}, false, false}});
  REQUIRE_OK(g);
  CHECK(g->width == 1280);
  CHECK(g->height == 736);   // 720 on the 32-pixel grid
  REQUIRE(g->references.size() == 2);
  CHECK(!g->references[0].base);
  CHECK(g->references[0].fitted.width == 800);
  CHECK(g->references[0].fitted.height == 800);
  CHECK(g->references[1].fitted.width == 640);
}

// More than two: a base and three references, all in one list each
// side, in order, each fitted on its own.
TEST(graph_builder, qwen_takes_a_list_of_four)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  auto g = vp::build_image_edit(job, {{"/base.png", {1024, 768}, false, true},
                                      {"/a.png", {512, 512}, false, false},
                                      {"/b.png", {300, 200}, false, false},
                                      {"/c.png", {2000, 2000}, false, false}});
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 4);
  CHECK(list_items(g->spec) ==
        (std::vector<std::string>{"ref-0-fit", "ref-1-fit", "ref-2-fit",
                                  "ref-3-fit"}));
  check_lists(g->spec, /*to_cond=*/true);
  CHECK(g->references[2].fitted.width == 320);   // padded
  CHECK(g->references[3].fitted.width <= 1024);  // shrunk
  // The tower's bounds span all four.
  const Json cc = config_of(config_stage(g->spec));
  CHECK(jget(cc, "vl_min_pixels", 1 << 30) <= 320 * 224);
}

// Qwen-Image 2.1 takes ten -- tensor-list's whole width; an eleventh is
// dropped, not wired past the list's ports.
TEST(graph_builder, qwen_takes_ten_and_drops_the_eleventh)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  std::vector<vp::RefImage> pics = {{"/base.png", {1024, 1024}, false, true}};
  for (int i = 1; i < 11; ++i) {
    pics.push_back({"/r" + std::to_string(i) + ".png", {512, 512}, false,
                    false});
  }
  auto g = vp::build_image_edit(job, pics);
  REQUIRE_OK(g);
  CHECK(g->references.size() == 10);
  CHECK(list_items(g->spec).size() == 10);
  CHECK(list_items(g->spec).back() == "ref-9-fit");
  check_lists(g->spec, /*to_cond=*/true);
}

// FLUX.2-klein's text encoder does not see pictures: the list goes to
// the DiT only, fitted the same way; the catalog's four is the most it
// takes here.
TEST(graph_builder, klein_edit_skips_the_conditioner)
{
  auto job = job_for("flux2-klein-9b", kOpEditImage);
  auto g = vp::build_image_edit(job, {{"/a.png", {1024, 1024}, true, true},
                                      {"/b.png", {800, 600}, false, false},
                                      {"/c.png", {640, 480}, false, false},
                                      {"/d.png", {640, 480}, false, false},
                                      {"/e.png", {640, 480}, false, false}});
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 4);
  CHECK(g->references[0].channels == 3);
  CHECK(g->references[1].fitted.width == 800);   // 16-pixel grid: as is
  CHECK(g->references[1].fitted.height == 608);
  check_lists(g->spec, /*to_cond=*/false);
  CHECK(!find_stage(g->spec, "qwen-image-21-model-config"));
}

// FLUX.3 Image Klein (vpipe b56bc04a): its own config stage, no
// scheduler (the checkpoint states its sampler), 16 steps, up to 4096 a
// side. An edit's references are fitted as BFL's pipeline fits them --
// under a megapixel, each side cut to the grid about the centre -- and
// its base at 4 MP at most, the output's shape kept: a 4096-pixel square
// does not carry a reference as long as itself.
TEST(graph_builder, flux3_klein_makes_4k_and_fits_as_bfl)
{
  auto job = job_for("flux3-klein", kOpGenerateImage);
  REQUIRE(job.model.id == "flux3-klein");
  job.params.erase("steps");
  job.params["width"] = 4096;
  job.params["height"] = 4096;
  auto g = vp::build_text_to_image(job);
  REQUIRE_OK(g);
  CHECK(g->width == 4096 && g->height == 4096);
  CHECK(!find_stage(g->spec, "scheduler-select"));
  const Json* cfg = find_stage(g->spec, "flux3-model-config");
  REQUIRE(cfg);
  const Json* gen = find_stage(g->spec, "generate-image");
  REQUIRE(gen);
  CHECK(jget(config_of(gen), "steps", 0) == 16);
  CHECK(jget(config_of(gen), "width", 0) == 4096);
  CHECK(src_of(gen, 7) == "flux3-model-config");

  auto edit = job_for("flux3-klein", kOpEditImage);
  edit.params["width"] = 4096;
  edit.params["height"] = 2752;
  auto e = vp::build_image_edit(edit, {{"/a.jpg", {6000, 4000}, false, true},
                                       {"/b.png", {1500, 1000}, false, false},
                                       {"/c.png", {700, 500}, false, false}});
  REQUIRE_OK(e);
  REQUIRE(e->references.size() == 3);
  // The base: the output's shape at 4 MP, on the 16-pixel grid.
  const auto& base = e->references[0].fitted;
  CHECK(base.width % 16 == 0 && base.height % 16 == 0);
  CHECK(static_cast<double>(base.width) * base.height <= 4194304.0);
  CHECK(std::abs(static_cast<double>(base.width) / base.height -
                 4096.0 / 2752.0) < 0.02);
  const Json bf = config_of(find_stage(e->spec, "ref-0-fit"));
  CHECK(jget<std::string>(bf, "fit", "") == "crop");
  CHECK(jget(bf, "width", 0) == base.width);
  // A reference past a megapixel: shrunk, cut to the grid; one under:
  // only cut.
  const Json rf = config_of(find_stage(e->spec, "ref-1-fit"));
  CHECK(jget<std::string>(rf, "fit", "") == "crop_to_grid");
  CHECK(jget<std::int64_t>(rf, "max_pixels", 0) == 1048576);
  CHECK(jget(rf, "grid", 0) == 16);
  CHECK(e->references[1].fitted.width == 1248);   // 1254 cut to the grid
  CHECK(e->references[1].fitted.height == 832);   // 836
  CHECK(e->references[2].fitted.width == 688);
  CHECK(e->references[2].fitted.height == 496);
  check_lists(e->spec, /*to_cond=*/false);
  // At its own default size the base is the output's frame, as before.
  auto small = job_for("flux3-klein", kOpEditImage);
  auto s1 = vp::build_image_edit(small, {{"/a.jpg", {1200, 800}, false,
                                          true}});
  REQUIRE_OK(s1);
  CHECK(s1->references[0].fitted.width == s1->width);
  CHECK(s1->references[0].fitted.height == s1->height);
}

// FIBO 2.0 EDIT TURBO (vpipe 17c53d63): an edit only, through its config
// stage, in its own 4 steps; the pictures to the conditioner as well as
// the VAE (Qwen3-VL reads them), and each cropped to the size the model
// was trained at for its shape -- the result at the first's, as Bria's
// pipeline makes it (16:9 -> 1424 x 800, 4:3 -> 1152 x 864).
TEST(graph_builder, fibo2_edits_at_its_trained_sizes)
{
  auto job = job_for("fibo2-edit-turbo", kOpEditImage);
  REQUIRE(job.model.id == "fibo2-edit-turbo");
  job.params.erase("steps");
  job.params["width"] = 1920;
  job.params["height"] = 1080;
  auto e = vp::build_image_edit(job, {{"/a.jpg", {1920, 1080}, false, true},
                                      {"/b.png", {1600, 1200}, false, false},
                                      {"/c.png", {900, 900}, false, false}});
  REQUIRE_OK(e);
  CHECK(e->width == 1424 && e->height == 800);
  REQUIRE(e->references.size() == 3);
  CHECK(e->references[0].fitted.width == 1424);
  CHECK(e->references[0].fitted.height == 800);
  CHECK(e->references[1].fitted.width == 1152);
  CHECK(e->references[1].fitted.height == 864);
  CHECK(e->references[2].fitted.width == 1024);
  CHECK(e->references[2].fitted.height == 1024);
  for (int i = 0; i < 3; ++i) {
    const Json f = config_of(find_stage(e->spec, std::format("ref-{}-fit",
                                                             i)));
    CHECK(jget<std::string>(f, "fit", "") == "crop");
    CHECK(jget(f, "width", 0) == e->references[i].fitted.width);
    CHECK(jget(f, "height", 0) == e->references[i].fitted.height);
  }
  check_lists(e->spec, /*to_cond=*/true);
  const Json* cfg = find_stage(e->spec, "fibo2-model-config");
  REQUIRE(cfg);
  CHECK(!find_stage(e->spec, "scheduler-select"));
  const Json* gen = find_stage(e->spec, "generate-image");
  REQUIRE(gen);
  CHECK(src_of(gen, 7) == "fibo2-model-config");
  CHECK(src_of(find_stage(e->spec, "diffusion-conditioner"), 5) ==
        "fibo2-model-config");
  const Json gc = config_of(gen);
  CHECK(jget(gc, "steps", 0) == 4);
  CHECK(jget(gc, "width", 0) == 1424 && jget(gc, "height", 0) == 800);
  // A square base at the card's own size stays the model's 1024^2.
  auto sq = job_for("fibo2-edit-turbo", kOpEditImage);
  sq.params["width"] = 768;
  sq.params["height"] = 768;
  auto s1 = vp::build_image_edit(sq, {{"/a.jpg", {768, 768}, false, true}});
  REQUIRE_OK(s1);
  CHECK(s1->width == 1024 && s1->height == 1024);
}

// Qwen-Image 2.1 Turbo: the base model's graph -- its config stage, the
// conditioner reading the pictures -- with NO scheduler (its schedule is
// in its files) and its own 8 steps whatever was asked; vpipe would run
// them anyway, and warn.
TEST(graph_builder, qwen_image_turbo_runs_its_own_schedule)
{
  auto job = job_for("qwen-image-2.1-turbo", kOpGenerateImage);
  REQUIRE(job.model.id == "qwen-image-2.1-turbo");
  CHECK(jget(job.params, "steps", 0) == 20);   // asked for another count
  auto g = vp::build_text_to_image(job);
  REQUIRE_OK(g);
  CHECK(!find_stage(g->spec, "scheduler-select"));
  REQUIRE(find_stage(g->spec, "qwen-image-21-model-config"));
  const Json* gen = find_stage(g->spec, "generate-image");
  REQUIRE(gen);
  CHECK(jget(config_of(gen), "steps", 0) == 8);
  CHECK(src_of(gen, 4) == "");                  // the scheduler port
  CHECK(src_of(gen, 7) == "qwen-image-21-model-config");
  CHECK(g->width == 1024 && g->height == 1024);

  auto edit = job_for("qwen-image-2.1-turbo", kOpEditImage);
  auto e = vp::build_image_edit(edit, {{"/a.png", {1200, 800}, false, true},
                                       {"/b.png", {640, 480}, false, false}});
  REQUIRE_OK(e);
  CHECK(!find_stage(e->spec, "scheduler-select"));
  CHECK(jget(config_of(find_stage(e->spec, "generate-image")), "steps", 0) ==
        8);
  check_lists(e->spec, /*to_cond=*/true);

  // The base model still has its scheduler, at the count asked.
  auto base = vp::build_text_to_image(job_for("qwen-image-2.1",
                                              kOpGenerateImage));
  REQUIRE_OK(base);
  REQUIRE(find_stage(base->spec, "scheduler-select"));
  CHECK(jget(config_of(find_stage(base->spec, "generate-image")), "steps",
             0) == 20);
}

TEST(graph_builder, edit_needs_a_recipe_and_a_reference)
{
  CHECK(!vp::build_image_edit(job_for("qwen-image-2.1", kOpEditImage), {})
             .ok());
  // Krea-2 has no edit block in the catalog.
  CHECK(!vp::build_image_edit(job_for("krea2-turbo", kOpEditImage),
                              {{"/a.png", {512, 512}, false}}).ok());
  // Text-to-image is unchanged: no source, no encode.
  auto t2i = vp::build_text_to_image(job_for("qwen-image-2.1",
                                             kOpGenerateImage));
  REQUIRE_OK(t2i);
  CHECK(t2i->references.empty());
  CHECK(!find_stage(t2i->spec, "ref-0"));
}

// The base's pre-generation adjustments (recipe "base_adjust") ride on
// its feed, for the engine to lay on as it decodes; a reference has none.
TEST(graph_builder, base_adjustments_ride_on_the_base_feed)
{
  auto job = job_for("qwen-image-2.1", kOpEditImage);
  job.params["base_adjust"] = {{"exposure", 0.5}, {"vibrance", -0.25}};
  auto g = vp::build_image_edit(
      job, {{"/in/base.cr2", {4480, 6720}, false, true},
            {"/in/logo.png", {200, 150}, false, false}});
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 2);
  CHECK(g->references[0].base);
  CHECK(g->references[0].adjust.exposure == 0.5);
  CHECK(g->references[0].adjust.vibrance == -0.25);
  CHECK(g->references[1].adjust.identity());
}

// An image modifier goes into an export as the file is made: the picture
// is fed through a valtz-source (decoded with its adjustments, upright,
// at its own size) instead of read by load-image, its EXIF beside it;
// OpenEXR takes it linear and without EXIF.
TEST(graph_builder, an_adjusted_still_exports_through_a_source)
{
  // A real 3x2 PNG: the builder reads its upright size.
  const auto png = test::temp_dir("graph-export") / "pic.png";
  {
    std::vector<std::uint8_t> px(3 * 2 * 4, 200);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx = CGBitmapContextCreate(
        px.data(), 3, 2, 8, 12, cs,
        static_cast<std::uint32_t>(kCGImageAlphaNoneSkipLast));
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        nullptr, reinterpret_cast<const UInt8*>(png.c_str()),
        static_cast<CFIndex>(std::strlen(png.c_str())), false);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
        url, CFSTR("public.png"), 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    REQUIRE(CGImageDestinationFinalize(dst));
    CFRelease(dst);
    CFRelease(url);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
  }
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpExportMedia);
  job.output_dir = "/tmp/out";
  job.params = {{"format", "png16"}, {"adjust", {{"exposure", 0.5}}}};
  JobInput still;
  still.role = "source";
  still.path = png;
  still.info.type = media::MediaType::Image;
  job.inputs = {still};
  auto g = vp::build_export(job);
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 1);
  const auto& f = g->references[0];
  CHECK(f.stage == vp::kExportRead);
  CHECK(f.size.width == 3 && f.size.height == 2);
  CHECK(f.adjust.exposure == 0.5);

  // A crop alone goes the same way; a transparent padding keeps alpha.
  job.params = {{"format", "png16"},
                {"crop", {{"content_w", 3}, {"content_h", 2},
                          {"scale", 0.5}, {"pad_a", 0.0}}}};
  auto c = vp::build_export(job);
  REQUIRE_OK(c);
  REQUIRE(c->references.size() == 1);
  CHECK(c->references[0].crop.scale_x == 0.5);
  CHECK(c->references[0].crop.scale_y == 0.5);
  CHECK(c->references[0].channels == 4);
  CHECK(!f.linear);
  const Json* read = find_stage(g->spec, vp::kExportRead);
  const Json* write = find_stage(g->spec, vp::kExportWrite);
  REQUIRE(read && write);
  CHECK(jget<std::string>(*read, "type", "") ==
        std::string(valtz::exchange::kSourceType));
  CHECK(jget(config_of(write), "bit_depth", 0) == 16);
  CHECK(src_of(write, 1) == vp::kExifSource);   // the EXIF beside it
  CHECK(g->exif_from == png);

  job.params = {{"format", "exr"}, {"adjust", {{"exposure", 0.5}}}};
  auto e = vp::build_export(job);
  REQUIRE_OK(e);
  REQUIRE(e->references.size() == 1);
  CHECK(e->references[0].linear);
  CHECK(e->exif_from.empty());
  CHECK(jget(*find_stage(e->spec, vp::kExportWrite), "iports",
             Json::array()).size() == 1);
}

// The int8 GEMMs' tier goes on the generating stage -- generate-image,
// generate-video -- only with int8 GEMM on and only when it is not the
// accurate one vpipe runs anyway: an Accurate job's graph is as it was.
TEST(graph_builder, the_int8_tier_goes_on_its_generating_stage)
{
  auto image = [](const Json& tuning) {
    JobSpec j = job_for("qwen-image-2.1", kOpGenerateImage);
    j.params["tuning"] = tuning;
    auto g = vp::build_text_to_image(j);
    CHECK(g.ok());
    const Json* gen = g.ok() ? find_stage(g->spec, "generate-image")
                             : nullptr;
    CHECK(gen != nullptr);
    return gen ? config_of(gen) : Json::object();
  };
  Json c = image({{"i8_gemm", true}, {"i8_gemm_tier", "token"}});
  CHECK(jget(c, "i8_gemm", false));
  CHECK(jget<std::string>(c, "i8_gemm_tier", "") == "token");
  c = image({{"i8_gemm", true}, {"i8_gemm_tier", "accurate"}});
  CHECK(jget(c, "i8_gemm", false) && !c.contains("i8_gemm_tier"));
  c = image({{"i8_gemm", false}, {"i8_gemm_tier", "fast"}});
  CHECK(!c.contains("i8_gemm_tier"));
  c = image({{"i8_gemm", true}});
  CHECK(!c.contains("i8_gemm_tier"));

  JobSpec v = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  v.params = {{"prompt", "a fox in snow"}, {"width", 832}, {"height", 480},
              {"frames", 39}, {"fps", 24.0}, {"steps", 8}, {"seed", 7},
              {"tuning", {{"i8_gemm", true},
                          {"i8_gemm_tier", "token_plain"}}}};
  auto g = vp::build_video(v, std::nullopt);
  REQUIRE_OK(g);
  const Json* gen = find_stage(g->spec, "generate-video");
  REQUIRE(gen);
  CHECK(jget<std::string>(config_of(gen), "i8_gemm_tier", "") ==
        "token_plain");
}

// SageAttention's int8 P*V reaches its generating stage (generate-video,
// generate-image) only on and only with SageAttention: any other job's
// graph is as it always was.
TEST(graph_builder, sage_pv8_goes_on_generate_video_with_sage)
{
  auto gen_cfg = [](const Json& tuning) {
    JobSpec v = job_for("minimax-h3-fl2va", kOpGenerateVideo);
    v.params = {{"prompt", "a fox in snow"}, {"width", 832},
                {"height", 480}, {"frames", 39}, {"fps", 24.0},
                {"steps", 16}, {"seed", 7}, {"tuning", tuning}};
    auto g = vp::build_video(v, std::nullopt);
    CHECK(g.ok());
    const Json* gen = g.ok() ? find_stage(g->spec, "generate-video")
                             : nullptr;
    CHECK(gen != nullptr);
    return gen ? config_of(gen) : Json::object();
  };
  Json c = gen_cfg({{"sage_attn", true}, {"sage_pv8", true}});
  CHECK(jget(c, "sage_attn", false) && jget(c, "sage_pv8", false));
  c = gen_cfg({{"sage_attn", false}, {"sage_pv8", true}});
  CHECK(!c.contains("sage_pv8"));
  c = gen_cfg({{"sage_attn", true}, {"sage_pv8", false}});
  CHECK(!c.contains("sage_pv8"));
  c = gen_cfg({{"sage_attn", true}});
  CHECK(!c.contains("sage_pv8"));
  // ...the prompt's and soundtrack's rows too, only with it.
  c = gen_cfg({{"sage_attn", true}, {"sage_pv8", true},
               {"sage_pv8_all_rows", true}});
  CHECK(jget(c, "sage_pv8", false) && jget(c, "sage_pv8_all_rows", false));
  c = gen_cfg({{"sage_attn", true}, {"sage_pv8_all_rows", true}});
  CHECK(!c.contains("sage_pv8_all_rows"));

  // A picture's generate-image takes the switch the same way.
  JobSpec j = job_for("krea2-turbo", kOpGenerateImage);
  j.params["tuning"] = {{"sage_attn", true}, {"sage_pv8", true}};
  auto g = vp::build_text_to_image(j);
  REQUIRE_OK(g);
  const Json* gen = find_stage(g->spec, "generate-image");
  REQUIRE(gen);
  CHECK(jget(config_of(gen), "sage_pv8", false));
}

// MiniMax H3 text-to-video, shaped like vpipe's turbo + preview graphs:
// one config beat carries the shifts, the Turbo LoRA and the TAE (as
// FILES: both live in folders that hold others) into generate-video's
// port 9; video and sound decode apart -- the picture in F16 into the
// project's intermediate format (ProRes 422 HQ unless named), the sound
// into a WAV -- and the engine joins them. Edges go UP to the 32-pixel
// grid, as vpipe takes them.
TEST(graph_builder, minimax_h3_clip_with_turbo_and_preview)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "a fox in snow"}, {"width", 832}, {"height", 470},
              {"frames", 73}, {"fps", 24.0}, {"steps", 6}, {"seed", 7}};
  j.model.lora = "/models/turbo/adapter.safetensors";
  j.model.preview = "/models/taehv/taeh3.safetensors";
  auto g = vp::build_video(j, std::nullopt);
  REQUIRE_OK(g);
  CHECK(g->width == 832 && g->height == 480);
  CHECK(g->output.extension() == ".mov");
  CHECK(g->video_part.extension() == ".mov");
  CHECK(g->sound == media::SoundCodec::Pcm);
  CHECK(!g->video_part.empty() && !g->audio_part.empty());
  CHECK(g->preview_sink == "preview-sink");

  const Json* cfg = find_stage(g->spec, "minimax-h3-model-config");
  REQUIRE(cfg);
  const Json c = config_of(cfg);
  CHECK(!c.contains("taomate"));   // unsaid: vpipe's auto
  CHECK(jget<std::string>(c, "lora", "") == j.model.lora.string());
  CHECK(jget(c, "lora_scale", 0.0) == 1.0);
  CHECK(jget<std::string>(c, "preview_vae", "") == j.model.preview.string());
  CHECK(jget(c, "video_shift", 0.0) == 12.0);

  const Json* gen = find_stage(g->spec, "generate-video");
  REQUIRE(gen);
  CHECK(src_of(gen, 0) == "diffusion-conditioner");
  CHECK(src_of(gen, 2) == "model-select");
  CHECK(src_of(gen, 5).empty());  // no first frame
  CHECK(src_of(gen, 9) == "minimax-h3-model-config");
  const Json gc = config_of(gen);
  CHECK(jget(gc, "width", 0) == 832 && jget(gc, "height", 0) == 480);
  CHECK(jget(gc, "frames", 0) == 73 && jget(gc, "steps", 0) == 6);
  CHECK(jget(gc, "i8_gemm", false));

  const Json* prev = find_stage(g->spec, "preview-sink");
  CHECK(src_of(prev, 0) == "generate-video" && oport_of(prev, 0) == 2);
  const Json* dec = find_stage(g->spec, "vae-decode");
  CHECK(src_of(dec, 0) == "generate-video" && oport_of(dec, 0) == 0);
  CHECK(jget<std::string>(config_of(dec), "dtype", "") == "f16");
  const Json* save = find_stage(g->spec, "save-video");
  REQUIRE(save);
  CHECK(jget<std::string>(*save, "type", "") == "avf-save-video");
  CHECK(jget<std::string>(config_of(save), "codec", "") == "prores422hq");
  CHECK(!config_of(save).contains("quality"));  // ProRes has none
  CHECK(jget<std::string>(config_of(save), "path", "") ==
        g->video_part.string());
  const Json* adec = find_stage(g->spec, "audio-vae-decode");
  CHECK(src_of(adec, 0) == "generate-video" && oport_of(adec, 0) == 1);
  const Json* snd = find_stage(g->spec, "save-audio");
  CHECK(src_of(snd, 0) == "audio-vae-decode");
  CHECK(jget<std::string>(config_of(snd), "output_path", "") ==
        g->audio_part.string());

  // Without the adapter or a TAE, the config beat names neither.
  j.model.lora.clear();
  j.model.preview.clear();
  auto plain = vp::build_video(j, std::nullopt);
  REQUIRE_OK(plain);
  const Json pc = config_of(find_stage(plain->spec,
                                       "minimax-h3-model-config"));
  CHECK(!pc.contains("lora") && !pc.contains("preview_vae"));
  CHECK(plain->preview_sink.empty());
  // TaoMate's method, said either way: on with its adapter in the first
  // slot, off otherwise.
  j.params["tuning"] = {{"taomate", true}};
  auto tg = vp::build_video(j, std::nullopt);
  REQUIRE_OK(tg);
  CHECK(jget<std::string>(config_of(find_stage(tg->spec,
                                               "minimax-h3-model-config")),
                          "taomate", "") == "on");
  j.params["tuning"] = {{"taomate", false}};
  tg = vp::build_video(j, std::nullopt);
  REQUIRE_OK(tg);
  CHECK(jget<std::string>(config_of(find_stage(tg->spec,
                                               "minimax-h3-model-config")),
                          "taomate", "") == "off");
  // Its adapter as a plain LoRA: the method off, the adapter's shifts on
  // the config stage as the tuning says.
  j.params["tuning"] = {{"taomate", true}, {"taomate_lora", true},
                        {"video_shift", 12}, {"audio_shift", 3}};
  tg = vp::build_video(j, std::nullopt);
  REQUIRE_OK(tg);
  const Json lc = config_of(find_stage(tg->spec, "minimax-h3-model-config"));
  CHECK(jget<std::string>(lc, "taomate", "") == "off");
  CHECK(jget(lc, "video_shift", 0.0) == 12.0);
  CHECK(jget(lc, "audio_shift", 0.0) == 3.0);
}

// A song, in the shapes of vpipe's YuE2 graphs: the description alone
// (text to music) leaves generate-audio's prompt port unwired; lyrics go
// in through it, one beat (songs from lyrics). The decoder is the
// model's own checkpoint; the song is a WAV, its score read back.
TEST(graph_builder, yue2_text_to_music_and_song_from_lyrics)
{
  JobSpec j = job_for("yue2-3b", kOpGenerateAudio);
  j.params = {{"style", "Lo-fi hip hop, mellow piano"}, {"lyrics", ""},
              {"cot", "full"}, {"seed", 7}, {"steps", 32},
              {"tuning", {{"sol_attn", true}, {"sol_tau", 0.5},
                          {"sage_attn", true}, {"ane_qkv", false},
                          {"motion_cache", true}}}};
  j.model.vae = "/models/m-a-p/YuE2-Vae";
  auto g = vp::build_audio(j);
  REQUIRE_OK(g);
  CHECK(g->output.extension() == ".wav");
  CHECK(g->score_sink == "score-sink");
  CHECK(g->preview_sink.empty());
  const Json* gen = find_stage(g->spec, "generate-audio");
  REQUIRE(gen);
  CHECK((*gen)["iports"].empty());  // the config's one song
  CHECK(!find_stage(g->spec, "lyrics"));
  const Json gc = config_of(gen);
  CHECK(jget<std::string>(gc, "hf_dir", "") == "/models/yue2-3b");
  CHECK(jget<std::string>(gc, "style", "") == "Lo-fi hip hop, mellow piano");
  CHECK(jget<std::string>(gc, "cot", "") == "full");
  CHECK(jget(gc, "ode_steps", 0) == 32 && jget(gc, "seed", 0) == 7);
  CHECK(!gc.contains("max_seconds") && !gc.contains("abc"));
  // Only the tiers the stage has.
  CHECK(jget(gc, "sol_attn", false) && jget(gc, "sage_attn", false));
  CHECK(jget(gc, "sol_tau", 0.0) == 0.5);
  CHECK(!gc.contains("ane_qkv") && !gc.contains("motion_cache"));
  const Json* dec = find_stage(g->spec, "audio-vae-decode");
  CHECK(src_of(dec, 0) == "generate-audio" && oport_of(dec, 0) == 0);
  CHECK(jget<std::string>(config_of(dec), "hf_dir", "") ==
        "/models/m-a-p/YuE2-Vae");
  const Json* save = find_stage(g->spec, "save-audio");
  CHECK(src_of(save, 0) == "audio-vae-decode");
  CHECK(jget<std::string>(config_of(save), "format", "") == "wav");
  CHECK(jget<std::string>(config_of(save), "output_path", "") ==
        g->output.string());
  const Json* score = find_stage(g->spec, "score-sink");
  CHECK(src_of(score, 0) == "generate-audio" && oport_of(score, 0) == 1);

  // Lyrics: one beat into the prompt port; a cap and a score to follow.
  j.params["lyrics"] = "[Verse]\nStreetlights hum a quiet tune";
  j.params["max_seconds"] = 90.0;
  j.params["cot"] = "melody";
  j.params["abc"] = "X:1\nK:C\n";
  auto l = vp::build_audio(j);
  REQUIRE_OK(l);
  const Json* lyr = find_stage(l->spec, "lyrics");
  REQUIRE(lyr);
  CHECK(jget<std::string>(*lyr, "type", "") == "text-prompt");
  CHECK(jget<std::string>(config_of(lyr), "text", "") ==
        "[Verse]\nStreetlights hum a quiet tune");
  const Json* lg = find_stage(l->spec, "generate-audio");
  CHECK(src_of(lg, 0) == "lyrics");
  CHECK(jget(config_of(lg), "max_seconds", 0.0) == 90.0);
  CHECK(jget<std::string>(config_of(lg), "abc", "") == "X:1\nK:C\n");

  // A score to follow needs a plan; a song needs words and a decoder.
  j.params["cot"] = "off";
  CHECK(!vp::build_audio(j).ok());
  j.params["cot"] = "full";
  j.params["style"] = "";
  j.params["lyrics"] = "";
  CHECK(!vp::build_audio(j).ok());
  j.params["style"] = "jazz";
  j.model.vae.clear();
  CHECK(!vp::build_audio(j).ok());
}

// MiniMax-Music3, in the shape of vpipe's minimax-music3-text-to-music:
// its description the style, its lyrics the beat (always: none, no graph),
// no score planned (no cot, no abc), its own folder's vocoder, the int8
// tier its flow matching takes (accurate left out, as elsewhere).
TEST(graph_builder, minimax_music3_sings_a_description_to_lyrics)
{
  JobSpec j = job_for("minimax-music3", kOpGenerateAudio);
  const std::string caption =
      "Global Metadata\nBasic Attributes: bpm is 96. Acoustic Pop.";
  j.params = {{"style", caption},
              {"lyrics", "[verse]\nMorning light"},
              {"seed", 7}, {"steps", 30},
              {"tuning", {{"i8_gemm", true}, {"i8_gemm_tier", "token"},
                          {"sage_attn", true}}}};
  j.model.vae = j.model.dir;
  auto g = vp::build_audio(j);
  REQUIRE_OK(g);
  const Json* gen = find_stage(g->spec, "generate-audio");
  REQUIRE(gen);
  CHECK(src_of(gen, 0) == "lyrics");
  const Json gc = config_of(gen);
  CHECK(jget<std::string>(gc, "style", "") == caption);
  CHECK(!gc.contains("cot") && !gc.contains("abc"));
  CHECK(jget(gc, "ode_steps", 0) == 30);
  CHECK(jget(gc, "i8_gemm", false));
  CHECK(jget<std::string>(gc, "i8_gemm_tier", "") == "token");
  CHECK(!gc.contains("sage_attn"));  // not its stage's
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "audio-vae-decode")),
                          "hf_dir", "") ==
        jget<std::string>(gc, "hf_dir", ""));
  j.params["tuning"]["i8_gemm_tier"] = "accurate";
  auto a = vp::build_audio(j);
  REQUIRE_OK(a);
  CHECK(!config_of(find_stage(a->spec, "generate-audio"))
             .contains("i8_gemm_tier"));
  // No lyrics, no song; no score to follow.
  j.params["lyrics"] = "";
  CHECK(!vp::build_audio(j).ok());
  j.params["lyrics"] = "[instrumental]";
  CHECK(vp::build_audio(j).ok());
  j.params["abc"] = "X:1\nK:C\n";
  CHECK(!vp::build_audio(j).ok());
}

// SPEECH, in the shape of vpipe's moss-tts-v1.5-speak graph: the words a
// prompt beat, the fields the stage's config, one-shot into a WAV, MOSS's
// own sampling seeded; a voice read from its file by vpipe's stages, mono
// at the codec's rate, one beat, waited for. The codec is job.model.vae.
TEST(graph_builder, moss_tts_speaks_a_voice_from_its_file)
{
  JobSpec j = job_for("moss-tts-v1.5", kOpGenerateSpeech);
  j.params = {{"text", "Hello there. [pause 0.5s] Welcome."},
              {"instruction", "A warm narrator."}, {"quality", ""},
              {"sound_event", "Laughter"}, {"ambient_sound", ""},
              {"language", "English"}, {"duration_tokens", 50},
              {"seed", 9}};
  j.model.vae = "/models/OpenMOSS-Team/MOSS-Audio-Tokenizer";
  auto g = vp::build_speech(j);
  REQUIRE_OK(g);
  CHECK(g->output.extension() == ".wav");
  const Json* text = find_stage(g->spec, "text");
  REQUIRE(text);
  CHECK(jget<std::string>(*text, "type", "") == "text-prompt");
  CHECK(jget<std::string>(config_of(text), "text", "") ==
        "Hello there. [pause 0.5s] Welcome.");
  const Json* tts = find_stage(g->spec, "speak");
  REQUIRE(tts);
  CHECK(jget<std::string>(*tts, "type", "") == "text-to-speech");
  CHECK(src_of(tts, 0) == "text");
  CHECK(src_of(tts, 1) == "");          // no voice: the slot empty
  CHECK(src_of(tts, 2) == "sampler");
  const Json tc = config_of(tts);
  CHECK(jget<std::string>(tc, "hf_dir", "") == "/models/moss-tts-v1.5");
  CHECK(jget<std::string>(tc, "codec_dir", "") ==
        "/models/OpenMOSS-Team/MOSS-Audio-Tokenizer");
  CHECK(jget<std::string>(tc, "instruction", "") == "A warm narrator.");
  CHECK(jget<std::string>(tc, "sound_event", "") == "Laughter");
  CHECK(jget<std::string>(tc, "language", "") == "English");
  CHECK(!tc.contains("quality") && !tc.contains("ambient_sound"));
  CHECK(jget(tc, "duration_tokens", 0) == 50);
  CHECK(jget(tc, "max_new_tokens", 0) >= 75);
  CHECK(jget(tc, "stream_chunk_frames", -1) == 0);
  CHECK(!jget(tc, "interrupt_on_new_text", true));
  CHECK(!jget(tc, "wait_for_reference", true));
  CHECK(jget<std::string>(tc, "lm_quant", "") == "w8");
  CHECK(!tc.contains("i8_gemm"));
  const Json sc = config_of(find_stage(g->spec, "sampler"));
  CHECK(jget(sc, "seed", 0) == 9 && jget(sc, "top_k", 0) == 25);
  const Json* save = find_stage(g->spec, "save-audio");
  CHECK(src_of(save, 0) == "speak");
  CHECK(jget(config_of(save), "sample_rate", 0) == 24000);
  CHECK(jget<std::string>(config_of(save), "output_path", "") ==
        g->output.string());

  // A voice: read, mono at 24 kHz, one beat, into port 1 -- waited for.
  j.inputs.push_back({"voice", "/media/voice.m4a", {}, {}});
  j.params["voice_seconds"] = 30.0;  // the stage keeps 12 s of it
  j.params["duration_tokens"] = 0;
  auto v = vp::build_speech(j);
  REQUIRE_OK(v);
  const Json* read = find_stage(v->spec, "voice-read");
  REQUIRE(read);
  CHECK(jget<std::string>(config_of(read), "input_url", "") ==
        "/media/voice.m4a");
  CHECK(jget(config_of(read), "duration_s", 0.0) == 12.0);
  const Json pc = config_of(find_stage(v->spec, "voice-pcm"));
  CHECK(jget(pc, "channels", 0) == 1 &&
        jget(pc, "output_sample_rate", 0) == 24000);
  const Json* stack = find_stage(v->spec, "voice");
  CHECK(jget(config_of(stack), "group_size", -1) == 0);
  const Json* vt = find_stage(v->spec, "speak");
  CHECK(src_of(vt, 1) == "voice");
  CHECK(jget(config_of(vt), "wait_for_reference", false));
  // No length asked: the catalog's longest (300 s at 12.5 a second).
  CHECK(jget(config_of(vt), "max_new_tokens", 0) == 3750);

  // Favor: Fine holds the bf16 as published, no int8 GEMMs; Fast and
  // Med 8-bit weights and int8 GEMMs.
  j.params["tuning"] = {{"w8_weights", false}, {"i8_gemm", false}};
  const Json fine = config_of(find_stage(vp::build_speech(j)->spec,
                                         "speak"));
  CHECK(jget<std::string>(fine, "lm_quant", "") == "bf16");
  CHECK(!jget(fine, "i8_gemm", true));
  j.params["tuning"] = {{"w8_weights", true}, {"i8_gemm", true}};
  const Json fast = config_of(find_stage(vp::build_speech(j)->spec,
                                         "speak"));
  CHECK(jget<std::string>(fast, "lm_quant", "") == "w8");
  CHECK(jget(fast, "i8_gemm", false));

  // Words and a codec are needed.
  j.params["text"] = "";
  CHECK(!vp::build_speech(j).ok());
  j.params["text"] = "Hi.";
  j.model.vae.clear();
  CHECK(!vp::build_speech(j).ok());
}

// DRAMA 3, in the shape of vpipe's drama3-scene graph: the script one
// beat, its voices the stage's `references` -- each its cut file and its
// words, a speaker's tag before all but the first's -- spoken in parts,
// one-shot, at 44.1 kHz; the makers' sampling seeded; no codec of its
// own (codec.pth is beside the model).
TEST(graph_builder, drama3_performs_a_script_in_its_voices)
{
  JobSpec j = job_for("drama-3-int4", kOpGenerateSpeech);
  j.params = {{"text", "<|speaker:0|>[calm] The ship was silent.\n"
                       "<|speaker:1|>[tired] Status report."},
              {"seed", 7}};
  auto g = vp::build_speech(j);
  REQUIRE_OK(g);
  CHECK(g->output.extension() == ".wav");
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "text")), "text",
                          "") == jget<std::string>(j.params, "text", ""));
  const Json* tts = find_stage(g->spec, "speak");
  REQUIRE(tts);
  CHECK(jget<std::string>(*tts, "type", "") == "text-to-speech");
  CHECK(src_of(tts, 0) == "text");
  CHECK(src_of(tts, 1) == "");
  CHECK(src_of(tts, 2) == "sampler");
  Json tc = config_of(tts);
  CHECK(jget<std::string>(tc, "hf_dir", "") == "/models/drama-3-int4");
  CHECK(!tc.contains("codec_dir") && !tc.contains("lm_quant") &&
        !tc.contains("instruction"));
  CHECK(jget(tc, "chunk_text", false));
  CHECK(jget<std::string>(tc, "context", "") == "text");
  CHECK(jget(tc, "max_frames", 0) == 600);
  CHECK(jget(tc, "stream_chunk_frames", -1) == 0);
  CHECK(!jget(tc, "interrupt_on_new_text", true));
  CHECK(!tc.contains("references"));
  const Json sc = config_of(find_stage(g->spec, "sampler"));
  CHECK(jget(sc, "seed", 0) == 7 && jget(sc, "top_k", 0) == 30);
  CHECK(jget(sc, "temperature", 0.0) == 1.0 && jget(sc, "top_p", 0.0) == 0.9);
  const Json* save = find_stage(g->spec, "save-audio");
  CHECK(src_of(save, 0) == "speak");
  CHECK(jget(config_of(save), "sample_rate", 0) == 44100);

  // Two voices: each file with its words, the second's tagged.
  j.params["voices"] = Json::array({{{"text", "We asked over twenty "
                                              "different people."},
                                     {"seconds", 3.9}},
                                    {{"text", "Nobody saw it."},
                                     {"seconds", 13.2}}});
  auto v = vp::build_speech(j, {"/tmp/v0.wav", "/tmp/v1.wav"});
  REQUIRE_OK(v);
  tc = config_of(find_stage(v->spec, "speak"));
  const Json refs = jget(tc, "references", Json::array());
  REQUIRE(refs.size() == 2);
  CHECK(jget<std::string>(refs[0], "audio", "") == "/tmp/v0.wav");
  CHECK(jget<std::string>(refs[0], "text", "") ==
        "We asked over twenty different people.");
  CHECK(jget<std::string>(refs[1], "audio", "") == "/tmp/v1.wav");
  CHECK(jget<std::string>(refs[1], "text", "") ==
        "<|speaker:1|> Nobody saw it.");
  // The stage keeps each voice as long as its cut.
  CHECK(jget(tc, "voice_ref_seconds", 0.0) >= 13.2);
  // A voice without its file or its words: refused.
  CHECK(!vp::build_speech(j, {"/tmp/v0.wav"}).ok());
  j.params["voices"][1]["text"] = "";
  CHECK(!vp::build_speech(j, {"/tmp/v0.wav", "/tmp/v1.wav"}).ok());
  j.params["text"] = "";
  j.params.erase("voices");
  CHECK(!vp::build_speech(j).ok());
}

// A quantized variant made: vpipe's model-quantize from the source folder
// into the path the job names, its scales in BF16 -- what it runs in.
TEST(graph_builder, quantize_model_names_its_output)
{
  JobSpec j = job_for("moss-tts-v1.5-w8g64", kOpQuantizeModel);
  j.model.dir = "/models/OpenMOSS-Team/MOSS-TTS-v1.5";
  j.params = {{"output", "/models/local/.MOSS-TTS-v1.5-8bit.quantizing"},
              {"bits", 8}, {"group_size", 64}};
  auto g = vp::build_quantize_model(j);
  REQUIRE_OK(g);
  const Json qc = config_of(find_stage(g->spec, "quantize"));
  CHECK(jget<std::string>(qc, "src_model", "") ==
        "/models/OpenMOSS-Team/MOSS-TTS-v1.5");
  CHECK(jget<std::string>(qc, "output_name", "") ==
        "/models/local/.MOSS-TTS-v1.5-8bit.quantizing");
  CHECK(jget(qc, "bits", 0) == 8 && jget(qc, "group_size", 0) == 64);
  // Its scales in the dtype it runs in (vpipe's run_dtype).
  CHECK(jget<std::string>(qc, "run_dtype", "") == "bf16");
  j.params.erase("output");
  CHECK(!vp::build_quantize_model(j).ok());
}

// A COMPRESSED copy (DESIGN §8b): a model-quantize pass a part, chained
// -- the DiT from the download, the text encoder from that pass -- the
// passes before the last beside the output, the last into it; a part's
// own options laid under the job's width.
TEST(graph_builder, a_compressed_copy_is_quantized_part_by_part)
{
  JobSpec j = job_for("flux2-klein-9b", kOpQuantizeModel);
  j.model.dir = "/models/black-forest-labs/FLUX.2-klein-9B";
  const std::string out = "/models/compressed/.flux2-klein-9b-4bit.x";
  j.params = {
      {"output", out}, {"bits", 4}, {"group_size", 64},
      {"parts", Json::array(
                    {{{"target", "dit"},
                      {"options", {{"awq", true}, {"bits", 8}}}},
                     {{"target", "text_encoder"},
                      {"options", Json::object()}}})}};
  auto g = vp::build_quantize_model(j);
  REQUIRE_OK(g);
  CHECK(g->spec["stages"].size() == 2);
  const Json* q1 = find_stage(g->spec, "quantize-1");
  const Json* q2 = find_stage(g->spec, "quantize-2");
  REQUIRE(q1 && q2);
  const Json c1 = config_of(q1);
  const Json c2 = config_of(q2);
  CHECK(jget<std::string>(c1, "src_model", "") == j.model.dir.string());
  CHECK(jget<std::string>(c1, "output_name", "") == out + ".pass1");
  CHECK(jget<std::string>(c1, "target", "") == "dit");
  CHECK(jget(c1, "awq", false));
  CHECK(jget(c1, "bits", 0) == 4);  // the job's, not the part's
  CHECK(src_of(q1, 0) == "<none>");
  CHECK(jget<std::string>(c2, "src_model", "") == out + ".pass1");
  CHECK(jget<std::string>(c2, "output_name", "") == out);
  CHECK(jget<std::string>(c2, "target", "") == "text_encoder");
  CHECK(!c2.contains("awq"));
  CHECK(src_of(q2, 0) == "quantize-1");
  CHECK(!jget(c2, "skip_existing", true));
  // Every pass's scales in BF16, whatever a part's options say.
  CHECK(jget<std::string>(c1, "run_dtype", "") == "bf16");
  CHECK(jget<std::string>(c2, "run_dtype", "") == "bf16");
  j.params["parts"] = Json::array({{{"options", Json::object()}}});
  CHECK(!vp::build_quantize_model(j).ok());
}

// The assistant's chat decodes with its MTP head (vpipe's text-chat
// `mtp`): on as the catalog's assistants declare it, off when asked.
// Greedy with no sampler; with one, its config on a sampler-select stage
// feeding text-chat's sampler port -- MTP drafting under it, penalties
// included (DESIGN §15 0k).
TEST(graph_builder, chat_decodes_with_the_mtp_head)
{
  auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  for (const char* id : {"qwen3.5-9b", "qwen3.8-27b"}) {
    const auto* m = cat->find(id);
    REQUIRE(m != nullptr);
    CHECK(jget(jget(m->engine, "vpipe", Json::object()), "mtp", false));
  }
  JobSpec j = job_for("qwen3.8-27b", kOpChat);
  j.model.dir = "/models/mlx-community/Qwen3.8-27B-OptiQ-4bit";
  j.params = {{"text", "a fox"}, {"mtp", true}};
  auto g = vp::build_chat(j);
  REQUIRE_OK(g);
  const Json* chat = find_stage(g->spec, "text-chat");
  REQUIRE(chat != nullptr);
  CHECK(jget(config_of(chat), "mtp", false) == true);
  CHECK(src_of(find_stage(g->spec, "text-chat"), 0) == "text-prompt");
  CHECK(!config_of(chat).contains("mtp_model"));
  CHECK(find_stage(g->spec, "sampler") == nullptr);
  // Every catalog assistant samples as its card recommends without
  // thinking -- a Qwen as Qwen's (Ternary-Bonsai 2, a Qwen3.8, as its
  // card's Instruct mode: Qwen's), DeepSeek V4.1 as DeepSeek's -- and the
  // job carries it to the graph.
  int qwen = 0, deepseek = 0;
  for (const auto* m : cat->serving(models::Capability::PromptEnhance)) {
    if (m->role != "assistant") {
      continue;
    }
    const Json smp = jget(jget(m->engine, "vpipe", Json::object()),
                          "sampling", Json::object());
    if (m->family == "deepseek-v4.1") {
      CHECK(jget(smp, "temperature", 0.0) == 1.0);
      CHECK(jget(smp, "top_p", 0.0) == 0.95);
      CHECK(jget(smp, "top_k", -1) == 0);
      CHECK(jget(smp, "presence_penalty", -1.0) == 0.0);
      ++deepseek;
      continue;
    }
    CHECK((m->family.rfind("qwen", 0) == 0 ||
           m->family == "ternary-bonsai"));
    CHECK(jget(smp, "temperature", 0.0) == 0.7);
    CHECK(jget(smp, "top_p", 0.0) == 0.8);
    CHECK(jget(smp, "top_k", 0) == 20);
    CHECK(jget(smp, "presence_penalty", 0.0) == 1.5);
    ++qwen;
  }
  CHECK(qwen >= 4);
  CHECK(deepseek == 2);
  j.params["sampling"] = jget(jget(cat->find("qwen3.8-27b")->engine,
                                   "vpipe", Json::object()),
                              "sampling", Json::object());
  g = vp::build_chat(j);
  REQUIRE_OK(g);
  const Json* smp = find_stage(g->spec, "sampler");
  REQUIRE(smp != nullptr);
  CHECK(jget<std::string>(*smp, "type", "") == "sampler-select");
  CHECK(jget(config_of(smp), "temperature", 0.0) == 0.7);
  CHECK(jget(config_of(smp), "top_k", 0) == 20);
  CHECK(jget(config_of(smp), "presence_penalty", 0.0) == 1.5);
  CHECK(jget(config_of(smp), "seed", 1) == 0);
  CHECK(src_of(find_stage(g->spec, "text-chat"), 1) == "sampler");
  CHECK(jget(config_of(find_stage(g->spec, "text-chat")), "mtp", false));
  j.params.erase("sampling");
  // Kept loaded after the graph for as long as the job says (vpipe's
  // warm hold); none, unloaded with it.
  CHECK(jget(config_of(find_stage(g->spec, "text-chat")), "keep_loaded",
             -1.0) == 0.0);
  j.params["keep_loaded"] = 600.0;
  g = vp::build_chat(j);
  REQUIRE_OK(g);
  CHECK(jget(config_of(find_stage(g->spec, "text-chat")), "keep_loaded",
             0.0) == 600.0);
  j.params.erase("keep_loaded");
  // Its buffers wired as they load, always; a DFlash 2 drafter only when
  // the job names one, at its bits.
  CHECK(jget(config_of(find_stage(g->spec, "text-chat")), "wire_weights",
             false));
  CHECK(!config_of(find_stage(g->spec, "text-chat")).contains(
      "draft_model"));
  j.params["draft_model"] = "/models/incoai/Qwen3.8-27B-DFlash2";
  j.params["draft_bits"] = 4;
  g = vp::build_chat(j);
  REQUIRE_OK(g);
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "text-chat")),
                          "draft_model", "") ==
        "/models/incoai/Qwen3.8-27B-DFlash2");
  CHECK(jget(config_of(find_stage(g->spec, "text-chat")), "draft_bits",
             0) == 4);
  j.params.erase("draft_model");
  j.params.erase("draft_bits");
  // A long prefill's GEMMs in int8 only where the model asks (the
  // catalog's engine.vpipe.i8_prefill: Flash-Next's alone).
  CHECK(!config_of(find_stage(g->spec, "text-chat")).contains("i8_prefill"));
  j.params["i8_prefill"] = true;
  g = vp::build_chat(j);
  REQUIRE_OK(g);
  CHECK(jget(config_of(find_stage(g->spec, "text-chat")), "i8_prefill",
             false));
  j.params.erase("i8_prefill");
  {
    const auto* fn = cat->find("qwen3.8-flash-next");
    REQUIRE(fn != nullptr);
    CHECK(jget(jget(fn->engine, "vpipe", Json::object()), "i8_prefill",
               false));
    const auto* q27 = cat->find("qwen3.8-27b-4bit");
    REQUIRE(q27 != nullptr);
    CHECK(!jget(jget(q27->engine, "vpipe", Json::object()), "i8_prefill",
                false));
  }
  j.params["mtp"] = false;
  g = vp::build_chat(j);
  REQUIRE_OK(g);
  CHECK(jget(config_of(find_stage(g->spec, "text-chat")), "mtp", true) ==
        false);
  // A drafter shipped apart: its directory, for text-chat to load beside
  // the model -- as the catalog's uniform 4-bit 27B names its own.
  const auto* q4 = cat->find("qwen3.8-27b-4bit");
  REQUIRE(q4 != nullptr);
  const std::string drafter = jget<std::string>(
      jget(q4->engine, "vpipe", Json::object()), "mtp_drafter", "");
  CHECK(drafter == "qwen3.8-27b-mtp-4bit");
  CHECK(cat->find(drafter) != nullptr &&
        cat->find(drafter)->role == "drafter");
  j.params = {{"text", "a fox"}, {"mtp", true},
              {"mtp_model", "/models/mlx-community/Qwen3.8-27B-MTP-4bit"}};
  g = vp::build_chat(j);
  REQUIRE_OK(g);
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "text-chat")),
                          "mtp_model", "") ==
        "/models/mlx-community/Qwen3.8-27B-MTP-4bit");
}

// Ref2VA: the prompt and the references through vpipe's
// video-ref-encoder -- pictures and whole sounds by their files, a clip's
// span (the tail of one continued) cut by vpipe's own stages into a port,
// its sound attached to it -- and, keeping the song, no soundtrack
// decoded: the song is joined over the clip's length.
TEST(graph_builder, minimax_h3_ref2va_references)
{
  JobSpec j = job_for("minimax-h3-ref2va", kOpGenerateVideo);
  j.params = {{"prompt", "<Picture 1> sings <Audio 1>"}, {"width", 832},
              {"height", 480}, {"frames", 56}, {"fps", 24.0}, {"steps", 8},
              {"seed", 7}, {"reference_sound", true}};
  std::vector<vp::RefMedia> refs(3);
  refs[0].path = "/p/cat.png";
  refs[0].kind = "image";
  refs[1].path = "/p/song.wav";
  refs[1].kind = "audio";
  refs[1].start_s = 12.5;
  refs[2].path = "/p/clip.mp4";
  refs[2].kind = "video";
  refs[2].list = false;
  refs[2].audio = true;
  refs[2].width = 832;
  refs[2].height = 480;
  refs[2].first = 34;
  refs[2].count = 90;
  refs[2].start_s = 34 / 24.0;
  refs[2].duration_s = 90 / 24.0;
  auto g = vp::build_video(j, std::nullopt, refs);
  REQUIRE_OK(g);
  CHECK(!find_stage(g->spec, "diffusion-conditioner"));
  const Json* enc = find_stage(g->spec, "video-ref-encoder");
  REQUIRE(enc);
  CHECK(src_of(enc, 0) == "text-prompt" && src_of(enc, 1) == "model-select");
  CHECK(src_of(enc, 2) == "ref1" && src_of(enc, 3) == "ref2");
  const Json ec = config_of(enc);
  CHECK(jget(ec, "frames", 0) == 56);
  CHECK((ec["references"] ==
         Json::array({"/p/cat.png", "/p/song.wav"})));
  CHECK((ec["attach_audio"] == Json::array({2})));
  CHECK(jget(ec, "reference_image_short_edge", 0) == 1024);
  // The clip's span, to the frame; its sound the same span.
  const Json* span = find_stage(g->spec, "ref1-span");
  REQUIRE(span);
  CHECK(jget<std::string>(*span, "type", "") == "temporal-slice");
  CHECK(jget(config_of(span), "start", 0) == 34 &&
        jget(config_of(span), "end", 0) == 124);
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "ref1-read")),
                          "input_url", "") == "/p/clip.mp4");
  const Json ar = config_of(find_stage(g->spec, "ref2-read"));
  CHECK(std::abs(jget(ar, "start_s", 0.0) - 34 / 24.0) < 1e-9);
  CHECK(std::abs(jget(ar, "duration_s", 0.0) - 90 / 24.0) < 1e-9);
  CHECK(jget(config_of(find_stage(g->spec, "ref2-pcm")),
             "output_sample_rate", 0) == 32000);
  const Json* gen = find_stage(g->spec, "generate-video");
  REQUIRE(gen);
  CHECK(src_of(gen, 0) == "video-ref-encoder" && oport_of(gen, 0) == 0);
  CHECK(src_of(gen, 7) == "video-ref-encoder" && oport_of(gen, 7) == 1);
  CHECK(src_of(gen, 8) == "video-ref-encoder" && oport_of(gen, 8) == 2);
  CHECK(src_of(gen, 5).empty());
  // The song kept: none decoded, the song joined from 12.5 s for the
  // clip's 56 frames.
  CHECK(!find_stage(g->spec, "audio-vae-decode"));
  CHECK(g->audio_part == "/p/song.wav" && g->audio_is_source);
  CHECK(g->audio_start == 12.5);
  CHECK(std::abs(g->audio_duration - 56 / 24.0) < 1e-9);

  // Every clip and sound cut to the clip's length by the encoder unless
  // kept WHOLE: those by their number in the order it reads them (the
  // list, then the ports), the key kept apart.
  CHECK(!ec.contains("whole_references"));
  refs[1].whole = true;
  refs[2].whole = true;
  auto kept = vp::build_video(j, std::nullopt, refs);
  REQUIRE_OK(kept);
  CHECK((config_of(find_stage(kept->spec, "video-ref-encoder"))
             ["whole_references"] == Json::array({2, 3})));
  refs[1].whole = false;
  refs[2].whole = false;

  // Its own soundtrack otherwise; references and a first frame never.
  j.params.erase("reference_sound");
  auto own = vp::build_video(j, std::nullopt, refs);
  REQUIRE_OK(own);
  CHECK(find_stage(own->spec, "audio-vae-decode"));
  CHECK(!own->audio_is_source);
  CHECK(!vp::build_video(j, vp::RefImage{"/p/first.png", {832, 480}},
                         refs).ok());
  // FL2VA reads no references.
  JobSpec f = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  CHECK(!vp::build_video(f, std::nullopt, refs).ok());
}

// Ref2VA from the prompt alone (catalog `references.prompt_only`):
// vpipe's prompt-only request -- the reference encoder with an EMPTY
// list, never the diffusion-conditioner, whose conditioning the Ref2VA
// weights refuse; no picture to open on. FL2VA from words as before.
TEST(graph_builder, minimax_h3_ref2va_from_the_prompt_alone)
{
  JobSpec j = job_for("minimax-h3-ref2va", kOpGenerateVideo);
  j.params = {{"prompt", "A fox trots through snow at dawn"},
              {"width", 832}, {"height", 480}, {"frames", 39},
              {"fps", 24.0}, {"steps", 8}, {"seed", 7}};
  auto g = vp::build_video(j, std::nullopt, {});
  REQUIRE_OK(g);
  CHECK(!find_stage(g->spec, "diffusion-conditioner"));
  const Json* enc = find_stage(g->spec, "video-ref-encoder");
  REQUIRE(enc);
  CHECK(src_of(enc, 0) == "text-prompt" && src_of(enc, 1) == "model-select");
  CHECK(src_of(enc, 2) == "<none>");  // no reference port
  const Json ec = config_of(enc);
  CHECK(ec.contains("references") && ec["references"] == Json::array());
  CHECK(jget(ec, "frames", 0) == 39);
  const Json* gen = find_stage(g->spec, "generate-video");
  REQUIRE(gen);
  CHECK(src_of(gen, 0) == "video-ref-encoder" && oport_of(gen, 0) == 0);
  CHECK(src_of(gen, 7) == "video-ref-encoder" && oport_of(gen, 7) == 1);
  CHECK(src_of(gen, 5).empty());
  CHECK(find_stage(g->spec, "audio-vae-decode"));
  CHECK(!vp::build_video(j, vp::RefImage{"/p/first.png", {832, 480}}, {})
             .ok());

  JobSpec f = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  f.params = j.params;
  auto fg = vp::build_video(f, std::nullopt, {});
  REQUIRE_OK(fg);
  CHECK(find_stage(fg->spec, "diffusion-conditioner"));
  CHECK(!find_stage(fg->spec, "video-ref-encoder"));
}

// A continuation's seam: its decoded frames matched to the tail it was
// given -- content-aware-continuation between the decode and the writer,
// reading the same stacked tail the encoder does, its config the
// catalog's. A clip only referred to (not continued) is left alone.
TEST(graph_builder, a_continuation_is_matched_at_its_seam)
{
  JobSpec j = job_for("minimax-h3-ref2va", kOpGenerateVideo);
  j.params = {{"prompt", "[video continuation + audio reference] ..."},
              {"width", 832}, {"height", 480}, {"frames", 124},
              {"fps", 24.0}, {"steps", 8}, {"seed", 7}};
  std::vector<vp::RefMedia> refs(1);
  refs[0].path = "/p/guide.mov";
  refs[0].kind = "video";
  refs[0].list = false;
  refs[0].audio = true;
  refs[0].continues = true;
  refs[0].width = 832;
  refs[0].height = 480;
  refs[0].count = 56;
  auto g = vp::build_video(j, std::nullopt, refs);
  REQUIRE_OK(g);
  const Json* seam = find_stage(g->spec, "seam");
  REQUIRE(seam);
  CHECK(jget<std::string>(*seam, "type", "") ==
        "content-aware-continuation");
  CHECK(src_of(seam, 0) == "vae-decode" && src_of(seam, 1) == "ref1");
  CHECK(jget(config_of(seam), "window", 0) == 4);
  // Its tone only: across a moving shot's seam the shift it measures is
  // the camera's.
  CHECK(jget(config_of(seam), "apply_geometry", true) == false);
  CHECK(src_of(find_stage(g->spec, "video-ref-encoder"), 2) == "ref1");
  CHECK(src_of(find_stage(g->spec, "save-video"), 0) == "seam");

  refs[0].continues = false;
  auto ref = vp::build_video(j, std::nullopt, refs);
  REQUIRE_OK(ref);
  CHECK(!find_stage(ref->spec, "seam"));
  CHECK(src_of(find_stage(ref->spec, "save-video"), 0) == "vae-decode");
  JobSpec f = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  f.params = {{"prompt", "a fox"}, {"width", 832}, {"height", 480},
              {"frames", 39}};
  auto words = vp::build_video(f, std::nullopt);
  REQUIRE_OK(words);
  CHECK(!find_stage(words->spec, "seam"));
}

// FL2VA from a picture: the picture is fed at its own size, cropped with
// Lanczos to EXACTLY the clip's frame (vpipe drops an anchor of another
// size and the clip quietly becomes text-to-video), VAE-encoded and
// wired to generate-video's first-keyframe port.
TEST(graph_builder, minimax_h3_clip_opens_on_a_picture)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "the fox runs off"}, {"width", 960},
              {"height", 544}, {"frames", 124}, {"steps", 6}};
  vp::RefImage first{"/tmp/fox.png", {1536, 1024}, false, true};
  auto g = vp::build_video(j, first);
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 1);
  CHECK(g->references[0].stage == "first");
  CHECK(g->references[0].size.width == 1536);
  CHECK(g->references[0].fitted.width == 960 &&
        g->references[0].fitted.height == 544);
  const Json* fit = find_stage(g->spec, "first-fit");
  REQUIRE(fit);
  CHECK(src_of(fit, 0) == "first");
  CHECK(jget<std::string>(config_of(fit), "fit", "") == "crop");
  CHECK(jget(config_of(fit), "width", 0) == 960);
  const Json* enc = find_stage(g->spec, "first-latent");
  REQUIRE(enc);
  CHECK(jget<std::string>(*enc, "type", "") == "vae-encode");
  CHECK(src_of(enc, 0) == "first-fit" && src_of(enc, 1) == "model-select");
  CHECK(src_of(find_stage(g->spec, "generate-video"), 5) == "first-latent");

  // A family that takes no first frame refuses one.
  JobSpec k = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  k.params = j.params;
  k.model.engine["vpipe"]["first_frame"] = false;
  CHECK(!vp::build_video(k, first).ok());
}

// FL2VA's two keyframes: the picture it ends on fitted as the first is
// -- at its own size, cropped to the frame -- but as it looks (the
// request's look is the first's), VAE-encoded into generate-video's
// LAST-keyframe port, 6. Never without a first, nor on a family that
// ends on none.
TEST(graph_builder, minimax_h3_clip_ends_on_a_picture)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "she opens the umbrella"}, {"width", 832},
              {"height", 480}, {"frames", 124}, {"steps", 8},
              {"base_adjust", {{"exposure", 0.5}}}};
  vp::RefImage first{"/tmp/closed.png", {1536, 1024}, false, true};
  vp::RefImage last{"/tmp/open.png", {1024, 1536}, false, true};
  auto g = vp::build_video(j, first, {}, last);
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 2);
  CHECK(g->references[1].stage == "last");
  CHECK(g->references[1].size.width == 1024 &&
        g->references[1].size.height == 1536);
  CHECK(g->references[1].fitted.width == 832 &&
        g->references[1].fitted.height == 480);
  CHECK(g->references[0].adjust.exposure == 0.5);
  CHECK(g->references[1].adjust.exposure == 0.0);
  const Json* fit = find_stage(g->spec, "last-fit");
  REQUIRE(fit);
  CHECK(src_of(fit, 0) == "last");
  CHECK(jget<std::string>(config_of(fit), "fit", "") == "crop");
  const Json* enc = find_stage(g->spec, "last-latent");
  REQUIRE(enc);
  CHECK(src_of(enc, 0) == "last-fit" && src_of(enc, 1) == "model-select");
  const Json* gen = find_stage(g->spec, "generate-video");
  CHECK(src_of(gen, 5) == "first-latent");
  CHECK(src_of(gen, 6) == "last-latent");

  CHECK(!vp::build_video(j, std::nullopt, {}, last).ok());
  JobSpec k = j;
  k.model.engine["vpipe"]["last_frame"] = false;
  CHECK(!vp::build_video(k, first, {}, last).ok());
  // One picture: nothing on port 6.
  auto one = vp::build_video(j, first);
  REQUIRE_OK(one);
  CHECK(src_of(find_stage(one->spec, "generate-video"), 6).empty());
}

// Past 768P a clip is DRAFTED at 768 on its short side and REGENERATED
// at the size asked (Open-H3-Regenerate-2K, DESIGN §4q): the draft's
// latent itself -- never decoded -- enlarged by the latent upscaler and
// refined in three steps under the merged LoRA; the draft's soundtrack.
TEST(graph_builder, a_clip_past_768p_is_drafted_and_regenerated)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "a lighthouse at dusk"}, {"width", 2560},
              {"height", 1440}, {"frames", 39}, {"steps", 8},
              {"seed", 7},
              {"draft", {{"width", 1376}, {"height", 768}}},
              {"regenerate", "open-h3-regenerate-2k"},
              {"regenerate_tuning",
               {{"i8_gemm", true}, {"i8_gemm_tier", "token"}}},
              {"regenerate_spec", {{"steps", 3}}}};
  j.model.regenerate_lora = "/m/regen/lora/open_h3_regenerate_2k_lora.st";
  j.model.latent_upscaler = "/m/regen/latent_upsampler/up.safetensors";
  vp::RefImage first{"/tmp/first.png", {1920, 1080}, false, true};
  auto g = vp::build_video(j, first);
  REQUIRE_OK(g);
  CHECK(g->width == 2560 && g->height == 1440);
  // The draft, and the picture it opens on, at 768P.
  const Json* gen = find_stage(g->spec, "generate-video");
  REQUIRE(gen);
  CHECK(jget(config_of(gen), "width", 0) == 1376);
  CHECK(jget(config_of(gen), "height", 0) == 768);
  REQUIRE(g->references.size() == 1);
  CHECK(g->references[0].fitted.width == 1376 &&
        g->references[0].fitted.height == 768);
  // Its latent straight into the upscaler, at the size asked.
  const Json* up = find_stage(g->spec, "latent-upscale");
  REQUIRE(up);
  CHECK(src_of(up, 0) == "generate-video" && oport_of(up, 0) == 0);
  CHECK(jget<std::string>(config_of(up), "hf_dir", "") ==
        j.model.latent_upscaler.string());
  CHECK(jget(config_of(up), "width", 0) == 2560);
  CHECK(jget(config_of(up), "height", 0) == 1440);
  CHECK(jget(config_of(up), "frames", 0) == 39);
  CHECK(jget<std::string>(config_of(up), "i8_gemm_tier", "") == "token");
  // The refine: the same words and model, the merged LoRA alone on its
  // config, the enlarged latent on init_latent, three steps.
  const Json* cfg = find_stage(g->spec, "regenerate-config");
  REQUIRE(cfg);
  CHECK(jget<std::string>(config_of(cfg), "lora", "") ==
        j.model.regenerate_lora.string());
  CHECK(jget(config_of(cfg), "lora_scale", 0.0) == 1.0);
  CHECK(!config_of(cfg).contains("preview_vae"));
  const Json* ref = find_stage(g->spec, "regenerate");
  REQUIRE(ref);
  CHECK(jget<std::string>(*ref, "type", "") == "generate-video");
  REQUIRE((*ref)["iports"].size() == 12);
  CHECK(src_of(ref, 0) == src_of(gen, 0));
  CHECK(src_of(ref, 2) == "model-select");
  CHECK(src_of(ref, 9) == "regenerate-config");
  CHECK(src_of(ref, 11) == "latent-upscale");
  CHECK(src_of(ref, 5).empty() && src_of(ref, 6).empty());
  CHECK(jget(config_of(ref), "width", 0) == 2560);
  CHECK(jget(config_of(ref), "steps", 0) == 3);
  CHECK(jget(config_of(ref), "seed", 0) == 7);
  CHECK(jget<std::string>(config_of(ref), "i8_gemm_tier", "") == "token");
  // Decoded from the refine, the released decoder; the draft never.
  CHECK(!find_stage(g->spec, "vae-decode"));
  const Json* dec = find_stage(g->spec, "regenerate-decode");
  REQUIRE(dec);
  CHECK(src_of(dec, 0) == "regenerate");
  CHECK(!config_of(dec).contains("fast_decoder"));
  CHECK(src_of(find_stage(g->spec, "save-video"), 0) ==
        "regenerate-decode");
  CHECK(src_of(find_stage(g->spec, "audio-vae-decode"), 0) ==
        "generate-video");
  CHECK(oport_of(find_stage(g->spec, "audio-vae-decode"), 0) == 1);
  // The draft decoded beside the refine for the stage alone -- 8-bit,
  // its decoder let go once done -- stacked into one clip for its sink;
  // small enough to hold (39 frames at 1376 x 768: as it is).
  const Json* ddec = find_stage(g->spec, "draft-decode");
  REQUIRE(ddec);
  CHECK(jget<std::string>(*ddec, "type", "") == "vae-decode");
  CHECK(src_of(ddec, 0) == "generate-video" && oport_of(ddec, 0) == 0);
  CHECK(src_of(ddec, 1) == "model-select");
  CHECK(!config_of(ddec).contains("dtype"));
  CHECK(jget<std::string>(config_of(ddec), "unload_when_idle", "") ==
        "destroy");
  CHECK(!find_stage(g->spec, "draft-fit"));
  const Json* stack = find_stage(g->spec, "draft-stack");
  REQUIRE(stack);
  CHECK(src_of(stack, 0) == "draft-decode");
  CHECK(jget<std::string>(config_of(stack), "mode", "") == "video");
  CHECK(jget(config_of(stack), "group_size", -1) == 0);
  CHECK(g->draft_sink == "draft-sink");
  CHECK(src_of(find_stage(g->spec, "draft-sink"), 0) == "draft-stack");
  // A long clip's draft made smaller: 124 frames at 1376 x 768 are 393
  // MB of frames, 256 at most.
  JobSpec longer = j;
  longer.params["frames"] = 124;
  auto lg = vp::build_video(longer, first);
  REQUIRE_OK(lg);
  const Json* fit = find_stage(lg->spec, "draft-fit");
  REQUIRE(fit);
  CHECK(src_of(fit, 0) == "draft-decode");
  CHECK(src_of(find_stage(lg->spec, "draft-stack"), 0) == "draft-fit");
  const int fw = jget(config_of(fit), "width", 0);
  const int fh = jget(config_of(fit), "height", 0);
  CHECK(fw == 1136 && fh == 634);
  CHECK(3.0 * fw * fh * 124 <= 256.0 * 1024 * 1024);
  // No previews, no draft decoded.
  JobSpec quiet = j;
  quiet.params["preview"] = false;
  auto q = vp::build_video(quiet, first);
  REQUIRE_OK(q);
  CHECK(q->draft_sink.empty() && !find_stage(q->spec, "draft-decode"));
  // The fast decoder, with the refine's tier -- the draft's too, at its
  // own accuracy.
  JobSpec fast = j;
  fast.model.fast_decoder = "/m/regen/vae_fast/fast.safetensors";
  auto f = vp::build_video(fast, first);
  REQUIRE_OK(f);
  const Json* fdec = find_stage(f->spec, "regenerate-decode");
  CHECK(jget<std::string>(config_of(fdec), "fast_decoder", "") ==
        fast.model.fast_decoder.string());
  CHECK(jget<std::string>(config_of(fdec), "i8_gemm_tier", "") == "token");
  const Json dfast = config_of(find_stage(f->spec, "draft-decode"));
  CHECK(jget<std::string>(dfast, "fast_decoder", "") ==
        fast.model.fast_decoder.string());
  CHECK(!dfast.contains("i8_gemm_tier"));
  // A VAE of Custom's: one source, read by both decoders.
  JobSpec own = j;
  own.model.vae = "/m/my-vae";
  auto o = vp::build_video(own, first);
  REQUIRE_OK(o);
  int selects = 0;
  for (const Json& st : o->spec["stages"]) {
    selects += jget<std::string>(st, "id", "") == "vae-select";
  }
  CHECK(selects == 1);
  CHECK(src_of(find_stage(o->spec, "draft-decode"), 1) == "vae-select");
  CHECK(src_of(find_stage(o->spec, "regenerate-decode"), 1) ==
        "vae-select");
  // Sol, Sage and its int8 P·V on the refine alone (vpipe's banded
  // forward takes them): the draft runs its own Favor. Sol's exact
  // flavour and a threshold a step (Med's refine): vpipe's sol_threshold
  // and sol_tau_steps, its first entry the sol_tau.
  JobSpec attn = j;
  attn.params["regenerate_tuning"] = {{"sol_attn", true},
                                      {"sol_threshold", "exact"},
                                      {"sol_tau", "1,1.5,1.75"},
                                      {"sage_attn", true},
                                      {"sage_pv8", true}};
  auto a = vp::build_video(attn, first);
  REQUIRE_OK(a);
  const Json rc = config_of(find_stage(a->spec, "regenerate"));
  CHECK(jget(rc, "sol_attn", false) && jget(rc, "sage_attn", false));
  CHECK(jget(rc, "sage_pv8", false));
  CHECK(jget(rc, "sol_tau", 0.0) == 1.0);
  CHECK(jget<std::string>(rc, "sol_tau_steps", "") == "1,1.5,1.75");
  CHECK(jget<std::string>(rc, "sol_threshold", "") == "exact");
  const Json dc = config_of(find_stage(a->spec, "generate-video"));
  CHECK(!dc.contains("sage_pv8"));
  CHECK(!dc.contains("sol_threshold") && !dc.contains("sol_tau_steps"));
  // One threshold, diag: the keys vpipe had before them, alone.
  attn.params["regenerate_tuning"] = {{"sol_attn", true},
                                      {"sol_threshold", "diag"},
                                      {"sol_tau", 1.75}};
  auto d = vp::build_video(attn, first);
  REQUIRE_OK(d);
  const Json rd = config_of(find_stage(d->spec, "regenerate"));
  CHECK(jget(rd, "sol_tau", 0.0) == 1.75);
  CHECK(!rd.contains("sol_tau_steps") && !rd.contains("sol_threshold"));
  // Exact with Sol off: nothing of it.
  attn.params["regenerate_tuning"] = {{"sol_attn", false},
                                      {"sol_threshold", "exact"}};
  auto off = vp::build_video(attn, first);
  REQUIRE_OK(off);
  CHECK(!config_of(find_stage(off->spec, "regenerate"))
             .contains("sol_threshold"));
  // A draft without the refiner's files is refused; none asked, the clip
  // is made at its size as it always was.
  JobSpec bare = j;
  bare.model.regenerate_lora.clear();
  CHECK(!vp::build_video(bare, first).ok());
  JobSpec plain = j;
  plain.params.erase("draft");
  auto p = vp::build_video(plain, first);
  REQUIRE_OK(p);
  CHECK(!find_stage(p->spec, "latent-upscale"));
  CHECK(jget(config_of(find_stage(p->spec, "generate-video")), "width",
             0) == 2560);
  CHECK(src_of(find_stage(p->spec, "vae-decode"), 0) == "generate-video");
}

// A Ref2VA clip past 768P (DESIGN §4q): drafted by Ref2VA from its
// references at 768P, its latent enlarged and refined on FL2VA -- the
// refiner's base -- against words of its own, read by FL2VA's prompt
// encoder: FL2VA's model-select, DiT and conditioner, Ref2VA's VAE. An
// FL2VA clip given words for its refine reads them through its own
// encoder; a continuation is not regenerated.
TEST(graph_builder, a_ref2va_clip_is_drafted_and_refined_on_fl2va)
{
  static auto cat = models::Catalog::builtin();
  REQUIRE_OK(cat);
  JobSpec j = job_for("minimax-h3-ref2va", kOpGenerateVideo);
  j.params = {{"prompt", "<Picture 1> walks along the shore"},
              {"width", 2560}, {"height", 1440}, {"frames", 39},
              {"fps", 24.0}, {"steps", 8}, {"seed", 7},
              {"draft", {{"width", 1376}, {"height", 768}}},
              {"regenerate", "open-h3-regenerate-2k"},
              {"regenerate_prompt", "a woman in a red coat walks along "
                                    "the shore at dusk"},
              {"regenerate_tuning", {{"sol_attn", true}}},
              {"regenerate_spec", {{"steps", 3},
                                   {"base", "minimax-h3-fl2va"}}}};
  j.model.dit = "/m/ref2va-community-dit";
  j.model.encoder_weights = "/m/ref2va-encoder.ckpt";
  j.model.regenerate_lora = "/m/regen/lora.safetensors";
  j.model.latent_upscaler = "/m/regen/up.safetensors";
  j.model.regenerate_base = "/models/minimax-h3-fl2va";
  j.model.regenerate_base_engine = cat->find("minimax-h3-fl2va")->engine;
  std::vector<vp::RefMedia> refs(1);
  refs[0].path = "/p/woman.png";
  refs[0].kind = "image";
  auto g = vp::build_video(j, std::nullopt, refs);
  REQUIRE_OK(g);
  // The draft: Ref2VA's, from its references, at 768P.
  const Json* gen = find_stage(g->spec, "generate-video");
  REQUIRE(gen);
  CHECK(src_of(gen, 0) == "video-ref-encoder");
  CHECK(src_of(gen, 2) == "model-select");
  CHECK(jget(config_of(gen), "width", 0) == 1376);
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "model-select")),
                          "hf_dir", "") == "/models/minimax-h3-ref2va");
  // The refine: FL2VA's model, its own words through FL2VA's encoder.
  const Json* bm = find_stage(g->spec, "regenerate-model-select");
  REQUIRE(bm);
  CHECK(jget<std::string>(config_of(bm), "hf_dir", "") ==
        "/models/minimax-h3-fl2va");
  const Json* words = find_stage(g->spec, "regenerate-prompt");
  REQUIRE(words);
  CHECK(jget<std::string>(config_of(words), "text", "") ==
        "a woman in a red coat walks along the shore at dusk");
  const Json* cond = find_stage(g->spec, "regenerate-conditioner");
  REQUIRE(cond);
  CHECK(jget<std::string>(*cond, "type", "") == "diffusion-conditioner");
  CHECK(src_of(cond, 0) == "regenerate-prompt");
  CHECK(src_of(cond, 2) == "regenerate-model-select");
  CHECK(!config_of(cond).contains("encoder_dir"));
  const Json* ref = find_stage(g->spec, "regenerate");
  REQUIRE(ref);
  CHECK(src_of(ref, 0) == "regenerate-conditioner");
  CHECK(src_of(ref, 2) == "regenerate-model-select");
  CHECK(src_of(ref, 11) == "latent-upscale");
  CHECK(!config_of(ref).contains("dit_dir"));  // FL2VA's own
  CHECK(jget(config_of(ref), "sol_attn", false));
  CHECK(src_of(find_stage(g->spec, "latent-upscale"), 0) == "generate-video");
  // Decoded by the job's own VAE (one codec), the draft shown too.
  const Json* dec = find_stage(g->spec, "regenerate-decode");
  REQUIRE(dec);
  CHECK(src_of(dec, 1) == "model-select");
  CHECK(find_stage(g->spec, "draft-decode"));
  // FL2VA's DiT and encoder linked apart: the refine's.
  j.model.regenerate_base_dit = "/m/fl2va-dt.ckpt";
  j.model.regenerate_base_encoder = "/m/fl2va-encoder.ckpt";
  auto linked = vp::build_video(j, std::nullopt, refs);
  REQUIRE_OK(linked);
  CHECK(jget<std::string>(config_of(find_stage(linked->spec, "regenerate")),
                          "dit_dir", "") == "/m/fl2va-dt.ckpt");
  CHECK(jget<std::string>(
            config_of(find_stage(linked->spec, "regenerate-conditioner")),
            "encoder_dir", "") == "/m/fl2va-encoder.ckpt");
  // A continuation: refused.
  refs[0].kind = "video";
  refs[0].list = false;
  refs[0].continues = true;
  CHECK(!vp::build_video(j, std::nullopt, refs).ok());
  // FL2VA given words for its refine: its own encoder reads them, on its
  // own model-select; none given, the draft's conditioning as before.
  JobSpec f = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  f.params = {{"prompt", "a lighthouse at dusk"}, {"width", 2560},
              {"height", 1440}, {"frames", 39}, {"seed", 7},
              {"draft", {{"width", 1376}, {"height", 768}}},
              {"regenerate_prompt", "a white lighthouse, sharp detail"},
              {"regenerate_spec", {{"steps", 3},
                                   {"base", "minimax-h3-fl2va"}}}};
  f.model.regenerate_lora = "/m/regen/lora.safetensors";
  f.model.latent_upscaler = "/m/regen/up.safetensors";
  auto fg = vp::build_video(f, std::nullopt);
  REQUIRE_OK(fg);
  CHECK(!find_stage(fg->spec, "regenerate-model-select"));
  const Json* fc = find_stage(fg->spec, "regenerate-conditioner");
  REQUIRE(fc);
  CHECK(src_of(fc, 2) == "model-select");
  CHECK(src_of(find_stage(fg->spec, "regenerate"), 0) ==
        "regenerate-conditioner");
  f.params.erase("regenerate_prompt");
  auto plain = vp::build_video(f, std::nullopt);
  REQUIRE_OK(plain);
  CHECK(!find_stage(plain->spec, "regenerate-conditioner"));
  CHECK(src_of(find_stage(plain->spec, "regenerate"), 0) ==
        "diffusion-conditioner");
}

// A clip REGENERATED as an upscale (Render Upscaled by Open-H3-Regenerate-
// 2K, DESIGN §4q): read whole at 768P at most, encoded with its posterior
// sampled, enlarged, refined against its words, cut at its frames, its
// own sound joined after.
TEST(graph_builder, a_clip_is_regenerated_as_an_upscale)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpUpscaleVideo);
  j.model.regenerate_lora = "/m/regen/lora.safetensors";
  j.model.latent_upscaler = "/m/regen/up.safetensors";
  j.params = {{"width", 1920}, {"height", 1080},
              {"encode", {{"width", 1376}, {"height", 768}}},
              {"first", 0}, {"count", 121}, {"seed", 3},
              {"prompt", "a fox in snow"},
              {"regenerate_spec", {{"steps", 3}, {"posterior_seed", 42}}},
              {"tuning", {{"ane_ffn", true}}}};
  JobInput in;
  in.role = "source";
  in.path = "/tmp/clip.mov";
  in.info.type = media::MediaType::Video;
  in.info.frame.width = 1280;
  in.info.frame.height = 720;
  in.info.frame_rate = {24, 1};
  in.info.frame_count = 130;
  in.info.has_audio = true;
  j.inputs.push_back(in);
  auto g = vp::build_regenerate_video(j);
  REQUIRE_OK(g);
  REQUIRE(g->movie);
  CHECK(g->movie->gate.group == 0);
  CHECK(g->movie->count == 121);
  CHECK(g->audio_is_source);
  CHECK(g->frames == 121);
  const Json* fit = find_stage(g->spec, "fit");
  REQUIRE(fit);
  CHECK(src_of(fit, 0) == "read");
  CHECK(jget(config_of(fit), "width", 0) == 1376);
  CHECK(jget<std::string>(config_of(fit), "fit", "") == "crop");
  const Json* enc = find_stage(g->spec, "encode");
  REQUIRE(enc);
  CHECK(jget<std::string>(*enc, "type", "") == "vae-encode");
  CHECK(src_of(enc, 0) == "stack");
  CHECK(jget<std::string>(config_of(enc), "posterior", "") == "sample");
  CHECK(jget(config_of(enc), "posterior_seed", 0) == 42);
  CHECK(src_of(find_stage(g->spec, "latent-upscale"), 0) == "encode");
  const Json* ref = find_stage(g->spec, "regenerate");
  REQUIRE(ref);
  CHECK(src_of(ref, 0) == "diffusion-conditioner");
  CHECK(src_of(ref, 11) == "latent-upscale");
  CHECK(jget(config_of(ref), "frames", 0) == 121);
  CHECK(jget(config_of(ref), "ane_ffn", false));
  // 1080 rows compute at 1088 and come back cut to 1080: no fit after.
  CHECK(!find_stage(g->spec, "fit-out"));
  const Json* tap = find_stage(g->spec, "tap");
  CHECK(src_of(tap, 0) == "regenerate-decode");
  CHECK(src_of(find_stage(g->spec, "write"), 0) == "tap");
  // A size the upscaler rounds DOWN (962 rows: 960) is fitted back.
  JobSpec odd = j;
  odd.params["height"] = 962;
  auto o = vp::build_regenerate_video(odd);
  REQUIRE_OK(o);
  CHECK(src_of(find_stage(o->spec, "fit-out"), 0) == "regenerate-decode");
  CHECK(src_of(find_stage(o->spec, "tap"), 0) == "fit-out");
  // Read at its own size: nothing fitted before the VAE.
  JobSpec own = j;
  own.params["encode"] = {{"width", 1280}, {"height", 720}};
  auto w = vp::build_regenerate_video(own);
  REQUIRE_OK(w);
  CHECK(!find_stage(w->spec, "fit"));
  CHECK(src_of(find_stage(w->spec, "stack"), 0) == "read");
  // Without words, or without the refiner, refused.
  JobSpec mute = j;
  mute.params.erase("prompt");
  CHECK(!vp::build_regenerate_video(mute).ok());
  JobSpec bare = j;
  bare.model.latent_upscaler.clear();
  CHECK(!vp::build_regenerate_video(bare).ok());
}

// A crop rides on the picture it belongs to, made real only as it is
// fed: an edit's base is fed as the crop's CANVAS (here a square cut
// from a 3:2 picture), and the crop goes with the feed to the decoder.
TEST(graph_builder, a_cropped_base_is_fed_as_its_canvas)
{
  JobSpec j = job_for("qwen-image-2.1", kOpEditImage);
  j.params["width"] = 1024;
  j.params["height"] = 1024;
  j.params["base_crop"] = {{"content_w", 1536}, {"content_h", 1024},
                           {"canvas_w", 1024},  {"canvas_h", 1024},
                           {"scale", 1.2},      {"rotate", 5.0}};
  std::vector<vp::RefImage> refs = {{"/tmp/base.png", {1536, 1024}, false,
                                     true}};
  auto g = vp::build_image_edit(j, refs);
  REQUIRE_OK(g);
  REQUIRE(g->references.size() == 1);
  const auto& f = g->references[0];
  CHECK(f.size.width == 1024 && f.size.height == 1024);
  CHECK(f.crop.scale_x == 1.2 && f.crop.rotate == 5.0);

  // A clip's first frame takes the same.
  JobSpec v = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  v.params = {{"prompt", "go"}, {"width", 832}, {"height", 480},
              {"base_crop", j.params["base_crop"]}};
  auto gv = vp::build_video(v, refs[0]);
  REQUIRE_OK(gv);
  REQUIRE(gv->references.size() == 1);
  CHECK(gv->references[0].size.width == 1024);
  CHECK(gv->references[0].crop.scale_y == 1.2);
}

// A trimmed clip exports from its mark-in, exactly as many frames as the
// marks keep, and its sound the same span.
TEST(graph_builder, a_trimmed_clip_exports_its_span)
{
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpExportMedia);
  job.output_dir = "/tmp/out";
  job.params = {{"format", "h264"},
                {"trim", {{"in", 12}, {"out", 35}, {"rate_num", 24},
                          {"rate_den", 1}}}};
  JobInput clip;
  clip.role = "source";
  clip.path = "/in/clip.mp4";
  clip.info.type = media::MediaType::Video;
  clip.info.frame_rate = Rational{24, 1};
  clip.info.frame_count = 56;
  clip.info.has_audio = true;
  job.inputs = {clip};
  auto g = vp::build_export(job);
  REQUIRE_OK(g);
  const Json r = config_of(find_stage(g->spec, vp::kExportRead));
  CHECK(std::abs(jget(r, "start_s", 0.0) - 0.5) < 1e-9);
  CHECK(jget(r, "max_frames", 0) == 24);
  CHECK(std::abs(g->audio_start - 0.5) < 1e-9);
  CHECK(std::abs(g->audio_duration - 1.0) < 1e-9);
  CHECK(g->audio_part == clip.path && g->audio_is_source);

  // A mark-out alone runs from the start; past the end, to the end.
  job.params["trim"] = {{"out", 500}, {"rate_num", 24}, {"rate_den", 1}};
  auto all = vp::build_export(job);
  REQUIRE_OK(all);
  CHECK(jget(config_of(find_stage(all->spec, vp::kExportRead)),
             "max_frames", 0) ==
        56);
}

// A clip with a look exports through a valtz-source: Valtz decodes its
// frames with the look at each one (media::decode_movie) from the trim's
// mark-in, and vpipe writes them; tagged with the clip's colour and rate.
TEST(graph_builder, a_clip_with_a_look_is_fed_frame_by_frame)
{
  JobSpec job;
  job.id = JobId::make();
  job.op = std::string(kOpExportMedia);
  job.output_dir = "/tmp/out";
  job.params = {
      {"format", "hevc10"},
      {"adjust", {{"keys", Json::array({{{"frame", 0}},
                                        {{"frame", 55}, {"exposure", 1}}})},
                  {"rate_num", 24}, {"rate_den", 1}}},
      {"trim", {{"in", 12}, {"out", 35}}}};
  JobInput clip;
  clip.role = "source";
  clip.path = "/in/clip.mp4";
  clip.info.type = media::MediaType::Video;
  clip.info.frame_rate = Rational{24, 1};
  clip.info.frame_count = 56;
  clip.info.frame.width = 832;
  clip.info.frame.height = 480;
  clip.info.frame.color.primaries = media::Primaries::BT709;
  clip.info.frame.color.transfer = media::Transfer::BT709;
  job.inputs = {clip};
  auto g = vp::build_export(job);
  REQUIRE_OK(g);
  REQUIRE(g->movie.has_value());
  const auto& m = *g->movie;
  CHECK(m.first == 12 && m.count == 24);
  CHECK(m.size.width == 832 && m.size.height == 480);
  CHECK(m.adjust.keys.size() == 2);
  CHECK(jget(m.tags, "color_transfer", 0) == 1);
  CHECK(jget<std::int64_t>(m.tags, "fps_num", 0) == 24);
  const Json* read = find_stage(g->spec, vp::kExportRead);
  REQUIRE(read);
  CHECK(jget<std::string>(*read, "type", "") == valtz::exchange::kSourceType);
  CHECK(src_of(find_stage(g->spec, vp::kExportWrite), 0) ==
        vp::kExportRead);
}

// Favor's Custom, as a recipe records it ("tuning", and the branch the
// job was given): each option reaches the stage that takes it.
TEST(graph_builder, tuning_reaches_the_stages)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "a kite"}, {"width", 832}, {"height", 480},
              {"frames", 39}, {"steps", 8}, {"seed", 1},
              {"tuning", {{"steps", 8}, {"hyperflow", true},
                          {"sol_attn", true}, {"sol_tau", 1.3},
                          {"sage_attn", true}, {"i8_gemm", false},
                          {"motion_cache", true}, {"video_shift", 10.0},
                          {"audio_shift", 2.5}, {"turbo", false}}}};
  j.model.lora = "/models/hyperflow/adapter.safetensors";
  j.model.branch = "/models/vdn/stage-dmd-step-250";
  auto g = vp::build_video(j, std::nullopt);
  REQUIRE_OK(g);
  const Json c = config_of(find_stage(g->spec, "minimax-h3-model-config"));
  CHECK(jget<std::string>(c, "linear_branch", "") == j.model.branch.string());
  CHECK(jget(c, "video_shift", 0.0) == 10.0);
  CHECK(jget(c, "audio_shift", 0.0) == 2.5);
  const Json gc = config_of(find_stage(g->spec, "generate-video"));
  CHECK(jget(gc, "sol_attn", false) && jget(gc, "sol_tau", 0.0) == 1.3);
  CHECK(jget(gc, "sage_attn", false) && jget(gc, "motion_cache", false));
  CHECK(!jget(gc, "i8_gemm", true));  // Custom's, over the catalog's
  CHECK(!gc.contains("hyperflow") && !gc.contains("turbo"));

  // A picture: the scheduler's shift, and the tiers generate-image takes.
  JobSpec p = job_for("krea2-turbo", kOpGenerateImage);
  p.params = {{"prompt", "a kite"}, {"width", 512}, {"height", 512},
              {"steps", 8}, {"seed", 1},
              {"tuning", {{"steps", 8}, {"shift", 0.5}, {"sol_attn", true},
                          {"sage_attn", false}, {"i8_gemm", true},
                          {"ane_ffn", true}, {"ane_qkv", true}}}};
  auto pg = vp::build_text_to_image(p);
  REQUIRE_OK(pg);
  const Json sc = config_of(find_stage(pg->spec, "scheduler-select"));
  CHECK(jget(sc, "shift", 0.0) == 0.5);
  const Json ic = config_of(find_stage(pg->spec, "generate-image"));
  CHECK(jget(ic, "sol_attn", false) && jget(ic, "i8_gemm", false));
  CHECK(jget(ic, "ane_ffn", false) && jget(ic, "ane_qkv", false));
  CHECK(!ic.contains("shift") && !ic.contains("steps_on"));
}

// Z-Image Turbo from words, as vpipe's own pipeline wires it
// (docs/pipelines/z-image-turbo-text-to-image): its config stage --
// guidance 0, the preview keys -- into the conditioner's model_config
// (5) and generate-image's (7); the scheduler simple, its shift 3.0
// linear, at the steps asked; no reference lists; a 16-bit result.
TEST(graph_builder, z_image_turbo_from_words)
{
  JobSpec j = job_for("z-image-turbo", kOpGenerateImage);
  j.model.preview = "/models/madebyollin/taef1";
  j.params = {{"prompt", "a red fox in snow"}, {"width", 512},
              {"height", 512}, {"steps", 8}, {"seed", 42}};
  auto g = vp::build_text_to_image(j);
  REQUIRE_OK(g);
  const Json& spec = g->spec;
  const Json* cfg = find_stage(spec, "z-image-model-config");
  REQUIRE(cfg);
  CHECK(jget<std::string>(*cfg, "type", "") == "z-image-model-config");
  const Json cc = config_of(cfg);
  CHECK(jget(cc, "guidance_scale", -1.0) == 0.0);
  CHECK(jget<std::string>(cc, "preview_vae", "") ==
        "/models/madebyollin/taef1");
  CHECK(g->preview_sink == "preview-sink");

  const Json sc = config_of(find_stage(spec, "scheduler-select"));
  CHECK(jget(sc, "steps", 0) == 8);
  CHECK(jget(sc, "shift", 0.0) == 3.0);
  CHECK(jget<std::string>(sc, "type", "") == "simple");
  CHECK(jget<std::string>(sc, "shift_type", "") == "linear");

  const Json* cond = find_stage(spec, "diffusion-conditioner");
  CHECK(src_of(cond, 0) == "text-prompt");
  CHECK(src_of(cond, 1).empty());                 // no negative
  CHECK(src_of(cond, 2) == "model-select");
  CHECK(src_of(cond, 5) == "z-image-model-config");
  const Json* gen = find_stage(spec, "generate-image");
  CHECK(src_of(gen, 0) == "diffusion-conditioner");
  CHECK(src_of(gen, 2) == "model-select");
  CHECK(src_of(gen, 4) == "scheduler-select");
  CHECK(src_of(gen, 7) == "z-image-model-config");
  CHECK(!find_stage(spec, vp::kRefList));
  const Json gc = config_of(gen);
  CHECK(jget(gc, "width", 0) == 512 && jget(gc, "height", 0) == 512);
  CHECK(jget(gc, "steps", 0) == 8 && jget(gc, "seed", 0) == 42);
  const Json* dec = find_stage(spec, "vae-decode");
  CHECK(src_of(dec, 0) == "generate-image");
  CHECK(jget(config_of(find_stage(spec, "save-image")), "bit_depth", 0) ==
        16);
}

// The LoRA slots after the first -- style or identity adapters, up to
// vpipe's eight in all -- reach the stage that takes them, each at its
// strength: H3's config stage, generate-image for pictures. A slot is its
// index, so one left empty keeps its place.
TEST(graph_builder, every_lora_slot)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "a kite"}, {"width", 832}, {"height", 480},
              {"frames", 39}, {"steps", 6}, {"seed", 1},
              {"lora_scale", 1.0}};
  j.model.lora = "/loras/turbo.safetensors";
  for (int k = 2; k <= kLoraSlots; ++k) {
    j.model.more_loras.push_back(std::format("/loras/s{}.safetensors", k));
    j.params[std::format("lora{}_scale", k)] = k / 10.0;
  }
  auto g = vp::build_video(j, std::nullopt);
  REQUIRE_OK(g);
  const Json c = config_of(find_stage(g->spec, "minimax-h3-model-config"));
  CHECK(jget<std::string>(c, "lora", "") == "/loras/turbo.safetensors");
  for (int k = 2; k <= kLoraSlots; ++k) {
    const std::string key = std::format("lora{}", k);
    CHECK(jget<std::string>(c, key.c_str(), "") ==
          std::format("/loras/s{}.safetensors", k));
    CHECK(jget(c, (key + "_scale").c_str(), 0.0) == k / 10.0);
  }
  CHECK(!c.contains("lora9"));

  JobSpec p = job_for("krea2-turbo", kOpGenerateImage);
  p.params = {{"prompt", "a kite"}, {"width", 512}, {"height", 512},
              {"steps", 8}, {"seed", 1}, {"lora2_scale", 0.9},
              {"lora4_scale", 0.5}};
  p.model.more_loras = {"/loras/m87.safetensors", "",
                        "/loras/ink.safetensors"};
  auto pg = vp::build_text_to_image(p);
  REQUIRE_OK(pg);
  const Json ic = config_of(find_stage(pg->spec, "generate-image"));
  CHECK(jget<std::string>(ic, "lora2", "") == "/loras/m87.safetensors");
  CHECK(jget(ic, "lora2_scale", 0.0) == 0.9);
  CHECK(!ic.contains("lora3"));
  CHECK(jget<std::string>(ic, "lora4", "") == "/loras/ink.safetensors");
  CHECK(jget(ic, "lora4_scale", 0.0) == 0.5);
  CHECK(!ic.contains("lora"));
}

// Community checkpoints: the DiT as generate-*'s dit_dir; the VAE through
// its own model-select, so the DiT stages keep the model's.
TEST(graph_builder, community_dit_and_vae)
{
  JobSpec p = job_for("krea2-turbo", kOpGenerateImage);
  p.params = {{"prompt", "a kite"}, {"width", 512}, {"height", 512},
              {"steps", 8}, {"seed", 1}};
  p.model.dit = "/ckpt/krea-finetune.safetensors";
  p.model.vae = "/ckpt/better-vae";
  auto g = vp::build_text_to_image(p);
  REQUIRE_OK(g);
  const Json ic = config_of(find_stage(g->spec, "generate-image"));
  CHECK(jget<std::string>(ic, "dit_dir", "") == p.model.dit.string());
  CHECK(src_of(find_stage(g->spec, "generate-image"), 2) == "model-select");
  const Json* vs = find_stage(g->spec, "vae-select");
  REQUIRE(vs);
  CHECK(jget<std::string>(config_of(vs), "hf_dir", "") ==
        p.model.vae.string());
  CHECK(src_of(find_stage(g->spec, "vae-decode"), 1) == "vae-select");

  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "a kite"}, {"width", 832}, {"height", 480},
              {"frames", 39}, {"steps", 6}, {"seed", 1}};
  j.model.dit = "/ckpt/h3-fp8.safetensors";
  auto v = vp::build_video(j, std::nullopt);
  REQUIRE_OK(v);
  CHECK(jget<std::string>(config_of(find_stage(v->spec, "generate-video")),
                          "dit_dir", "") == j.model.dit.string());
  CHECK(!find_stage(v->spec, "vae-select"));
  CHECK(src_of(find_stage(v->spec, "vae-decode"), 1) == "model-select");
}

// A family's own options, declared by its extension (catalog
// engine.vpipe.options): each settled value goes to the stage the option
// names -- generate-image, or the family's config stage.
TEST(graph_builder, declared_options_reach_their_stages)
{
  JobSpec p = job_for("krea2-turbo", kOpGenerateImage);
  p.model.engine["vpipe"]["options"] = {
    {{"key", "acme_cache"}, {"type", "bool"}},
    {{"key", "acme_window"}, {"type", "int"}, {"stage", "config"}},
    {{"key", "acme_unset"}, {"type", "bool"}}};
  p.params = {{"prompt", "a kite"}, {"width", 512}, {"height", 512},
              {"steps", 8}, {"seed", 1},
              {"tuning", {{"acme_cache", true}, {"acme_window", 6}}}};
  auto g = vp::build_text_to_image(p);
  REQUIRE_OK(g);
  const Json ic = config_of(find_stage(g->spec, "generate-image"));
  CHECK(jget(ic, "acme_cache", false));
  CHECK(!ic.contains("acme_window") && !ic.contains("acme_unset"));
  const Json cc = config_of(find_stage(g->spec, "krea2-model-config"));
  CHECK(jget(cc, "acme_window", 0) == 6);
}

// ---- kept encodings (DESIGN §4l) ------------------------------------------

namespace {

// An EncodingCache holding the keys in `held`.
struct FakeCache {
  vp::EncodingCache     c;
  std::set<std::string> held;

  explicit FakeCache(std::string model = "stamp-1")
  {
    c.engine = "vpipe test";
    c.model = std::move(model);
    c.kept = [this](const std::string& k) { return held.contains(k); };
  }
  FakeCache(const FakeCache&) = delete;
};

const vp::EncodingIo*
io_of(const vp::BuiltGraph& g, std::string_view what, std::size_t nth = 0)
{
  for (const auto& io : g.encodings) {
    if (io.what == what && nth-- == 0) {
      return &io;
    }
  }
  return nullptr;
}

std::string
type_of(const Json* stage)
{
  return stage ? jget<std::string>(*stage, "type", "") : "<none>";
}

}

// A prompt's conditioning: tapped beside the DiT as it is made, named by
// the words, the model's files, vpipe's build and the config beat it
// reads -- not the seed, the steps, the size or the preview -- and, kept,
// fed in the conditioner's place: no text encoder, no words read.
TEST(graph_builder, a_prompts_conditioning_is_tapped_then_fed)
{
  JobSpec j = job_for("krea2-turbo", kOpGenerateImage);
  j.model.preview = "/models/tae";
  FakeCache fc;
  auto miss = vp::build_text_to_image(j, &fc.c);
  REQUIRE_OK(miss);
  const auto* io = io_of(*miss, "conditioning");
  REQUIRE(io);
  CHECK(!io->fed);
  REQUIRE(io->ports.size() == 1);
  const Json* tap = find_stage(miss->spec, io->ports[0].stage);
  CHECK(type_of(tap) == ex::kSinkType);
  CHECK(src_of(tap, 0) == "diffusion-conditioner" && oport_of(tap, 0) == 0);
  CHECK(src_of(find_stage(miss->spec, "generate-image"), 0) ==
        "diffusion-conditioner");
  const std::string key = io->key;
  CHECK(key.starts_with("enc-conditioning-"));
  CHECK(cache::CacheStore::valid_key(key));

  JobSpec other = j;
  other.params["seed"] = 8;
  other.params["steps"] = 4;
  other.params["width"] = 768;
  other.params["preview"] = false;
  auto same = vp::build_text_to_image(other, &fc.c);
  REQUIRE_OK(same);
  CHECK(io_of(*same, "conditioning")->key == key);
  other = j;
  other.params["prompt"] = "a hat";
  auto words = vp::build_text_to_image(other, &fc.c);
  REQUIRE_OK(words);
  CHECK(io_of(*words, "conditioning")->key != key);
  FakeCache moved("stamp-2");
  auto files = vp::build_text_to_image(j, &moved.c);
  REQUIRE_OK(files);
  CHECK(io_of(*files, "conditioning")->key != key);

  fc.held.insert(key);
  auto hit = vp::build_text_to_image(j, &fc.c);
  REQUIRE_OK(hit);
  CHECK(!find_stage(hit->spec, "diffusion-conditioner"));
  CHECK(!find_stage(hit->spec, "text-prompt"));
  const auto* fed = io_of(*hit, "conditioning");
  REQUIRE(fed && fed->fed && fed->key == key);
  CHECK(type_of(find_stage(hit->spec, fed->ports[0].stage)) ==
        ex::kSourceType);
  const Json* gen = find_stage(hit->spec, "generate-image");
  CHECK(src_of(gen, 0) == fed->ports[0].stage);
  CHECK(src_of(gen, 7) == "krea2-model-config");
  // No cache: the graph as ever.
  auto plain = vp::build_text_to_image(j);
  REQUIRE_OK(plain);
  CHECK(plain->encodings.empty());
  CHECK(!find_stage(plain->spec, vp::kConditioningTap));
}

// An edit's two encodings are kept apart: the pictures' latents by the
// pictures alone, the conditioning by the words -- and the pictures, where
// the text encoder sees them (Qwen-Image 2.1). Each kept is fed; a
// picture is still read while something encodes it, and not at all once
// both are kept (the base's EXIF still carried to the result).
TEST(graph_builder, an_edits_latents_and_conditioning_are_kept_apart)
{
  JobSpec j = job_for("qwen-image-2.1", kOpEditImage);
  std::vector<vp::RefImage> pics = {
      {"/base.png", {1024, 768}, false, true, ContentHash::of("base")},
      {"/ref.png", {512, 512}, false, false, ContentHash::of("ref")}};
  FakeCache fc;
  auto miss = vp::build_image_edit(j, pics, &fc.c);
  REQUIRE_OK(miss);
  const auto* cond = io_of(*miss, "conditioning");
  const auto* lat = io_of(*miss, "latents");
  REQUIRE(cond && lat);
  CHECK(!cond->fed && !lat->fed);
  REQUIRE(lat->ports.size() == 1);
  CHECK(lat->ports[0].list);
  const Json* tap = find_stage(miss->spec, lat->ports[0].stage);
  CHECK(src_of(tap, 0) == vp::kRefLatents && oport_of(tap, 0) == 1);
  const std::string cond_key = cond->key;
  const std::string lat_key = lat->key;

  // Latents kept, other words: the pictures read for the text encoder,
  // not encoded again.
  fc.held.insert(lat_key);
  JobSpec hat = j;
  hat.params["prompt"] = "a hat";
  auto lk = vp::build_image_edit(hat, pics, &fc.c);
  REQUIRE_OK(lk);
  CHECK(io_of(*lk, "latents")->fed && io_of(*lk, "latents")->key == lat_key);
  CHECK(io_of(*lk, "conditioning")->key != cond_key);
  CHECK(!find_stage(lk->spec, vp::kRefLatents));
  CHECK(lk->references.size() == 2);
  CHECK(src_of(find_stage(lk->spec, "diffusion-conditioner"), 6) ==
        vp::kRefList);
  CHECK(src_of(find_stage(lk->spec, "generate-image"), 8) ==
        vp::kKeptLatents);

  // Both kept: no picture read, no encoder.
  fc.held.insert(cond_key);
  auto both = vp::build_image_edit(j, pics, &fc.c);
  REQUIRE_OK(both);
  CHECK(both->references.empty());
  CHECK(!find_stage(both->spec, "ref-0") && !find_stage(both->spec,
                                                        vp::kRefList));
  CHECK(!find_stage(both->spec, "diffusion-conditioner"));
  CHECK(!find_stage(both->spec, "text-prompt"));
  CHECK(both->exif_from == "/base.png");
  const Json* gen = find_stage(both->spec, "generate-image");
  CHECK(src_of(gen, 0) == vp::kKeptConditioning);
  CHECK(src_of(gen, 8) == vp::kKeptLatents);

  // Another base: both are another encoding.
  auto moved = pics;
  moved[0].hash = ContentHash::of("another base");
  auto other = vp::build_image_edit(j, moved, &fc.c);
  REQUIRE_OK(other);
  CHECK(io_of(*other, "conditioning")->key != cond_key);
  CHECK(io_of(*other, "latents")->key != lat_key);
  // The same base with a look laid on: another picture.
  JobSpec looked = j;
  looked.params["base_adjust"] = {{"exposure", 0.5}};
  auto lit = vp::build_image_edit(looked, pics, &fc.c);
  REQUIRE_OK(lit);
  CHECK(io_of(*lit, "latents")->key != lat_key);
  // A picture whose bytes are not known: nothing kept for the edit.
  moved[0].hash = {};
  auto unknown = vp::build_image_edit(j, moved, &fc.c);
  REQUIRE_OK(unknown);
  CHECK(unknown->encodings.empty());
  CHECK(find_stage(unknown->spec, "diffusion-conditioner"));
}

// FLUX.2 klein's text encoder sees no picture: its conditioning is the
// words' alone, kept across bases, and with it kept the edit reads its
// pictures for the VAE only.
TEST(graph_builder, a_word_only_conditioning_is_kept_across_pictures)
{
  JobSpec j = job_for("flux2-klein-9b", kOpEditImage);
  std::vector<vp::RefImage> pics = {
      {"/base.png", {1024, 1024}, false, true, ContentHash::of("base")}};
  FakeCache fc;
  auto a = vp::build_image_edit(j, pics, &fc.c);
  REQUIRE_OK(a);
  pics[0].hash = ContentHash::of("another");
  auto b = vp::build_image_edit(j, pics, &fc.c);
  REQUIRE_OK(b);
  CHECK(io_of(*a, "conditioning")->key == io_of(*b, "conditioning")->key);
  CHECK(io_of(*a, "latents")->key != io_of(*b, "latents")->key);
  fc.held.insert(io_of(*a, "conditioning")->key);
  auto kept = vp::build_image_edit(j, pics, &fc.c);
  REQUIRE_OK(kept);
  CHECK(!find_stage(kept->spec, "diffusion-conditioner"));
  CHECK(find_stage(kept->spec, vp::kRefLatents));
  CHECK(kept->references.size() == 1);
}

// Draw Things checkpoints of a model's parts (ModelRef::encoder_weights /
// vae_weights; its DiT is `dit`) go to every stage that reads those
// WEIGHTS -- the conditioner, the reference encoder, each VAE -- as
// `encoder_dir` / `vae_dir`, the model's own folder still hf_dir; and an
// encoding made with them is keyed apart from one made without
// (EncodingCache::weights).
TEST(graph_builder, a_draw_things_models_parts_reach_their_stages)
{
  const std::string enc = "/dt/qwen_3_vl_8b_instruct_i8x.ckpt";
  const std::string vae = "/dt/qwen_image_2.1_vae_f16.ckpt";
  JobSpec e = job_for("qwen-image-2.1", kOpEditImage);
  e.model.encoder_weights = enc;
  e.model.vae_weights = vae;
  std::vector<vp::RefImage> pics = {
      {"/base.png", {1024, 768}, false, true, ContentHash::of("base")}};
  FakeCache plain;
  auto g = vp::build_image_edit(e, pics, &plain.c);
  REQUIRE_OK(g);
  CHECK(jget<std::string>(config_of(find_stage(g->spec,
                                               "diffusion-conditioner")),
                          "encoder_dir", "") == enc);
  CHECK(jget<std::string>(config_of(find_stage(g->spec, vp::kRefLatents)),
                          "vae_dir", "") == vae);
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "vae-decode")),
                          "vae_dir", "") == vae);
  CHECK(jget<std::string>(config_of(find_stage(g->spec, "model-select")),
                          "hf_dir", "") == e.model.dir.string());
  // None linked: none laid on.
  JobSpec own = job_for("qwen-image-2.1", kOpEditImage);
  auto o = vp::build_image_edit(own, pics, &plain.c);
  REQUIRE_OK(o);
  CHECK(!config_of(find_stage(o->spec, "diffusion-conditioner"))
             .contains("encoder_dir"));
  CHECK(!config_of(find_stage(o->spec, "vae-decode")).contains("vae_dir"));
  // Keyed apart: the same job with Draw Things' encoder and VAE stamped
  // -- each encoding by the one it reads.
  FakeCache stamped;
  stamped.c.weights = {{"encoder", "e-1"}, {"vae", "v-1"}};
  auto st = vp::build_image_edit(e, pics, &stamped.c);
  REQUIRE_OK(st);
  CHECK(io_of(*st, "conditioning")->key != io_of(*g, "conditioning")->key);
  CHECK(io_of(*st, "latents")->key != io_of(*g, "latents")->key);
  FakeCache vae_only;
  vae_only.c.weights = {{"vae", "v-1"}};
  auto vo = vp::build_image_edit(e, pics, &vae_only.c);
  REQUIRE_OK(vo);
  CHECK(io_of(*vo, "conditioning")->key == io_of(*g, "conditioning")->key);
  CHECK(io_of(*vo, "latents")->key == io_of(*st, "latents")->key);

  // MiniMax H3: the conditioner, the keyframes' encoders, both decoders.
  const std::string h3e = "/dt/qwen_3_vl_32b_i8x.ckpt";
  const std::string h3v = "/dt/minimax_h3_vae_f16.ckpt";
  JobSpec f = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  f.params = {{"prompt", "she opens the umbrella"}, {"width", 832},
              {"height", 480}, {"frames", 39}, {"steps", 8}};
  f.model.encoder_weights = h3e;
  f.model.vae_weights = h3v;
  vp::RefImage first{"/closed.png", {1536, 1024}, false, true,
                     ContentHash::of("closed")};
  auto v = vp::build_video(f, first, {}, std::nullopt, &plain.c);
  REQUIRE_OK(v);
  CHECK(jget<std::string>(config_of(find_stage(v->spec,
                                               "diffusion-conditioner")),
                          "encoder_dir", "") == h3e);
  for (const char* id : {"first-latent", "vae-decode", "audio-vae-decode"}) {
    CHECK(jget<std::string>(config_of(find_stage(v->spec, id)), "vae_dir",
                            "") == h3v);
  }
  // Ref2VA's reference encoder: both.
  JobSpec r = job_for("minimax-h3-ref2va", kOpGenerateVideo);
  r.params = {{"prompt", "<Picture 1> waves"}, {"width", 832},
              {"height", 480}, {"frames", 39}, {"steps", 8}};
  r.model.encoder_weights = h3e;
  r.model.vae_weights = h3v;
  std::vector<vp::RefMedia> refs(1);
  refs[0].path = "/p/cat.png";
  refs[0].kind = "image";
  refs[0].hash = ContentHash::of("cat");
  auto rv = vp::build_video(r, std::nullopt, refs, std::nullopt, &plain.c);
  REQUIRE_OK(rv);
  const Json rc = config_of(find_stage(rv->spec, "video-ref-encoder"));
  CHECK(jget<std::string>(rc, "encoder_dir", "") == h3e);
  CHECK(jget<std::string>(rc, "vae_dir", "") == h3v);
}

// A clip's encoders, kept as a picture's: H3's prompt reader (its 32B
// conditioner), the pictures it opens and ends on (each a VAE latent),
// and Ref2VA's reference encoder -- its three outputs -- the clip a
// continuation is matched to still read for its seam.
TEST(graph_builder, a_clips_encoders_are_kept)
{
  JobSpec f = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  f.params = {{"prompt", "she opens the umbrella"}, {"width", 832},
              {"height", 480}, {"frames", 124}, {"steps", 8}};
  vp::RefImage first{"/closed.png", {1536, 1024}, false, true,
                     ContentHash::of("closed")};
  vp::RefImage last{"/open.png", {1024, 1536}, false, true,
                    ContentHash::of("open")};
  FakeCache fc;
  auto miss = vp::build_video(f, first, {}, last, &fc.c);
  REQUIRE_OK(miss);
  const auto* cond = io_of(*miss, "conditioning");
  const auto* k0 = io_of(*miss, "keyframe", 0);
  const auto* k1 = io_of(*miss, "keyframe", 1);
  REQUIRE(cond && k0 && k1);
  CHECK(k0->ports[0].stage == "first-latent-tap");
  CHECK(src_of(find_stage(miss->spec, "first-latent-tap"), 0) ==
        "first-latent");
  CHECK(k0->key != k1->key);
  for (const auto* io : {cond, k0, k1}) {
    fc.held.insert(io->key);
  }
  auto hit = vp::build_video(f, first, {}, last, &fc.c);
  REQUIRE_OK(hit);
  CHECK(!find_stage(hit->spec, "diffusion-conditioner"));
  CHECK(!find_stage(hit->spec, "text-prompt"));
  CHECK(!find_stage(hit->spec, "first") && !find_stage(hit->spec, "last"));
  CHECK(hit->references.empty());
  const Json* gen = find_stage(hit->spec, "generate-video");
  CHECK(src_of(gen, 0) == vp::kKeptConditioning);
  CHECK(src_of(gen, 5) == "first-latent-kept");
  CHECK(src_of(gen, 6) == "last-latent-kept");

  JobSpec r = job_for("minimax-h3-ref2va", kOpGenerateVideo);
  r.params = {{"prompt", "[video continuation + audio reference] ..."},
              {"width", 832}, {"height", 480}, {"frames", 124},
              {"fps", 24.0}, {"steps", 8}, {"seed", 7}};
  std::vector<vp::RefMedia> refs(2);
  refs[0].path = "/p/guide.mov";
  refs[0].kind = "video";
  refs[0].list = false;
  refs[0].audio = true;
  refs[0].continues = true;
  refs[0].width = 832;
  refs[0].height = 480;
  refs[0].count = 56;
  refs[0].hash = ContentHash::of("guide");
  refs[1].path = "/p/cat.png";
  refs[1].kind = "image";
  refs[1].hash = ContentHash::of("cat");
  auto rm = vp::build_video(r, std::nullopt, refs, std::nullopt, &fc.c);
  REQUIRE_OK(rm);
  const auto* io = io_of(*rm, "references");
  REQUIRE(io && !io->fed);
  REQUIRE(io->ports.size() == 3);
  for (int k = 0; k < 3; ++k) {
    const Json* t = find_stage(rm->spec, io->ports[k].stage);
    CHECK(src_of(t, 0) == "video-ref-encoder" && oport_of(t, 0) == k);
  }
  // Another seed and steps: the same references; another tail: not.
  JobSpec r2 = r;
  r2.params["seed"] = 9;
  auto same = vp::build_video(r2, std::nullopt, refs, std::nullopt, &fc.c);
  CHECK(io_of(*same, "references")->key == io->key);
  auto tail = refs;
  tail[0].count = 73;
  auto longer = vp::build_video(r, std::nullopt, tail, std::nullopt, &fc.c);
  CHECK(io_of(*longer, "references")->key != io->key);

  fc.held.insert(io->key);
  auto rh = vp::build_video(r, std::nullopt, refs, std::nullopt, &fc.c);
  REQUIRE_OK(rh);
  CHECK(!find_stage(rh->spec, "video-ref-encoder"));
  CHECK(!find_stage(rh->spec, "text-prompt"));
  const Json* rg = find_stage(rh->spec, "generate-video");
  CHECK(src_of(rg, 0) == "references-kept-conditioning");
  CHECK(src_of(rg, 7) == "references-kept-ref_video");
  CHECK(src_of(rg, 8) == "references-kept-ref_audio");
  // The seam still has its clip; the clip's sound, for the encoder only,
  // is not read.
  CHECK(src_of(find_stage(rh->spec, "seam"), 1) == "ref1");
  CHECK(find_stage(rh->spec, "ref1-read"));
  CHECK(!find_stage(rh->spec, "ref2-read"));
}

// Pika H3 Flex (vpipe 24246ed1): its adapter on the config stage as any
// LoRA (vpipe reads its manifest beside it) and, as its release's graphs
// have it, the keyframes SHOWN to the prompt encoder too -- its ports 3
// and 4 -- each picture fed once for both readers; kept, the
// conditioning is keyed by the pictures it was shown.
TEST(graph_builder, pika_h3_flex_shows_its_keyframes)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "<Picture 1> opens the umbrella"},
              {"width", 832}, {"height", 480}, {"frames", 124},
              {"steps", 6},
              {"tuning", {{"pika_flex", true}, {"video_shift", 12},
                          {"audio_shift", 3}}}};
  j.model.lora = "/m/Pika-Labs/Pika-H3-Flex/fl2va/adapter.safetensors";
  vp::RefImage first{"/closed.png", {1536, 1024}, false, true,
                     ContentHash::of("closed")};
  vp::RefImage last{"/open.png", {1024, 1536}, false, true,
                    ContentHash::of("open")};
  auto g = vp::build_video(j, first, {}, last);
  REQUIRE_OK(g);
  const Json* cond = find_stage(g->spec, "diffusion-conditioner");
  REQUIRE(cond);
  REQUIRE((*cond)["iports"].size() == 5);
  CHECK(src_of(cond, 0) == "text-prompt");
  CHECK(src_of(cond, 2) == "model-select");
  CHECK(src_of(cond, 3) == "first-fit" && src_of(cond, 4) == "last-fit");
  CHECK(src_of(find_stage(g->spec, "first-latent"), 0) == "first-fit");
  CHECK(src_of(find_stage(g->spec, "last-latent"), 0) == "last-fit");
  REQUIRE(g->references.size() == 2);
  CHECK(g->references[0].stage == "first");
  CHECK(g->references[1].stage == "last");
  int fits = 0;
  for (const Json& st : g->spec["stages"]) {
    fits += jget<std::string>(st, "id", "") == "first-fit";
  }
  CHECK(fits == 1);
  const Json cfg = config_of(find_stage(g->spec, "minimax-h3-model-config"));
  CHECK(jget<std::string>(cfg, "lora", "") == j.model.lora.string());
  CHECK(jget(cfg, "lora_scale", 0.0) == 1.0);
  // Opening alone: nothing on port 4.
  auto one = vp::build_video(j, first);
  REQUIRE_OK(one);
  const Json* c1 = find_stage(one->spec, "diffusion-conditioner");
  CHECK(src_of(c1, 3) == "first-fit" && src_of(c1, 4).empty());
  // Without it, the words alone.
  JobSpec plain = j;
  plain.params["tuning"] = {{"pika_flex", false}};
  auto p = vp::build_video(plain, first, {}, last);
  REQUIRE_OK(p);
  CHECK((*find_stage(p->spec, "diffusion-conditioner"))["iports"].size() ==
        3);
  // Kept: the conditioning keyed by the pictures it was shown.
  FakeCache fc;
  auto a = vp::build_video(j, first, {}, last, &fc.c);
  auto b = vp::build_video(plain, first, {}, last, &fc.c);
  vp::RefImage other = last;
  other.hash = ContentHash::of("other");
  auto o = vp::build_video(j, first, {}, other, &fc.c);
  REQUIRE_OK(a);
  REQUIRE_OK(b);
  REQUIRE_OK(o);
  const auto* ka = io_of(*a, "conditioning");
  const auto* kb = io_of(*b, "conditioning");
  const auto* ko = io_of(*o, "conditioning");
  REQUIRE(ka && kb && ko);
  CHECK(ka->key != kb->key && ka->key != ko->key);
  // Its latents kept and not its conditioning: the pictures still fed,
  // for the encoder alone.
  fc.held.insert(io_of(*a, "keyframe", 0)->key);
  fc.held.insert(io_of(*a, "keyframe", 1)->key);
  auto d = vp::build_video(j, first, {}, last, &fc.c);
  REQUIRE_OK(d);
  CHECK(src_of(find_stage(d->spec, "diffusion-conditioner"), 3) ==
        "first-fit");
  CHECK(src_of(find_stage(d->spec, "generate-video"), 5) ==
        "first-latent-kept");
  CHECK(!find_stage(d->spec, "first-latent"));
  CHECK(d->references.size() == 2);
  // All kept: nothing read.
  fc.held.insert(ka->key);
  auto e = vp::build_video(j, first, {}, last, &fc.c);
  REQUIRE_OK(e);
  CHECK(!find_stage(e->spec, "diffusion-conditioner"));
  CHECK(e->references.empty());
}

// A kept encoding's file: every port's tensors -- bf16, a strided view
// made contiguous, a list, one of no bytes -- and their sidebands, read
// back as written; a file cut short or of another kind refused.
TEST(graph_builder, a_kept_encoding_is_written_and_read)
{
  const fs::path dir = test::temp_dir("kept-enc");
  std::vector<std::uint8_t> six(12);
  for (std::size_t i = 0; i < six.size(); ++i) {
    six[i] = static_cast<std::uint8_t>(i + 1);
  }
  vp::Kept k;
  k.ports.push_back({"conditioning", false,
                     {make_owned_tensor(Tensor::DType::BF16, {2, 3}, six,
                                        Json::object())},
                     {std::string("\x01\x02\x00sb", 5)}, {false}, ""});
  // [2, 2] of a [2, 3] u8: rows 3 apart.
  auto store = std::make_shared<std::vector<std::uint8_t>>(
      std::vector<std::uint8_t>{1, 2, 99, 3, 4, 99});
  auto view = std::make_shared<Tensor>();
  view->dtype = Tensor::DType::U8;
  view->shape = {2, 2};
  view->strides = {3, 1};
  view->data = store->data();
  view->byte_size = store->size();
  view->owner = store;
  std::vector<std::uint8_t> f8(8, 7);
  k.ports.push_back({"latents", true,
                     {view, make_owned_tensor(Tensor::DType::F32, {1, 2}, f8,
                                              Json::object())},
                     {"", "item"}, {true, false}, "list"});
  auto none = std::make_shared<Tensor>();
  none->dtype = Tensor::DType::F32;
  none->shape = {0, 96};
  k.ports.push_back({"ref_video", false, {none}, {""}, {false}, ""});
  const fs::path file = dir / "k.enc";
  REQUIRE_OK(vp::write_kept(file, k));
  auto back = vp::read_kept(file);
  REQUIRE_OK(back);
  REQUIRE(back->ports.size() == 3);
  const auto& c = back->ports[0];
  CHECK(c.name == "conditioning" && !c.list);
  REQUIRE(c.tensors.size() == 1);
  CHECK(c.tensors[0]->dtype == Tensor::DType::BF16);
  CHECK(c.tensors[0]->shape == (std::vector<std::int64_t>{2, 3}));
  CHECK(std::memcmp(c.tensors[0]->data, six.data(), 12) == 0);
  CHECK(c.sidebands[0] == std::string("\x01\x02\x00sb", 5));
  CHECK(c.shared == (std::vector<bool>{false}));
  const auto& l = back->ports[1];
  CHECK(l.list && l.list_sideband == "list");
  REQUIRE(l.tensors.size() == 2);
  CHECK(l.tensors[0]->byte_size == 4);
  CHECK(l.tensors[0]->data[0] == 1 && l.tensors[0]->data[1] == 2 &&
        l.tensors[0]->data[2] == 3 && l.tensors[0]->data[3] == 4);
  CHECK(l.sidebands[0].empty() && l.sidebands[1] == "item");
  CHECK(l.shared == (std::vector<bool>{true, false}));
  CHECK(l.tensors[1]->shape == (std::vector<std::int64_t>{1, 2}));
  const auto& z = back->ports[2];
  CHECK(z.tensors[0]->shape == (std::vector<std::int64_t>{0, 96}));
  CHECK(z.tensors[0]->byte_size == 0);
  CHECK(back->bytes() == 12 + 4 + 8);

  // Cut short, or not one at all.
  const auto size = fs::file_size(file);
  fs::resize_file(file, size - 3);
  CHECK(vp::read_kept(file).code() == Code::Corrupt);
  {
    std::ofstream o(file, std::ios::binary | std::ios::trunc);
    o << "not a kept encoding at all";
  }
  CHECK(vp::read_kept(file).code() == Code::Corrupt);
  CHECK(vp::read_kept(dir / "absent").code() == Code::NotFound);
}

// The model's files as they are: the same stamp until a file's size or
// time changes; a Finder's .DS_Store is not the model; no folder, none.
TEST(graph_builder, a_models_stamp_follows_its_files)
{
  const fs::path dir = test::temp_dir("stamp");
  fs::create_directories(dir / "text_encoder");
  {
    std::ofstream(dir / "model.safetensors") << "0123456789";
    std::ofstream(dir / "text_encoder" / "config.json") << "{}";
  }
  const std::string a = vp::model_stamp(dir);
  CHECK(a.size() == 64);
  std::ofstream(dir / ".DS_Store") << "finder";
  CHECK(vp::model_stamp(dir) == a);
  std::ofstream(dir / "model.safetensors", std::ios::app) << "more";
  CHECK(vp::model_stamp(dir) != a);
  CHECK(vp::model_stamp(dir / "absent").empty());
}

// The CODEC's int8 (Favor's `vae_i8_gemm`, `vae_i8_gemm_tier`) goes on
// the stages that run the codec as their own `i8_gemm` and tier -- the
// decode, and an edit's pictures' encode -- and not on the DiT's stage;
// off, the graph is as it was. An int8-made latent is kept apart.
TEST(graph_builder, the_codecs_int8_goes_on_its_vae_stages)
{
  std::vector<vp::RefImage> pics = {
      {"/base.png", {1024, 768}, false, true, ContentHash::of("base")}};
  FakeCache cache;
  auto edit = [&](const Json& tuning) {
    JobSpec e = job_for("qwen-image-2.1", kOpEditImage);
    e.params["tuning"] = tuning;
    return vp::build_image_edit(e, pics, &cache.c);
  };
  auto on = edit({{"vae_i8_gemm", true}, {"vae_i8_gemm_tier", "fast"}});
  REQUIRE_OK(on);
  for (const char* id : {"vae-decode", vp::kRefLatents}) {
    const Json c = config_of(find_stage(on->spec, id));
    CHECK(jget(c, "i8_gemm", false));
    CHECK(jget<std::string>(c, "i8_gemm_tier", "") == "fast");
  }
  const Json gen = config_of(find_stage(on->spec, "generate-image"));
  CHECK(!gen.contains("i8_gemm") && !gen.contains("i8_gemm_tier"));
  CHECK(jget<std::string>(config_of(find_stage(on->spec, "vae-decode")),
                          "dtype", "") == "f16");
  // Accurate: the switch alone, vpipe's default tier.
  auto acc = edit({{"vae_i8_gemm", true}, {"vae_i8_gemm_tier", "accurate"}});
  REQUIRE_OK(acc);
  const Json ac = config_of(find_stage(acc->spec, "vae-decode"));
  CHECK(jget(ac, "i8_gemm", false) && !ac.contains("i8_gemm_tier"));
  // Off: neither key, and the DiT's own int8 stays the DiT's.
  auto off = edit({{"vae_i8_gemm", false}, {"vae_i8_gemm_tier", "fast"},
                   {"i8_gemm", true}});
  REQUIRE_OK(off);
  for (const char* id : {"vae-decode", vp::kRefLatents}) {
    const Json c = config_of(find_stage(off->spec, id));
    CHECK(!c.contains("i8_gemm") && !c.contains("i8_gemm_tier"));
  }
  CHECK(jget(config_of(find_stage(off->spec, "generate-image")), "i8_gemm",
             false));
  // The latents keyed by it; the conditioning, which the codec never
  // touches, not.
  auto plain = edit(Json::object());
  REQUIRE_OK(plain);
  CHECK(io_of(*on, "latents")->key != io_of(*plain, "latents")->key);
  CHECK(io_of(*acc, "latents")->key != io_of(*on, "latents")->key);
  CHECK(io_of(*off, "latents")->key == io_of(*plain, "latents")->key);
  CHECK(io_of(*on, "conditioning")->key ==
        io_of(*plain, "conditioning")->key);
}

// ...and the 2D AutoencoderKL FLUX.2 shares with FLUX.3, Z-Image,
// MOSS-Image and Fibo 2.0 (vpipe eeec3c5c): each takes the switch and its
// tier on its decode (an edit's encode too), and Fast runs it at Fast on
// a Mac with matrix cores.
TEST(graph_builder, the_flux2_vaes_take_the_codecs_int8)
{
  const Json on = {{"vae_i8_gemm", true}, {"vae_i8_gemm_tier", "fast"}};
  for (const char* id : {"flux2-klein-9b", "flux3-klein", "z-image-turbo",
                         "moss-image-sft", "moss-image-base"}) {
    JobSpec j = job_for(id, kOpGenerateImage);
    j.params["tuning"] = on;
    auto g = vp::build_text_to_image(j);
    REQUIRE_OK(g);
    const Json c = config_of(find_stage(g->spec, "vae-decode"));
    CHECK(jget(c, "i8_gemm", false));
    CHECK(jget<std::string>(c, "i8_gemm_tier", "") == "fast");
  }
  std::vector<vp::RefImage> pics = {{"/base.png", {1024, 768}, false, true}};
  for (const char* id : {"flux2-klein-9b", "flux3-klein",
                         "fibo2-edit-turbo"}) {
    JobSpec j = job_for(id, kOpEditImage);
    j.params["tuning"] = on;
    auto e = vp::build_image_edit(j, pics);
    REQUIRE_OK(e);
    for (const char* st : {"vae-decode", vp::kRefLatents}) {
      CHECK(jget(config_of(find_stage(e->spec, st)), "i8_gemm", false));
    }
  }
}

// MOSS-IMAGE (vpipe 3b34e70a): text to image through generate-image, its
// config stage taking the guidance and the shift Custom sets (it has no
// scheduler-select), and its CFG run against the NEGATIVE the conditioner
// emits on its second output -- the empty prompt's; unwired, vpipe runs
// it without guidance. Kept, the conditioning is both outputs, tapped and
// fed together.
TEST(graph_builder, moss_image_runs_its_cfg_against_the_conditioners_negative)
{
  auto job = job_for("moss-image-sft", kOpGenerateImage);
  REQUIRE(job.model.family == "moss-image");
  job.params["tuning"] = {{"guidance_scale", 4.5}, {"shift", 6.0}};
  auto g = vp::build_text_to_image(job);
  REQUIRE_OK(g);
  CHECK(!find_stage(g->spec, "scheduler-select"));
  const Json* cfg = find_stage(g->spec, "moss-image-model-config");
  REQUIRE(cfg);
  CHECK(jget(config_of(cfg), "guidance_scale", 0.0) == 4.5);
  CHECK(jget(config_of(cfg), "shift", 0.0) == 6.0);
  const Json* gen = find_stage(g->spec, "generate-image");
  REQUIRE(gen);
  CHECK(src_of(gen, 0) == "diffusion-conditioner" && oport_of(gen, 0) == 0);
  CHECK(src_of(gen, 1) == "diffusion-conditioner" && oport_of(gen, 1) == 1);
  CHECK(src_of(gen, 7) == "moss-image-model-config");
  CHECK(!config_of(gen).contains("guidance_scale"));
  // Another family's negative stays unwired.
  auto krea = vp::build_text_to_image(job_for("krea2-turbo",
                                              kOpGenerateImage));
  REQUIRE_OK(krea);
  CHECK(src_of(find_stage(krea->spec, "generate-image"), 1).empty());
  // Kept: both outputs tapped; then fed, the conditioner left out.
  FakeCache fc;
  auto t = vp::build_text_to_image(job, &fc.c);
  REQUIRE_OK(t);
  const auto* io = io_of(*t, "conditioning");
  REQUIRE(io);
  CHECK(!io->fed);
  REQUIRE(io->ports.size() == 2);
  CHECK(io->ports[1].name == "negative");
  const Json* tap = find_stage(t->spec, vp::kNegativeTap);
  REQUIRE(tap);
  CHECK(src_of(tap, 0) == "diffusion-conditioner" && oport_of(tap, 0) == 1);
  fc.held.insert(io->key);
  auto f = vp::build_text_to_image(job, &fc.c);
  REQUIRE_OK(f);
  CHECK(!find_stage(f->spec, "diffusion-conditioner"));
  const Json* fgen = find_stage(f->spec, "generate-image");
  REQUIRE(fgen);
  CHECK(src_of(fgen, 0) == vp::kKeptConditioning);
  CHECK(src_of(fgen, 1) == vp::kKeptNegative);
  const auto* fio = io_of(*f, "conditioning");
  REQUIRE(fio);
  CHECK(fio->fed);
  CHECK(fio->ports.size() == 2);
}
