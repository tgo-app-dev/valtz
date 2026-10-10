// The vpipe graph builder: how edit graphs are wired and sized for each
// family, straight from the built-in catalog. No engine, no models.

#include "testing.h"

#include "engine/vpipe/graph-builder.h"
#include "valtz/models/catalog.h"
#include "valtz-vpipe/exchange.h"

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <cstring>

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

// A video summary (DESIGN §4i): the clip read sparse into a valtz-source
// at the size given, the helper's model loaded as its chat loads it (its
// drafters, its warm hold), its sampler on the stage's port, Valtz's own
// words for the scenes and the whole.
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
  // Its pages as the helper's chat has them: one model warm for both.
  {
    JobSpec chat = job;
    chat.op = std::string(kOpChat);
    chat.params = {{"text", "a fox"}};
    auto cg = vp::build_chat(chat);
    REQUIRE_OK(cg);
    const Json cc = config_of(find_stage(cg->spec, "text-chat"));
    CHECK(jget(cfg, ex::kSummaryPageTokens, 0) == jget(cc, "page_tokens", -1));
    CHECK(jget(cfg, ex::kSummaryMaxPages, 0) == jget(cc, "max_pages", -1));
  }
  CHECK(g->summary_sink == "summary-sink");
  REQUIRE(find_stage(g->spec, g->summary_sink));
  CHECK(src_of(find_stage(g->spec, g->summary_sink), 0) == "summary");

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
  const Json* read = find_stage(g->spec, "read");
  const Json* write = find_stage(g->spec, "write");
  REQUIRE(read && write);
  CHECK(jget<std::string>(*read, "type", "") == "load-image");
  CHECK(jget<std::string>(config_of(read), "dtype", "") == "f16");
  CHECK(jget<std::string>(config_of(read), "alpha", "") == "drop");
  CHECK(jget<std::string>(config_of(read), "raw", "") == "rendered");
  CHECK(jget(config_of(write), "bit_depth", 0) == 16);
  CHECK(src_of(write, 1) == "read");   // the EXIF
  CHECK(!config_of(write).contains("quality"));

  job.params = {{"format", "jpeg"}, {"quality", 60}};
  auto j = vp::build_export(job);
  REQUIRE_OK(j);
  CHECK(j->output.extension() == ".jpg");
  const Json& jw = config_of(find_stage(j->spec, "write"));
  CHECK(jget<std::string>(jw, "format", "") == "jpeg");
  CHECK(jget(jw, "quality", 0) == 60);

  job.params = {{"format", "exr"}};
  auto e = vp::build_export(job);
  REQUIRE_OK(e);
  CHECK(e->output.extension() == ".exr");
  CHECK(jget<std::string>(config_of(find_stage(e->spec, "read")), "raw",
                          "") == "linear");
  CHECK(jget(*find_stage(e->spec, "write"), "iports",
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
  const Json* vr = find_stage(v->spec, "read");
  const Json* vw = find_stage(v->spec, "write");
  REQUIRE(vr && vw);
  CHECK(jget<std::string>(*vr, "type", "") == "avf-load-video");
  CHECK(jget<std::string>(config_of(vr), "alpha", "") == "keep");
  CHECK(jget<std::string>(config_of(vw), "codec", "") == "hevc");
  CHECK(!config_of(vw).contains("bitrate"));

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
  const Json& hw = config_of(find_stage(h->spec, "write"));
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
  CHECK(f.stage == "read");
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
  const Json* read = find_stage(g->spec, "read");
  const Json* write = find_stage(g->spec, "write");
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
  CHECK(jget(*find_stage(e->spec, "write"), "iports",
             Json::array()).size() == 1);
}

// MiniMax H3 text-to-video, shaped like vpipe's turbo + preview graphs:
// one config beat carries the shifts, the Turbo LoRA and the TAE (as
// FILES: both live in folders that hold others) into generate-video's
// port 9; video and sound decode apart -- the picture in F16 into HEVC
// Main10, the sound into a WAV -- and the engine joins them. Edges go UP
// to the 32-pixel grid, as vpipe takes them.
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
  CHECK(g->output.extension() == ".mp4");
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
  CHECK(jget<std::string>(config_of(save), "codec", "") == "hevc");
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

// A quantized variant made: vpipe's model-quantize from the source folder
// into the path the job names.
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
  j.params.erase("output");
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
  // Every catalog assistant samples as Qwen's card recommends without
  // thinking; the job carries it to the graph.
  for (const auto* m : cat->serving(models::Capability::PromptEnhance)) {
    if (m->role != "assistant") {
      continue;
    }
    const Json smp = jget(jget(m->engine, "vpipe", Json::object()),
                          "sampling", Json::object());
    CHECK(jget(smp, "temperature", 0.0) == 0.7);
    CHECK(jget(smp, "top_p", 0.0) == 0.8);
    CHECK(jget(smp, "top_k", 0) == 20);
    CHECK(jget(smp, "presence_penalty", 0.0) == 1.5);
  }
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
  const Json r = config_of(find_stage(g->spec, "read"));
  CHECK(std::abs(jget(r, "start_s", 0.0) - 0.5) < 1e-9);
  CHECK(jget(r, "max_frames", 0) == 24);
  CHECK(std::abs(g->audio_start - 0.5) < 1e-9);
  CHECK(std::abs(g->audio_duration - 1.0) < 1e-9);
  CHECK(g->audio_part == clip.path && g->audio_is_source);

  // A mark-out alone runs from the start; past the end, to the end.
  job.params["trim"] = {{"out", 500}, {"rate_num", 24}, {"rate_den", 1}};
  auto all = vp::build_export(job);
  REQUIRE_OK(all);
  CHECK(jget(config_of(find_stage(all->spec, "read")), "max_frames", 0) ==
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
  const Json* read = find_stage(g->spec, "read");
  REQUIRE(read);
  CHECK(jget<std::string>(*read, "type", "") == valtz::exchange::kSourceType);
  CHECK(src_of(find_stage(g->spec, "write"), 0) == "read");
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

// The second LoRA slot -- a style or identity adapter -- reaches the
// stage that takes it: H3's config stage, generate-image for pictures.
TEST(graph_builder, second_lora_slot)
{
  JobSpec j = job_for("minimax-h3-fl2va", kOpGenerateVideo);
  j.params = {{"prompt", "a kite"}, {"width", 832}, {"height", 480},
              {"frames", 39}, {"steps", 6}, {"seed", 1},
              {"lora_scale", 1.0}, {"lora2_scale", 0.6}};
  j.model.lora = "/loras/turbo.safetensors";
  j.model.lora2 = "/loras/style.safetensors";
  auto g = vp::build_video(j, std::nullopt);
  REQUIRE_OK(g);
  const Json c = config_of(find_stage(g->spec, "minimax-h3-model-config"));
  CHECK(jget<std::string>(c, "lora2", "") == "/loras/style.safetensors");
  CHECK(jget(c, "lora2_scale", 0.0) == 0.6);

  JobSpec p = job_for("krea2-turbo", kOpGenerateImage);
  p.params = {{"prompt", "a kite"}, {"width", 512}, {"height", 512},
              {"steps", 8}, {"seed", 1}, {"lora2_scale", 0.9}};
  p.model.lora2 = "/loras/m87.safetensors";
  auto pg = vp::build_text_to_image(p);
  REQUIRE_OK(pg);
  const Json ic = config_of(find_stage(pg->spec, "generate-image"));
  CHECK(jget<std::string>(ic, "lora2", "") == "/loras/m87.safetensors");
  CHECK(jget(ic, "lora2_scale", 0.0) == 0.9);
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
