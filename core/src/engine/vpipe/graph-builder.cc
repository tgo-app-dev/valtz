#include "engine/vpipe/graph-builder.h"

#include "valtz/base/log.h"
#include "valtz-vpipe/exchange.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace valtz::engine::vp {

namespace fs = std::filesystem;

namespace {

Json
port(std::string_view src, int oport = 0)
{
  return {{"src", std::string(src)}, {"oport", oport}};
}

Json
unwired()
{
  return port("", 0);
}

// Favor's acceleration options (the recipe's "tuning",
// models/tuning.h) as the generating stage spells them -- the keys of
// vpipe's acceleration vocabulary; a recipe from before them has none.
Json
accel_of(const JobSpec& job)
{
  const Json t = jget(job.params, "tuning", Json::object());
  Json out = Json::object();
  for (const char* k : {"sol_attn", "sol_tau", "sage_attn", "i8_gemm",
                        "motion_cache", "ane_ffn", "ane_qkv"}) {
    if (t.is_object() && t.contains(k)) {
      out[k] = t[k];
    }
  }
  return out;
}

// A family's OWN options (catalog `engine.vpipe.options`: an
// extension's knobs, models/tuning.h) as its stages take them: each value
// the recipe's tuning settled, into the stage the option names --
// "generate" (the default) or "config" (the family's config stage).
Json
declared_of(const JobSpec& job, std::string_view where)
{
  const Json t = jget(job.params, "tuning", Json::object());
  Json out = Json::object();
  for (const auto& o : jget(jget(job.model.engine, "vpipe", Json::object()),
                            "options", Json::array())) {
    const auto key = jget<std::string>(o, "key", "");
    if (!key.empty() &&
        jget<std::string>(o, "stage", "generate") == where &&
        t.is_object() && t.contains(key)) {
      out[key] = t[key];
    }
  }
  return out;
}

Json
stage(std::string_view id, std::string_view type, Json iports, Json config)
{
  return {{"id", std::string(id)},
          {"type", std::string(type)},
          {"iports", std::move(iports)},
          {"config", std::move(config)}};
}

// Merge `over` into `base` (shallow).
Json
merged(Json base, const Json& over)
{
  if (!base.is_object()) {
    base = Json::object();
  }
  if (over.is_object()) {
    for (auto it = over.begin(); it != over.end(); ++it) {
      base[it.key()] = it.value();
    }
  }
  return base;
}

namespace ex = valtz::exchange;

// A valtz-sink on `src`. Previews keep only the newest frame -- the
// denoise loop never waits for the UI; everything else is queued, so no
// text is lost and a slow reader back-pressures the producer instead.
// Where the decoder reads its VAE: the model's (model-select), or a
// community one of Favor's Custom -- its own model-select source, so the
// DiT stages keep pointing at the model (vae-decode's `hf_dir` doc).
std::string
vae_source(const JobSpec& job, Json& stages)
{
  if (job.model.vae.empty()) {
    return "model-select";
  }
  stages.push_back(stage("vae-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.vae.string()}}));
  return "vae-select";
}

Json
sink(std::string_view id, Json src, bool latest_only)
{
  Json cfg = {{ex::kSinkPolicy, latest_only ? ex::kSinkPolicyLatest
                                            : ex::kSinkPolicyQueue},
              {ex::kSinkDepth, latest_only ? 1 : 64}};
  return stage(id, ex::kSinkType, Json::array({std::move(src)}),
               std::move(cfg));
}

// Round to the family's alignment (`vpipe.align`, 16 unless the latent
// grid needs more: Qwen-Image-2.1 wants an even grid, i.e. 32).
int
round_to(int v, int align)
{
  return std::max(align, (v + align / 2) / align * align);
}

// Up to the next multiple of `align`.
int
ceil_to(int v, int align)
{
  return std::max(align, (v + align - 1) / align * align);
}

// tensor-list's declared item ports: the most pictures one list holds.
constexpr int kMaxListItems = 10;

// The most pixels a picture is fed at: past this it is shrunk while it
// is decoded (memory), and the graph's Lanczos resample does the rest.
constexpr double kMaxFeedArea = 16.0 * 1024 * 1024;

// The picture a feed hands over: the crop's canvas when the picture is
// cropped (at the picture's own scale), else the picture.
media::PixelSize
picture_size(media::PixelSize own, const media::Crop& crop)
{
  if (crop.identity()) {
    return own;
  }
  const auto p = media::crop_placement(crop, own.width, own.height);
  return {std::max(1, static_cast<int>(std::lround(p.canvas_w))),
          std::max(1, static_cast<int>(std::lround(p.canvas_h)))};
}

// `sz`'s aspect at `area` pixels, each axis floored to `align`: floored
// so that the area never exceeds a budget the model enforces.
media::PixelSize
fit_area(media::PixelSize sz, double area, int align)
{
  const double k = std::sqrt(area / (static_cast<double>(sz.width) *
                                     sz.height));
  auto axis = [&](double v) {
    return std::max(align, static_cast<int>(v * k) / align * align);
  };
  return {axis(sz.width), axis(sz.height)};
}

// Text-to-image and edit share one graph. An edit adds, per picture, a
// valtz-source the engine fills at the picture's own size, an
// `image-resample` that fits it (Lanczos), a vae-encode of the fitted
// picture for the DiT, and -- where the family's text encoder sees
// pictures -- the SAME fitted picture into the conditioner. One resize
// feeds both: Qwen-Image-2.1's DiT drops a reference whose latent grid
// is not exactly twice the conditioner's merged grid.
Result<BuiltGraph>
build_image(const JobSpec& job, const std::vector<RefImage>& refs)
{
  const bool edit = job.op == kOpEditImage;
  const Json vp = jget(job.model.engine, "vpipe", Json::object());
  const Json ed = jget(vp, "edit", Json());
  if (!vp.is_object() || vp.empty() || (edit && !ed.is_object())) {
    return make_error(Code::Unsupported, std::format(
        "{} has no vpipe recipe for {}", job.model.id, job.op));
  }
  std::string prompt = jget<std::string>(job.params, "prompt", "");
  if (prompt.empty()) {
    return make_error(Code::InvalidArgument, "prompt is empty");
  }
  if (edit && refs.empty()) {
    return make_error(Code::InvalidArgument,
                      "an edit needs a reference picture");
  }
  const Json defaults = jget(vp, "defaults", Json::object());
  const int align = std::max(16, jget(vp, "align", 16));
  const bool has_base = edit && refs[0].base;
  int width = jget(job.params, "width", 0);
  int height = jget(job.params, "height", 0);
  if (width <= 0 || height <= 0) {
    const int dw = jget(defaults, "width", 1024);
    const int dh = jget(defaults, "height", 1024);
    if (has_base) {
      // The base's shape at the model's default area.
      auto sz = fit_area(refs[0].size, static_cast<double>(dw) * dh, align);
      width = sz.width;
      height = sz.height;
    } else {
      // No base: a picture composed from the references, at the model's
      // own size (or the one asked for).
      width = dw;
      height = dh;
    }
  }
  width = round_to(width, align);
  height = round_to(height, align);
  int steps = jget(job.params, "steps", jget(defaults, "steps", 8));
  auto seed = jget<std::int64_t>(job.params, "seed", 0);

  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.png", jid);

  // Pictures go in as ONE LIST on each side (vpipe's tensor-list, after
  // each picture's own resample), so the count is the model's
  // `max_references`, up to what tensor-list declares.
  std::vector<Json> fits;  // each picture's image-resample config
  if (edit) {
    const int max_refs = std::clamp(jget(ed, "max_references", 1), 1,
                                    kMaxListItems);
    const std::size_t nref = std::min<std::size_t>(refs.size(), max_refs);
    if (refs.size() > nref) {
      VALTZ_LOG_WARN("engine", "{} takes {} picture(s) here; using the "
                     "first {} of {}", job.model.id, max_refs, nref,
                     refs.size());
    }
    const double budget = jget(ed, "reference_area", 1024.0 * 1024.0);
    for (std::size_t i = 0; i < nref; ++i) {
      const auto& r = refs[i];
      RefFeed f;
      f.stage = std::format("ref-{}", i);
      f.path = r.path;
      f.base = r.base && i == 0;
      media::PixelSize pic = r.size;
      if (f.base) {
        f.adjust = media::adjustments_from_json(
            jget(job.params, "base_adjust", Json::object()));
        f.crop = media::crop_from_json(
            jget(job.params, "base_crop", Json::object()));
        pic = picture_size(r.size, f.crop);
      }
      // `image-resample` reads planar RGB; alpha is not carried.
      f.channels = 3;
      const double own = static_cast<double>(pic.width) * pic.height;
      f.size = own > kMaxFeedArea ? fit_area(pic, kMaxFeedArea, 1) : pic;
      Json fit;
      if (f.base) {
        // The base becomes the output's frame: fill it, centre-crop the
        // (alignment-sized) difference.
        f.fitted = {width, height};
        fit = {{"width", width}, {"height", height}, {"fit", "crop"},
               {"algorithm", "lanczos"}};
      } else {
        // A reference keeps its pixels, padded right and bottom to the
        // grid; only one larger than the model's reference area is
        // shrunk first (same aspect).
        const double area =
            static_cast<double>(f.size.width) * f.size.height;
        const double k = area > budget ? std::sqrt(budget / area) : 1.0;
        const int nw = std::max(1, static_cast<int>(f.size.width * k));
        const int nh = std::max(1, static_cast<int>(f.size.height * k));
        f.fitted = {ceil_to(nw, align), ceil_to(nh, align)};
        fit = {{"width", f.fitted.width}, {"height", f.fitted.height},
               {"fit", "manual"}, {"scale", k}, {"src_x", 0},
               {"src_y", 0}, {"algorithm", "lanczos"}};
      }
      fits.push_back(std::move(fit));
      g.references.push_back(std::move(f));
    }
  }
  const bool ref_to_cond = edit && jget(ed, "reference_to_conditioner",
                                        false);

  Json stages = Json::array();
  stages.push_back(stage("model-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.dir.string()}}));
  stages.push_back(stage("text-prompt", "text-prompt", Json::array(),
                         {{"text", prompt}}));
  // Per picture a source and its fit; then ONE list of the fitted
  // pictures, which both the conditioner and one vae-encode read -- so
  // latent k is always made from the picture the conditioner sees as k.
  const bool have_refs = !g.references.empty();
  Json items = Json::array();
  for (std::size_t i = 0; i < g.references.size(); ++i) {
    const auto& f = g.references[i];
    stages.push_back(stage(f.stage, ex::kSourceType, Json::array(),
                           Json::object()));
    stages.push_back(stage(f.stage + "-fit", "image-resample",
                           Json::array({port(f.stage)}), fits[i]));
    items.push_back(port(f.stage + "-fit"));
  }
  if (have_refs) {
    stages.push_back(stage(kRefList, "tensor-list", items, Json::object()));
    // vae-encode: 0 image (unwired), 1 model, 2 images (the list); its
    // oport 1 is the latents list.
    stages.push_back(stage(kRefLatents, "vae-encode",
                           Json::array({unwired(), port("model-select"),
                                        port(kRefList)}),
                           Json::object()));
  }

  // The family's config source carries the preview keys.
  std::string cfg_stage = jget<std::string>(vp, "config_stage", "");
  if (!cfg_stage.empty()) {
    Json cfg = jget(vp, "config", Json::object());
    if (edit) {
      cfg = merged(cfg, jget(ed, "config", Json::object()));
    }
    if (ref_to_cond && jget(ed, "vl_bounds", false) &&
        !g.references.empty()) {
      // The conditioner's vision tower resizes on its own: down to
      // `vl_pixel_budget` (1024^2 by default) and `vl_max_pixels`, up to
      // `vl_min_pixels` (256^2). Bounded around the fitted pictures, it
      // reads them as they are -- the same pixels as the VAE.
      std::int64_t most = 0;
      std::int64_t least = std::numeric_limits<std::int64_t>::max();
      for (const auto& f : g.references) {
        const std::int64_t a =
            static_cast<std::int64_t>(f.fitted.width) * f.fitted.height;
        most = std::max(most, a);
        least = std::min(least, a);
      }
      cfg["vl_pixel_budget"] = most;
      cfg["vl_max_pixels"] = most;
      cfg["vl_min_pixels"] = std::min<std::int64_t>(least, 65536);
    }
    if (!job.model.preview.empty() && jget(job.params, "preview", true)) {
      cfg["preview_vae"] = job.model.preview.string();
      cfg["preview_every"] = jget(job.params, "preview_every", 1);
      cfg["preview_max_edge"] = jget(job.params, "preview_max_edge", 512);
      g.preview_sink = "preview-sink";
    }
    cfg = merged(cfg, declared_of(job, "config"));
    stages.push_back(stage(cfg_stage, cfg_stage, Json::array(), cfg));
  }

  Json sched = jget(vp, "scheduler", Json());
  bool have_sched = sched.is_object();
  if (have_sched) {
    sched["steps"] = steps;
    // Favor's Custom may move the shift.
    const Json t = jget(job.params, "tuning", Json::object());
    if (t.is_object() && t.contains("shift") && t["shift"].is_number()) {
      sched["shift"] = t["shift"];
    }
    stages.push_back(stage("scheduler-select", "scheduler-select",
                           Json::array(), sched));
  }
  // diffusion-conditioner: 0 prompt, 1 negative, 2 model, 3/4 single
  // reference images (unused: the list carries them all), 5
  // model_config, 6 ref_images -- the list, where the family's text
  // encoder sees pictures (a VLM).
  Json cond_in = Json::array({port("text-prompt"), unwired(),
                              port("model-select")});
  if (!cfg_stage.empty() || ref_to_cond) {
    cond_in.push_back(unwired());
    cond_in.push_back(unwired());
    cond_in.push_back(cfg_stage.empty() ? unwired() : port(cfg_stage));
    if (ref_to_cond && have_refs) {
      cond_in.push_back(port(kRefList));
    }
  }
  stages.push_back(stage("diffusion-conditioner", "diffusion-conditioner",
                         cond_in, Json::object()));

  // generate-image: 0 cond, 1 neg, 2 model, 3 sampler, 4 scheduler,
  // 5/6 single ref latents (unused), 7 model_config, 8 ref_latents --
  // the list, from vae-encode's oport 1.
  Json gen_in = Json::array({port("diffusion-conditioner"), unwired(),
                             port("model-select"), unwired(),
                             have_sched ? port("scheduler-select")
                                        : unwired()});
  if (!cfg_stage.empty() || edit) {
    gen_in.push_back(unwired());
    gen_in.push_back(unwired());
    gen_in.push_back(cfg_stage.empty() ? unwired() : port(cfg_stage));
    if (have_refs) {
      gen_in.push_back(port(kRefLatents, 1));
    }
  }
  Json gen_cfg = {{"height", height}, {"width", width}, {"steps", steps},
                  {"seed", seed}};
  gen_cfg = merged(gen_cfg, accel_of(job));
  // A community DiT in place of the model's (Favor's Custom).
  if (!job.model.dit.empty()) {
    gen_cfg["dit_dir"] = job.model.dit.string();
  }
  // Run-time LoRAs (Favor's Custom): generate-image takes both slots for
  // every image family.
  if (!job.model.lora.empty()) {
    gen_cfg["lora"] = job.model.lora.string();
    gen_cfg["lora_scale"] = jget(job.params, "lora_scale", 1.0);
  }
  if (!job.model.lora2.empty()) {
    gen_cfg["lora2"] = job.model.lora2.string();
    gen_cfg["lora2_scale"] = jget(job.params, "lora2_scale", 1.0);
  }
  gen_cfg = merged(gen_cfg, declared_of(job, "generate"));
  gen_cfg = merged(gen_cfg, jget(job.params, "vpipe_generate",
                                 Json::object()));
  stages.push_back(stage("generate-image", "generate-image", gen_in,
                         gen_cfg));

  if (!g.preview_sink.empty()) {
    stages.push_back(sink(g.preview_sink, port("generate-image", 2),
                          /*latest_only=*/true));
  }
  // The decoder's samples kept (F16) and written as a 16-bit PNG: a
  // result is as smooth as the model made it, not banded to 8 bits.
  const std::string vae = vae_source(job, stages);
  stages.push_back(stage("vae-decode", "vae-decode",
                         Json::array({port("generate-image"), port(vae)}),
                         {{"dtype", "f16"}}));
  // save-image: 0 the image, 1 metadata -- the base's EXIF, carried to
  // the result. Software names Valtz, with vpipe's own line in brackets.
  Json save_in = Json::array({port("vae-decode")});
  for (const auto& f : g.references) {
    if (f.base) {
      g.exif_from = f.path;
      stages.push_back(stage(kExifSource, ex::kSourceType, Json::array(),
                             Json::object()));
      save_in.push_back(port(kExifSource));
      break;
    }
  }
  stages.push_back(stage("save-image", "save-image", std::move(save_in),
                         {{"path", g.output.string()},
                          {"bit_depth", 16},
                          {"software_host",
                           std::string("Valtz ") + VALTZ_VERSION}}));

  g.width = width;
  g.height = height;
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

}

Result<BuiltGraph>
build_text_to_image(const JobSpec& job)
{
  return build_image(job, {});
}

Result<BuiltGraph>
build_image_edit(const JobSpec& job, const std::vector<RefImage>& refs)
{
  return build_image(job, refs);
}

// The shape of vpipe's MiniMax H3 reference graphs
// (docs/pipelines/minimax-h3-text-to-video-turbo / -preview /
// -first-last-to-video), with the output written Apple-native:
//
//   text-prompt -> diffusion-conditioner -0-> generate-video
//   [first -> first-fit -> vae-encode] -5-> generate-video
//   <family>-model-config -9-> generate-video
//   generate-video -0-> vae-decode (F16) -> avf-save-video (HEVC 10-bit)
//                  -1-> audio-vae-decode -> save-audio (WAV)
//                  -2-> preview-sink
Result<BuiltGraph>
build_video(const JobSpec& job, const std::optional<RefImage>& first,
            const std::vector<RefMedia>& refs)
{
  const Json vp = jget(job.model.engine, "vpipe", Json::object());
  std::string cfg_stage = jget<std::string>(vp, "config_stage", "");
  if (!vp.is_object() || vp.empty() || cfg_stage.empty()) {
    return make_error(Code::Unsupported, std::format(
        "{} has no vpipe recipe for {}", job.model.id, job.op));
  }
  if (first && !jget(vp, "first_frame", false)) {
    return make_error(Code::Unsupported, std::format(
        "{} cannot start a clip from a picture", job.model.id));
  }
  std::string prompt = jget<std::string>(job.params, "prompt", "");
  if (prompt.empty()) {
    return make_error(Code::InvalidArgument, "prompt is empty");
  }
  const Json defaults = jget(vp, "defaults", Json::object());
  // vpipe rounds a clip's edges UP to the grid (and logs it); so does
  // this, so the first frame is fitted to the frame the model makes.
  const int align = std::max(16, jget(vp, "align", 32));
  const int width = ceil_to(
      jget(job.params, "width", jget(defaults, "width", 960)), align);
  const int height = ceil_to(
      jget(job.params, "height", jget(defaults, "height", 544)), align);
  const int frames = jget(job.params, "frames", jget(defaults, "frames", 124));
  const double fps = jget(job.params, "fps", jget(defaults, "fps", 24.0));
  const int steps = jget(job.params, "steps", jget(defaults, "steps", 8));
  const auto seed = jget<std::int64_t>(job.params, "seed", 0);

  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.mp4", jid);
  g.video_part = job.output_dir / std::format("{}-picture.mp4", jid);
  g.audio_part = job.output_dir / std::format("{}-sound.wav", jid);
  g.width = width;
  g.height = height;

  const Json rv = jget(vp, "references", Json());
  if (!refs.empty() && !rv.is_object()) {
    return make_error(Code::Unsupported, std::format(
        "{} does not read references", job.model.id));
  }
  if (!refs.empty() && first) {
    return make_error(Code::InvalidArgument,
                      "a clip opens on a picture or reads references");
  }

  Json stages = Json::array();
  stages.push_back(stage("model-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.dir.string()}}));
  stages.push_back(stage("text-prompt", "text-prompt", Json::array(),
                         {{"text", prompt}}));
  // What conditions the denoise, and the reference rows when there are
  // references.
  Json cond = port("diffusion-conditioner");
  Json ref_video = unwired(), ref_audio = unwired();
  // The first sound reference, when the clip keeps its sound.
  const RefMedia* song = nullptr;
  if (refs.empty()) {
    // The prompt encoder (Qwen3-VL-32B for H3) streams, and is dropped
    // before the denoise: the two never share the machine.
    stages.push_back(stage("diffusion-conditioner", "diffusion-conditioner",
                           Json::array({port("text-prompt"), unwired(),
                                        port("model-select")}),
                           jget(vp, "conditioner", Json::object())));
  } else {
    // vpipe's video-ref-encoder: the prompt and the references, read in
    // order -- its `references` list first, then its six ports, which is
    // the order the job lists them in (the controller's).
    Json files = Json::array();
    Json enc_in = Json::array({port("text-prompt"), port("model-select")});
    Json attach = Json::array();
    int ref = 0;  // reference ports used, 1..6
    for (const auto& r : refs) {
      if (r.kind == "audio" && !song) {
        song = &r;
      }
      if (r.list) {
        files.push_back(r.path.string());
        continue;
      }
      if (r.kind == "video") {
        const std::string id = std::format("ref{}", ++ref);
        stages.push_back(stage(id + "-read", "load-video", Json::array(),
                               {{"input_url", r.path.string()},
                                {"enable_audio", false}}));
        stages.push_back(stage(id + "-rgb", "video-to-rgb",
                               Json::array({port(id + "-read")}),
                               {{"output_dtype", "u8"}}));
        // Exact to the frame (a seek lands on a keyframe).
        std::string last = id + "-rgb";
        if (r.first > 0 || r.count > 0) {
          Json slice = {{"start", r.first}};
          if (r.count > 0) {
            slice["end"] = r.first + r.count;
          }
          stages.push_back(stage(id + "-span", "temporal-slice",
                                 Json::array({port(last)}), slice));
          last = id + "-span";
        }
        // The whole span in one beat: its bytes, with room.
        const double mb = static_cast<double>(std::max<std::int64_t>(
                              r.count > 0 ? r.count : 362, 1)) *
                          3.0 * std::max(r.width, 1) *
                          std::max(r.height, 1) / (1024.0 * 1024.0);
        stages.push_back(stage(id, "temporal-stack",
                               Json::array({port(last)}),
                               {{"mode", "video"}, {"group_size", 0},
                                {"max_mb", std::max(256, static_cast<int>(
                                                             mb) + 64)}}));
        enc_in.push_back(port(id));
        if (!r.audio) {
          continue;
        }
      }
      // A sound on a port: a clip's own (attached to it), or a trimmed
      // one -- at the audio VAE's rate, stereo.
      const std::string id = std::format("ref{}", ++ref);
      Json read = {{"input_url", r.path.string()}};
      if (r.start_s > 0) {
        read["start_s"] = r.start_s;
      }
      if (r.duration_s > 0) {
        read["duration_s"] = r.duration_s;
      }
      stages.push_back(stage(id + "-read", "load-audio", Json::array(),
                             read));
      stages.push_back(stage(id + "-pcm", "audio-to-pcm",
                             Json::array({port(id + "-read")}),
                             {{"output_sample_rate", 32000},
                              {"channels", 2},
                              {"chunk_duration_s",
                               r.duration_s > 0 ? r.duration_s : 30.0},
                              {"max_chunk_duration_s", 60.0}}));
      stages.push_back(stage(id, "temporal-stack",
                             Json::array({port(id + "-pcm")}),
                             {{"mode", "audio"}, {"group_size", 0}}));
      enc_in.push_back(port(id));
      if (r.kind == "video") {
        attach.push_back(ref);
      }
    }
    if (ref > jget(rv, "ports", 6)) {
      return make_error(Code::InvalidArgument, std::format(
          "{} reference ports wanted, {} there", ref, jget(rv, "ports", 6)));
    }
    Json enc = merged({{"frames", frames}},
                      jget(rv, "encoder", Json::object()));
    if (!files.empty()) {
      enc["references"] = files;
    }
    if (!attach.empty()) {
      enc["attach_audio"] = attach;
    }
    stages.push_back(stage("video-ref-encoder", "video-ref-encoder",
                           std::move(enc_in), std::move(enc)));
    cond = port("video-ref-encoder", 0);
    ref_video = port("video-ref-encoder", 1);
    ref_audio = port("video-ref-encoder", 2);
  }
  const bool keep_song = song && jget(job.params, "reference_sound", false);

  // The family's own knobs -- its sigma shifts, the run-time adapter, the
  // preview TAE -- ride one config beat into generate-video's port 9.
  Json cfg = jget(vp, "config", Json::object());
  if (!job.model.lora.empty()) {
    cfg["lora"] = job.model.lora.string();
    cfg["lora_scale"] = jget(job.params, "lora_scale", 1.0);
  }
  if (!job.model.lora2.empty()) {
    cfg["lora2"] = job.model.lora2.string();
    cfg["lora2_scale"] = jget(job.params, "lora2_scale", 1.0);
  }
  // Favor's Custom: the two schedules' shifts (whole numbers, sent as the
  // reals the stage declares), and the VDN branch beside the DiT (its
  // folder).
  {
    const Json t = jget(job.params, "tuning", Json::object());
    for (const char* k : {"video_shift", "audio_shift"}) {
      if (t.is_object() && t.contains(k) && t[k].is_number()) {
        cfg[k] = t[k].get<double>();
      }
    }
    // TaoMate's method, said either way: its adapter in the first slot
    // runs the streaming method when on, and nothing else ever does.
    if (t.is_object() && t.contains("taomate") &&
        t["taomate"].is_boolean()) {
      cfg["taomate"] = t["taomate"].get<bool>() ? "on" : "off";
    }
  }
  if (!job.model.branch.empty()) {
    cfg["linear_branch"] = job.model.branch.string();
  }
  if (!job.model.preview.empty() && jget(job.params, "preview", true)) {
    cfg["preview_vae"] = job.model.preview.string();
    cfg["preview_every"] = jget(job.params, "preview_every", 1);
    cfg["preview_max_edge"] = jget(job.params, "preview_max_edge", 512);
    g.preview_sink = "preview-sink";
  }
  cfg = merged(cfg, declared_of(job, "config"));
  stages.push_back(stage(cfg_stage, cfg_stage, Json::array(), cfg));

  // The picture the clip opens on: decoded into a lease at its own size
  // (capped), cropped with Lanczos to fill the frame exactly -- vpipe
  // drops an anchor of another size, and the clip silently becomes
  // text-to-video -- and VAE-encoded as the first keyframe.
  Json anchor = unwired();
  if (first) {
    RefFeed f;
    f.stage = "first";
    f.path = first->path;
    f.base = true;
    f.channels = 3;
    f.adjust = media::adjustments_from_json(
        jget(job.params, "base_adjust", Json::object()));
    f.crop = media::crop_from_json(
        jget(job.params, "base_crop", Json::object()));
    const media::PixelSize pic = picture_size(first->size, f.crop);
    const double own = static_cast<double>(pic.width) * pic.height;
    f.size = own > kMaxFeedArea ? fit_area(pic, kMaxFeedArea, 1) : pic;
    f.fitted = {width, height};
    stages.push_back(stage(f.stage, ex::kSourceType, Json::array(),
                           Json::object()));
    stages.push_back(stage("first-fit", "image-resample",
                           Json::array({port(f.stage)}),
                           {{"width", width}, {"height", height},
                            {"fit", "crop"}, {"algorithm", "lanczos"}}));
    stages.push_back(stage("first-latent", "vae-encode",
                           Json::array({port("first-fit"),
                                        port("model-select")}),
                           {{"unload_when_idle", "always"}}));
    anchor = port("first-latent");
    g.references.push_back(std::move(f));
  }

  // generate-video: 0 cond, 1 neg (inert: H3 is guidance-distilled),
  // 2 model, 3 sampler, 4 scheduler (H3 brings its own), 5 / 6 first /
  // last keyframe, 7 / 8 Ref2VA rows, 9 model_config.
  Json gen_in = Json::array({cond, unwired(), port("model-select"),
                             unwired(), unwired(), anchor, unwired(),
                             ref_video, ref_audio, port(cfg_stage)});
  Json gen_cfg = {{"width", width}, {"height", height}, {"frames", frames},
                  {"fps", fps}, {"steps", steps}, {"seed", seed}};
  gen_cfg = merged(gen_cfg, jget(vp, "generate", Json::object()));
  gen_cfg = merged(gen_cfg, accel_of(job));
  // A community DiT in place of the model's (Favor's Custom): a single
  // .safetensors (FP8 kept FP8) or a transformer folder.
  if (!job.model.dit.empty()) {
    gen_cfg["dit_dir"] = job.model.dit.string();
  }
  gen_cfg = merged(gen_cfg, declared_of(job, "generate"));
  gen_cfg = merged(gen_cfg, jget(job.params, "vpipe_generate",
                                 Json::object()));
  stages.push_back(stage("generate-video", "generate-video", gen_in,
                         gen_cfg));
  if (!g.preview_sink.empty()) {
    stages.push_back(sink(g.preview_sink, port("generate-video", 2),
                          /*latest_only=*/true));
  }

  // The picture: decoded in F16 and written as HEVC Main10 -- ten bits,
  // not the eight the FFmpeg path rounds to -- tagged BT.709, which is
  // what the model's sRGB-encoded BT.709 samples are taken as in video.
  const std::string vae = vae_source(job, stages);
  stages.push_back(stage("vae-decode", "vae-decode",
                         Json::array({port("generate-video"), port(vae)}),
                         {{"dtype", "f16"}}));
  stages.push_back(stage("save-video", "avf-save-video",
                         Json::array({port("vae-decode")}),
                         {{"path", g.video_part.string()},
                          {"codec", "hevc"},
                          {"fps", fps},
                          {"quality", jget(vp, "quality", 0.8)},
                          {"color_primaries", 1},
                          {"color_transfer", 1},
                          {"color_matrix", 1}}));
  if (keep_song) {
    // The soundtrack is the song as it is, over the clip's length -- not
    // a generated one, so none is decoded.
    g.audio_part = song->path;
    g.audio_is_source = true;
    g.audio_start = song->start_s;
    g.audio_duration = static_cast<double>(frames) / fps;
  } else {
    // The soundtrack (32 kHz stereo for H3), as a 16-bit WAV.
    stages.push_back(stage("audio-vae-decode", "audio-vae-decode",
                           Json::array({port("generate-video", 1),
                                        port("model-select")}),
                           Json::object()));
    stages.push_back(stage("save-audio", "save-audio",
                           Json::array({port("audio-vae-decode")}),
                           {{"output_path", g.audio_part.string()},
                            {"format", "wav"}}));
  }

  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

// vpipe's YuE2 graphs, with the song written as a WAV and its score read
// back in memory:
//
//   [text-prompt (the lyrics)] -0-> generate-audio
//   generate-audio -0-> audio-vae-decode (the decoder) -> save-audio (WAV)
//                  -1-> score-sink
Result<BuiltGraph>
build_audio(const JobSpec& job)
{
  const Json vp = jget(job.model.engine, "vpipe", Json::object());
  if (!vp.is_object() || vp.empty()) {
    return make_error(Code::Unsupported, std::format(
        "{} has no vpipe recipe for {}", job.model.id, job.op));
  }
  if (job.model.vae.empty()) {
    return make_error(Code::Unsupported, std::format(
        "{} has no decoder", job.model.id));
  }
  const std::string style = jget<std::string>(job.params, "style", "");
  const std::string lyrics = jget<std::string>(job.params, "lyrics", "");
  if (style.empty() && lyrics.empty()) {
    return make_error(Code::InvalidArgument,
                      "a song needs a style or lyrics");
  }
  const Json defaults = jget(vp, "defaults", Json::object());
  const std::string cot = jget<std::string>(
      job.params, "cot", jget<std::string>(defaults, "cot", "full"));
  if (cot != "full" && cot != "melody" && cot != "off") {
    return make_error(Code::InvalidArgument, std::format(
        "'{}' is not a score plan (full, melody or off)", cot));
  }
  const std::string abc = jget<std::string>(job.params, "abc", "");
  if (!abc.empty() && cot == "off") {
    return make_error(Code::InvalidArgument,
                      "a score to follow needs a plan (full or melody)");
  }

  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.wav", jid);
  g.score_sink = "score-sink";

  Json stages = Json::array();
  Json gen_in = Json::array();
  if (!lyrics.empty()) {
    // One beat, the lyrics: the songs-from-lyrics shape (a plain string
    // beat IS the lyrics; the style stays the config's).
    stages.push_back(stage("lyrics", "text-prompt", Json::array(),
                           {{"text", lyrics}}));
    gen_in.push_back(port("lyrics"));
  }
  Json gen_cfg = {{"hf_dir", job.model.dir.string()},
                  {"style", style},
                  {"cot", cot},
                  {"seed", jget<std::int64_t>(job.params, "seed", 0)},
                  {"ode_steps", jget(job.params, "steps",
                                     jget(defaults, "steps", 32))}};
  if (!abc.empty()) {
    gen_cfg["abc"] = abc;
  }
  if (const double s = jget(job.params, "max_seconds", 0.0); s > 0) {
    gen_cfg["max_seconds"] = s;
  }
  // Favor's tiers, those the stage has (the catalog's `accel`): the
  // flow matching's, under the image and video stages' own names.
  {
    const Json all = accel_of(job);
    const Json takes = jget(vp, "accel", Json::array());
    for (auto it = all.begin(); it != all.end(); ++it) {
      const std::string k = it.key() == "sol_tau" ? "sol_attn" : it.key();
      if (std::ranges::find(takes, Json(k)) != takes.end()) {
        gen_cfg[it.key()] = it.value();
      }
    }
  }
  gen_cfg = merged(gen_cfg, declared_of(job, "generate"));
  gen_cfg = merged(gen_cfg, jget(job.params, "vpipe_generate",
                                 Json::object()));
  stages.push_back(stage("generate-audio", "generate-audio",
                         std::move(gen_in), std::move(gen_cfg)));
  // The decoder over the same latents: the model's own, or another
  // (YuE2's legacy one) in its place.
  stages.push_back(stage("audio-vae-decode", "audio-vae-decode",
                         Json::array({port("generate-audio")}),
                         {{"hf_dir", job.model.vae.string()}}));
  // 16 bits at the decoder's rate: what it made, kept whole.
  stages.push_back(stage("save-audio", "save-audio",
                         Json::array({port("audio-vae-decode")}),
                         {{"output_path", g.output.string()},
                          {"format", "wav"}}));
  stages.push_back(sink(g.score_sink, port("generate-audio", 1),
                        /*latest_only=*/false));
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

// SPEECH (MOSS-TTS, vpipe's moss-tts-v1.5-speak graph): the words in as
// one beat, spoken in one utterance and written as a WAV:
//
//   text-prompt (the words) -0-> text-to-speech -0-> save-audio (WAV)
//   [voice-read -> voice-pcm -> voice (one beat)] -1-> text-to-speech
//   sampler (the seed, MOSS's own sampling) -2-> text-to-speech
//
// The voice is read from its file by vpipe's own stages -- files in --
// mono at the codec's rate, stacked into ONE beat (the stage keeps the
// latest reference beat: a voice in chunks would clone its last one),
// and the stage waits for it before it speaks (wait_for_reference).
// One-shot (stream_chunk_frames 0): one PCM beat, so one file.
Result<BuiltGraph>
build_speech(const JobSpec& job)
{
  const Json vp = jget(job.model.engine, "vpipe", Json::object());
  const Json sp = jget(vp, "speech", Json());
  if (!sp.is_object()) {
    return make_error(Code::Unsupported, std::format(
        "{} has no vpipe recipe for {}", job.model.id, job.op));
  }
  if (job.model.vae.empty()) {
    return make_error(Code::Unsupported, std::format(
        "{} has no audio codec", job.model.id));
  }
  const std::string text = jget<std::string>(job.params, "text", "");
  if (text.empty()) {
    return make_error(Code::InvalidArgument, "speech needs its words");
  }
  const double rate = jget(sp, "sample_rate", 24000.0);
  const double tps = jget(sp, "tokens_per_second", 12.5);

  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.wav", jid);

  Json stages = Json::array();
  stages.push_back(stage("text", "text-prompt", Json::array(),
                         {{"text", text}}));
  Json tts_in = Json::array({port("text")});
  const JobInput* voice = nullptr;
  for (const auto& in : job.inputs) {
    if (in.role == "voice") {
      voice = &in;
    }
  }
  if (voice) {
    // Read where its span starts, no further than the stage keeps.
    const double keep = jget(sp, "voice_seconds", 12.0);
    const double start = jget(job.params, "voice_start", 0.0);
    double secs = jget(job.params, "voice_seconds", 0.0);
    secs = secs > 0 ? std::min(secs, keep) : keep;
    Json read = {{"input_url", voice->path.string()},
                 {"duration_s", secs}};
    if (start > 0) {
      read["start_s"] = start;
    }
    stages.push_back(stage("voice-read", "load-audio", Json::array(),
                           std::move(read)));
    stages.push_back(stage("voice-pcm", "audio-to-pcm",
                           Json::array({port("voice-read")}),
                           {{"output_sample_rate", static_cast<int>(rate)},
                            {"channels", 1},
                            {"chunk_duration_s", secs},
                            {"max_chunk_duration_s", secs + 1.0}}));
    stages.push_back(stage("voice", "temporal-stack",
                           Json::array({port("voice-pcm")}),
                           {{"mode", "audio"}, {"group_size", 0}}));
    tts_in.push_back(port("voice"));
  } else {
    tts_in.push_back(nullptr);  // the reference slot, unwired
  }
  // MOSS's own audio sampling (greedy would loop in silence), seeded.
  stages.push_back(stage("sampler", "sampler-select", Json::array(),
                         {{"temperature", 1.7},
                          {"top_p", 0.8},
                          {"top_k", 25},
                          {"seed", jget<std::int64_t>(job.params, "seed",
                                                      0)}}));
  tts_in.push_back(port("sampler"));
  // Long enough for the duration asked for, else the catalog's longest.
  const int tokens = std::max(0, jget(job.params, "duration_tokens", 0));
  const double most = jget(sp, "max_seconds", 300.0);
  const int budget = std::max(
      jget(job.params, "max_new_tokens", 0),
      tokens > 0 ? tokens * 3 / 2 + 64
                 : static_cast<int>(std::ceil(most * tps)));
  // How the LM is held: Favor's 8-bit weights (Fast, Med) or its bf16
  // (Fine); an 8-bit pack loads as it sits either way.
  const Json tuning = jget(job.params, "tuning", Json::object());
  std::string lm_quant = jget<std::string>(
      job.params, "lm_quant", jget<std::string>(sp, "lm_quant", "w8"));
  if (tuning.is_object() && tuning.contains("w8_weights")) {
    lm_quant = jget(tuning, "w8_weights", true) ? "w8" : "bf16";
  }
  Json cfg = {{"hf_dir", job.model.dir.string()},
              {"codec_dir", job.model.vae.string()},
              {"max_new_tokens", budget},
              {"duration_tokens", tokens},
              {"stream_chunk_frames", 0},
              {"interrupt_on_new_text", false},
              {"wait_for_reference", voice != nullptr},
              {"lm_quant", lm_quant}};
  for (const char* k : {"instruction", "quality", "sound_event",
                        "ambient_sound", "language"}) {
    const std::string v = jget<std::string>(job.params, k, "");
    if (!v.empty()) {
      cfg[k] = v;
    }
  }
  // Favor's tiers the stage has (the catalog's `accel`).
  {
    const Json all = accel_of(job);
    const Json takes = jget(vp, "accel", Json::array());
    for (auto it = all.begin(); it != all.end(); ++it) {
      if (std::ranges::find(takes, Json(it.key())) != takes.end()) {
        cfg[it.key()] = it.value();
      }
    }
  }
  cfg = merged(cfg, declared_of(job, "speak"));
  stages.push_back(stage("speak", "text-to-speech", std::move(tts_in),
                         std::move(cfg)));
  stages.push_back(stage("save-audio", "save-audio",
                         Json::array({port("speak")}),
                         {{"output_path", g.output.string()},
                          {"format", "wav"},
                          {"sample_rate", static_cast<int>(rate)}}));
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

// vpipe's model-quantize (prepare-moss-tts-v1.5's last stage): the source
// folder in, an affine-quantized pack out, where the job says -- a path,
// so vpipe neither infers nor registers it.
Result<BuiltGraph>
build_quantize_model(const JobSpec& job)
{
  const std::string out = jget<std::string>(job.params, "output", "");
  if (job.model.dir.empty() || out.empty()) {
    return make_error(Code::InvalidArgument,
                      "a quantize needs its source and its output");
  }
  BuiltGraph g;
  Json stages = Json::array();
  stages.push_back(stage("quantize", "model-quantize", Json::array(),
                         {{"src_model", job.model.dir.string()},
                          {"output_name", out},
                          {"bits", jget(job.params, "bits", 8)},
                          {"group_size", jget(job.params, "group_size",
                                              64)},
                          {"skip_existing", false}}));
  g.spec = {{"id", "valtz-" + job.id.str()},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

Result<BuiltGraph>
build_chat(const JobSpec& job)
{
  std::string text = jget<std::string>(job.params, "text", "");
  if (text.empty()) {
    return make_error(Code::InvalidArgument, "chat text is empty");
  }
  // Pictures to look at go ahead of the words, in order, as vpipe's
  // media-line markers (text-chat reads the file through the model's
  // vision tower; common/media-line.h): "<image1>" is the first one.
  std::string pictures;
  for (const auto& in : job.inputs) {
    if (in.role == "image") {
      pictures += std::format("<|__vpipe_fs_im_start__|>{}"
                              "<|__vpipe_fs_im_end__|>", in.path.string());
    }
  }
  if (!pictures.empty()) {
    text = pictures + "\n" + text;
  }
  BuiltGraph g;
  const std::string jid = job.id.str();
  g.text_sink = "text-sink";
  g.result_sink = "result-sink";

  Json chat_cfg = {
    {"hf_dir", job.model.dir.string()},
    {"disable_thinking", jget(job.params, "disable_thinking", true)},
    {"max_new_tokens", jget(job.params, "max_new_tokens", 1024)},
    // Its MTP head drafting ahead, verified token for token -- greedy or
    // sampled, penalties too (DESIGN §15 0k): the same reply, decoded
    // faster. Where the model has no head, vpipe decodes as ever.
    {"mtp", jget(job.params, "mtp", true)},
    // Kept loaded after the graph (vpipe's warm hold, DESIGN §15 0l): the
    // next request is handed the model -- no 16 GB read, no residency
    // warmup -- until a generation needs other weights or the time runs
    // out.
    {"keep_loaded", jget(job.params, "keep_loaded", 0.0)},
    // Its buffers wired as they load (vpipe's wire_weights, §15 0m): the
    // compressor never takes the weights mid-read -- a 27B's first
    // forward took 24-32 s bringing them back on a box short of free
    // memory, 1.8 s wired.
    {"wire_weights", true},
    // The stream port a piece at a time (vpipe's default is ~20-word
    // chunks for text-to-speech): the suggestion is watched as it is
    // written.
    {"stream_words", 0},
  };
  // A drafter shipped apart from the model (its conversion dropped the
  // head): vpipe's text-chat loads it beside the model.
  if (auto mm = jget<std::string>(job.params, "mtp_model", "");
      !mm.empty()) {
    chat_cfg["mtp_model"] = mm;
  }
  // A DFlash 2 block drafter, chosen in Settings: it takes over from MTP
  // (vpipe's draft_model), held at draft_bits; its round length adapts.
  if (auto dm = jget<std::string>(job.params, "draft_model", "");
      !dm.empty()) {
    chat_cfg["draft_model"] = dm;
    chat_cfg["draft_bits"] = jget(job.params, "draft_bits", 8);
  }
  Json stages = Json::array();
  stages.push_back(stage("text-prompt", "text-prompt", Json::array(),
                         {{"text", text}}));
  // The model's own sampler (catalog engine.vpipe.sampling: Qwen's
  // recommendation), on text-chat's sampler port; none, greedy. Its seed
  // 0 is a fresh one each time: asked again, the assistant may answer
  // otherwise.
  Json chat_in = Json::array({port("text-prompt")});
  if (const Json smp = jget(job.params, "sampling", Json::object());
      smp.is_object() && !smp.empty()) {
    Json cfg = Json::object();
    for (const char* k : {"temperature", "top_k", "top_p", "min_p",
                          "repetition_penalty", "presence_penalty"}) {
      if (smp.contains(k) && smp[k].is_number()) {
        cfg[k] = smp[k];
      }
    }
    cfg["seed"] = jget<std::uint64_t>(smp, "seed", 0);
    stages.push_back(stage("sampler", "sampler-select", Json::array(), cfg));
    chat_in.push_back(port("sampler"));
  }
  stages.push_back(stage("text-chat", "text-chat", chat_in, chat_cfg));
  stages.push_back(sink(g.result_sink, port("text-chat", 0), false));
  stages.push_back(sink(g.text_sink, port("text-chat", 1), false));
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

namespace {

// A picture exported as it is: read, written as the format says.
// `suffix` names the stages of one among several (a still's pages).
void
picture_stages(const JobSpec& job, const JobInput& in, const fs::path& out,
               const std::string& suffix, Json& stages)
{
  const std::string name = jget<std::string>(job.params, "format", "");
  const bool alpha = in.info.frame.alpha != media::AlphaMode::None;
  const std::string read = "read" + suffix;
  // F16 whatever the source: 8 bits lose nothing in it, and 16-bit or
  // float sources keep what they have. ImageIO, so the file's colour
  // space is read as declared and written into the copy. A camera RAW
  // has none to declare: vpipe develops it, upright -- scene-linear for
  // OpenEXR (a linear format, which keeps the sensor's highlights above
  // 1), the camera's own look in sRGB for everything else.
  stages.push_back(stage(read, "load-image", Json::array(),
                         {{"url", in.path.string()},
                          {"dtype", "f16"},
                          {"decoder", "imageio"},
                          {"raw", name == "exr" ? "linear" : "rendered"},
                          {"alpha", alpha ? "keep" : "drop"}}));
  const bool deep = name == "png16" || name == "tiff16";
  Json cfg = {{"path", out.string()},
              {"format", name.starts_with("png")    ? "png"
                         : name.starts_with("tiff") ? "tiff"
                                                    : name},
              {"bit_depth", deep ? 16 : 8}};
  if (name == "exr") {
    cfg.erase("bit_depth");
  }
  if (const int q = jget(job.params, "quality", 0); q > 0) {
    cfg["quality"] = q;
  }
  Json save_in = Json::array({port(read)});
  // The source's EXIF goes with it, where the format can hold EXIF.
  if (name != "exr") {
    save_in.push_back(port(read, 1));
  }
  stages.push_back(stage("write" + suffix, "save-image", std::move(save_in),
                         std::move(cfg)));
}

}

Result<BuiltGraph>
build_export(const JobSpec& job)
{
  const std::string name = jget<std::string>(job.params, "format", "");
  const ExportFormat* f = export_format(name);
  if (!f) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' is not an export format", name));
  }
  // A sound is cut to the sample by the engine itself (media::
  // write_sound): no graph.
  if (f->sound) {
    return make_error(Code::Unsupported, std::format(
        "{} is a sound format: the engine writes it itself", name));
  }
  // A COMPOSITION on a timeline (DESIGN §6a): its layers drawn whole by
  // Valtz, frame by frame, at the size, rate and length the job gives;
  // its mix -- an input "sound", when anything in it sounds -- joined
  // after.
  if (const Json sj = jget(job.params, "stack", Json());
      sj.is_object() && jget(job.params, "composition", false)) {
    if (!f->video) {
      return make_error(Code::InvalidArgument, std::format(
          "{} is for pictures", name));
    }
    BuiltGraph g;
    const std::string jid = job.id.str();
    g.output = job.output_dir / std::format("{}.{}", jid, f->extension);
    fs::path written = g.output;
    for (const auto& in : job.inputs) {
      if (in.role == "sound") {
        g.video_part = job.output_dir / std::format("{}-picture.{}", jid,
                                                    f->extension);
        g.audio_part = in.path;
        g.audio_is_source = true;  // the mix stays where it is cached
        written = g.video_part;
      }
    }
    MovieFeed m;
    m.stage = "read";
    m.stack = media::movie_stack_from_json(sj);
    const Rational rate = m.stack->rate.num > 0 ? m.stack->rate
                                                : Rational{24, 1};
    m.rate = rate;
    m.first = 0;
    m.count = std::max<std::int64_t>(1, jget<std::int64_t>(job.params,
                                                           "frames", 1));
    m.size = {jget<std::int32_t>(job.params, "width", 0),
              jget<std::int32_t>(job.params, "height", 0)};
    if (m.size.width <= 0 || m.size.height <= 0) {
      return make_error(Code::InvalidArgument, "a composition has no frame");
    }
    m.tags = {{"color_primaries", 1}, {"color_transfer", 1},
              {"color_matrix", 1}, {"color_full_range", true},
              {"fps_num", rate.num}, {"fps_den", rate.den}};
    // The PROJECT's output colour (an export of the project): drawn in
    // it, tagged so.
    if (const auto c = media::output_color(
            jget<std::string>(job.params, "color", ""))) {
      m.color = *c;
      m.tags["color_primaries"] = static_cast<int>(c->primaries);
      m.tags["color_transfer"] = static_cast<int>(c->transfer);
      m.tags["color_matrix"] = static_cast<int>(
          c->matrix == media::Matrix::Identity ? media::Matrix::BT709
                                               : c->matrix);
    }
    g.width = m.size.width;
    g.height = m.size.height;
    g.frames = m.count;
    g.movie = std::move(m);
    const std::string codec = name == "hevc10" ? "hevc" : name;
    Json stages = Json::array();
    stages.push_back(stage("read", ex::kSourceType, Json::array(),
                           Json::object()));
    stages.push_back(stage("write", "avf-save-video",
                           Json::array({port("read")}),
                           {{"path", written.string()},
                            {"codec", codec},
                            {"fps", rate.to_double()},
                            {"frames", g.frames}}));
    g.spec = {{"id", "valtz-" + jid},
              {"stages", stages},
              {"subpipelines", Json::array()}};
    return g;
  }
  // A STILL'S PAGES (DESIGN §6a): each page's rendering written as the
  // format says, one chain a page, all in one graph -- a file a page.
  if (std::ranges::any_of(job.inputs, [](const JobInput& in) {
        return in.role == "page";
      })) {
    if (f->video) {
      return make_error(Code::InvalidArgument, std::format(
          "{} is for videos", name));
    }
    BuiltGraph g;
    const std::string jid = job.id.str();
    Json stages = Json::array();
    for (const auto& in : job.inputs) {
      if (in.role != "page") {
        continue;
      }
      const std::size_t n = g.pages.size();
      g.pages.push_back(job.output_dir /
                        std::format("{}-{}.{}", jid, n + 1, f->extension));
      picture_stages(job, in, g.pages.back(), std::format("-{}", n + 1),
                     stages);
    }
    g.output = g.pages.front();
    g.spec = {{"id", "valtz-" + jid},
              {"stages", stages},
              {"subpipelines", Json::array()}};
    return g;
  }
  if (job.inputs.size() != 1) {
    return make_error(Code::InvalidArgument, "an export takes one input");
  }
  const JobInput& in = job.inputs.front();
  const bool video = in.info.type == media::MediaType::Video;
  if (video != f->video) {
    return make_error(Code::InvalidArgument, std::format(
        "{} is for {}", name, f->video ? "videos" : "pictures"));
  }
  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.{}", jid, f->extension);
  const bool alpha = in.info.frame.alpha != media::AlphaMode::None;
  Json stages = Json::array();
  if (video) {
    const std::string codec = name == "hevc10" ? "hevc" : name;
    // The picture goes through vpipe; a soundtrack -- which its stages do
    // not carry -- is joined back from the source by the engine.
    fs::path written = g.output;
    if (in.info.has_audio) {
      g.video_part = job.output_dir / std::format("{}-picture.{}", jid,
                                                  f->extension);
      g.audio_part = in.path;
      g.audio_is_source = true;
      written = g.video_part;
    }
    Json read = {{"url", in.path.string()},
                 {"dtype", "f16"},
                 {"alpha", alpha ? "keep" : "drop"}};
    // A trim (the asset's "trim" modifier): from the mark-in, exactly as
    // many frames as it keeps -- the sound the same span.
    const media::Trim trim = media::trim_from_json(
        jget(job.params, "trim", Json::object()));
    const Rational rate = in.info.frame_rate.num > 0 ? in.info.frame_rate
                          : trim.rate.num > 0        ? trim.rate
                                                     : Rational{24, 1};
    const double fps = rate.to_double();
    std::int64_t first = 0;
    std::int64_t count = -1;
    if (!trim.identity()) {
      const std::int64_t total = in.info.frame_count;
      first = std::max<std::int64_t>(0, trim.in);
      std::int64_t last = trim.out >= 0 ? trim.out
                          : total > 0   ? total - 1
                                        : -1;
      if (total > 0) {
        last = std::min(last, total - 1);
      }
      read["start_s"] = static_cast<double>(first) / fps;
      if (last >= first) {
        count = last - first + 1;
        read["max_frames"] = count;
        g.audio_duration = static_cast<double>(count) / fps;
      }
      g.audio_start = static_cast<double>(first) / fps;
    }
    // A LOOK (the clip's adjustment and crop tracks) is laid on by Valtz
    // as the frames are decoded, each at its own frame, and the frames go
    // in through a valtz-source: vpipe writes them, no file is made in
    // between. Without one, vpipe reads the clip itself.
    const auto adj = media::keyed_adjustments_from_json(
        jget(job.params, "adjust", Json::object()));
    const auto crop = media::keyed_crop_from_json(
        jget(job.params, "crop", Json::object()));
    // A clip's STACK -- its layers, its canvas, its timeline -- is drawn
    // whole by Valtz, frame by frame, and goes in as the look does.
    const Json sj = jget(job.params, "stack", Json());
    if (sj.is_object() && !jget(sj, "layers", Json::array()).empty()) {
      MovieFeed m;
      m.stage = "read";
      m.path = in.path;
      m.stack = media::movie_stack_from_json(sj);
      m.rate = rate;
      m.first = first;
      m.count = count;
      // Untrimmed, the timeline runs as long as it was set to.
      if (trim.identity() && m.stack->frames > 0) {
        m.count = m.stack->frames;
      }
      const media::PixelSize own{in.info.frame.width, in.info.frame.height};
      const auto& bottom = m.stack->layers.front().crop;
      m.size = m.stack->canvas.set()
          ? media::PixelSize{m.stack->canvas.width, m.stack->canvas.height}
          : m.stack->canvas.framed() ? m.stack->canvas.frame()
          : bottom.empty() ? own : picture_size(own, bottom.at(0));
      const auto& c = in.info.frame.color;
      m.tags = {{"color_primaries", static_cast<int>(c.primaries)},
                {"color_transfer", static_cast<int>(c.transfer)},
                {"color_matrix", static_cast<int>(c.matrix)},
                {"color_full_range", true},
                {"fps_num", rate.num},
                {"fps_den", rate.den}};
      g.width = m.size.width;
      g.height = m.size.height;
      g.frames = m.count > 0 ? m.count : in.info.frame_count;
      g.movie = std::move(m);
      stages.push_back(stage("read", ex::kSourceType, Json::array(),
                             Json::object()));
    } else if (!adj.identity() || !crop.identity()) {
      MovieFeed m;
      m.stage = "read";
      m.path = in.path;
      m.adjust = adj;
      m.crop = crop;
      m.rate = rate;
      m.first = first;
      m.count = count;
      const media::PixelSize own{in.info.frame.width, in.info.frame.height};
      m.size = crop.empty() ? own : picture_size(own, crop.at(0));
      const auto& c = in.info.frame.color;
      m.tags = {{"color_primaries", static_cast<int>(c.primaries)},
                {"color_transfer", static_cast<int>(c.transfer)},
                {"color_matrix", static_cast<int>(c.matrix)},
                {"color_full_range", true},
                {"fps_num", rate.num},
                {"fps_den", rate.den}};
      g.width = m.size.width;
      g.height = m.size.height;
      g.frames = m.count > 0
          ? m.count
          : std::max<std::int64_t>(0, in.info.frame_count - m.first);
      g.movie = std::move(m);
      stages.push_back(stage("read", ex::kSourceType, Json::array(),
                             Json::object()));
    } else {
      stages.push_back(stage("read", "avf-load-video", Json::array(),
                             read));
      g.frames = count > 0
          ? count
          : std::max<std::int64_t>(0, in.info.frame_count - first);
      g.preview_from = in.path;
      g.preview_start = static_cast<double>(first) / fps;
      g.preview_fps = fps;
    }
    // The writer counts the frames against what it is told to expect:
    // the export's progress.
    Json write = {{"path", written.string()},
                  {"codec", codec},
                  {"fps", fps}};
    if (g.frames > 0) {
      write["frames"] = g.frames;
    }
    stages.push_back(stage("write", "avf-save-video",
                           Json::array({port("read")}), std::move(write)));
  } else if (const Json aj = jget(job.params, "adjust", Json::object()),
                        cj = jget(job.params, "crop", Json::object());
             (aj.is_object() && !aj.empty()) ||
             (cj.is_object() && !cj.empty())) {
    // AN IMAGE MODIFIER (project::Modifier) goes into the file as it is
    // made, and only there: the engine decodes the picture WITH its
    // adjustments and its crop -- Core Image, as the stage shows them, a
    // RAW developed on the way -- into an F16 valtz-source lease, and
    // save-image writes that. Upright (the decode applies the EXIF
    // orientation), so the EXIF that goes with it says Orientation 1
    // (media::exif_block_for); OpenEXR gets it linear and unclamped.
    VALTZ_ASSIGN(media::PixelSize own, media::oriented_size(in.path));
    RefFeed f;
    f.stage = "read";
    f.path = in.path;
    f.crop = media::crop_from_json(cj);
    // A crop's padding may be transparent: the file keeps alpha then.
    const bool clear = !f.crop.identity() && f.crop.pad[3] < 1;
    const media::PixelSize sz = picture_size(own, f.crop);
    f.size = sz;
    f.fitted = sz;
    f.channels = alpha || clear ? 4 : 3;
    f.adjust = media::adjustments_from_json(aj);
    f.linear = name == "exr";
    g.references.push_back(f);
    g.width = sz.width;
    g.height = sz.height;
    stages.push_back(stage("read", ex::kSourceType, Json::array(),
                           Json::object()));
    const bool deep = name == "png16" || name == "tiff16";
    Json cfg = {{"path", g.output.string()},
                {"format", name.starts_with("png")    ? "png"
                           : name.starts_with("tiff") ? "tiff"
                                                      : name}};
    if (name != "exr") {
      cfg["bit_depth"] = deep ? 16 : 8;
    }
    if (const int q = jget(job.params, "quality", 0); q > 0) {
      cfg["quality"] = q;
    }
    Json save_in = Json::array({port("read")});
    if (name != "exr") {
      g.exif_from = in.path;
      stages.push_back(stage(kExifSource, ex::kSourceType, Json::array(),
                             Json::object()));
      save_in.push_back(port(kExifSource));
    }
    stages.push_back(stage("write", "save-image", std::move(save_in),
                           std::move(cfg)));
  } else {
    picture_stages(job, in, g.output, "", stages);
  }
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

Result<BuiltGraph>
build_upscale_video(const JobSpec& job)
{
  if (job.inputs.size() != 1 ||
      job.inputs.front().info.type != media::MediaType::Video) {
    return make_error(Code::InvalidArgument,
                      "an upscale takes one clip (role \"source\")");
  }
  const JobInput& in = job.inputs.front();
  const int W = jget(job.params, "width", 0);
  const int H = jget(job.params, "height", 0);
  if (W <= 0 || H <= 0) {
    return make_error(Code::InvalidArgument, "an upscale needs its size");
  }
  // FlashVSR's grid: both sides multiples of 128 (its window partition);
  // the restored frames are fitted back to the size asked for.
  const Json up = jget(jget(job.model.engine, "vpipe", Json::object()),
                       "upscale", Json::object());
  const int grid = std::max(16, jget(up, "grid", 128));
  const int PW = ceil_to(W, grid);
  const int PH = ceil_to(H, grid);
  // A group of frames is one clip to the model: 8k + 1 of them (it takes
  // eight per chunk after its opening), giving `group - overlap`.
  const int G = std::max(9, jget(job.params, "group", jget(up, "group", 25)));
  const int O = std::clamp(jget(job.params, "overlap",
                                jget(up, "overlap", 4)), 0, G - 1);
  const int S = G - O;
  const Rational rate = in.info.frame_rate.num > 0 ? in.info.frame_rate
                                                   : Rational{24, 1};
  const double fps = rate.to_double();
  const std::int64_t total = in.info.frame_count;
  const std::int64_t first = std::max<std::int64_t>(
      0, jget<std::int64_t>(job.params, "first", 0));
  std::int64_t count = jget<std::int64_t>(job.params, "count", -1);
  if (count <= 0) {
    count = total > first ? total - first : 0;
  }
  if (total > 0) {
    count = std::min(count, total - first);
  }
  if (count <= 0) {
    return make_error(Code::InvalidArgument, "the clip has no frames there");
  }
  // Groups enough for every frame, the last filled to a whole group.
  const std::int64_t groups = (count + S - 1) / S;

  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.mp4", jid);
  fs::path written = g.output;
  if (in.info.has_audio) {
    g.video_part = job.output_dir / std::format("{}-picture.mp4", jid);
    g.audio_part = in.path;
    g.audio_is_source = true;
    g.audio_start = static_cast<double>(first) / fps;
    g.audio_duration = static_cast<double>(count) / fps;
    written = g.video_part;
  }
  g.width = W;
  g.height = H;
  g.frames = count;

  MovieFeed m;
  m.stage = "read";
  m.path = in.path;
  m.rate = rate;
  m.first = first;
  m.count = count;
  m.size = {in.info.frame.width, in.info.frame.height};
  m.sample = media::Sample::U8;
  const auto& c = in.info.frame.color;
  m.tags = {{"color_primaries", static_cast<int>(c.primaries)},
            {"color_transfer", static_cast<int>(c.transfer)},
            {"color_matrix", static_cast<int>(c.matrix)},
            {"color_full_range", true},
            {"fps", fps},
            {"fps_num", rate.num},
            {"fps_den", rate.den}};
  // What one group more in flight holds: its frames at the model's size
  // (resampled, then stacked), the projection's rows (one row frame per
  // four frames, a token per 16 x 16 cell, the denoiser's width, bf16),
  // and its frames decoded and fitted (F16).
  const std::uint64_t px = static_cast<std::uint64_t>(PW) * PH;
  const std::uint64_t hidden = jget<std::uint64_t>(up, "hidden", 1536);
  const std::uint64_t group_bytes =
      2 * G * px * 3 +
      static_cast<std::uint64_t>((G - 1) / 4 + 1) * (px / 256) * hidden * 2 +
      static_cast<std::uint64_t>(S) * 3 * 2 *
          (px + static_cast<std::uint64_t>(W) * H);
  m.gate = {"tap", G, O,
            std::max(0, jget(job.params, "ahead", jget(up, "ahead", 0))),
            groups, group_bytes};
  g.movie = std::move(m);

  Json stages = Json::array();
  stages.push_back(stage("read", ex::kSourceType, Json::array(),
                         Json::object()));
  stages.push_back(stage("model-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.dir.string()}}));
  // Bicubic, as the model was trained: it restores what the upsampling
  // blurred. 8-bit resamples on the GPU.
  stages.push_back(stage("upsample", "image-resample",
                         Json::array({port("read")}),
                         {{"width", PW}, {"height", PH}, {"fit", "stretch"},
                          {"algorithm", "bicubic"}}));
  const std::int64_t group_mb =
      static_cast<std::int64_t>(G) * PW * PH * 3 / (1 << 20) + 64;
  // Whole groups only: the feeder sends each group's overlap again, so
  // the stack keeps nothing back -- kept, its last frames would go out at
  // the end as a group too short for the model.
  stages.push_back(stage("stack", "temporal-stack",
                         Json::array({port("upsample")}),
                         {{"mode", "video"}, {"group_size", G},
                          {"max_mb", group_mb}}));
  stages.push_back(stage("encode", "flashvsr-src-encoder",
                         Json::array({port("stack"), port("model-select")}),
                         Json::object()));
  stages.push_back(stage("generate", "generate-video",
                         Json::array({port("encode"), unwired(),
                                      port("model-select")}),
                         merged({{"width", PW},
                                 {"height", PH},
                                 {"frames", G},
                                 {"seed", jget<std::int64_t>(job.params,
                                                             "seed", 0)},
                                 {"unload_when_idle", "auto"}},
                                accel_of(job))));
  stages.push_back(stage("decode", "vae-decode",
                         Json::array({port("generate"),
                                      port("model-select")}),
                         Json::object()));
  std::string last = "decode";
  if (PW != W || PH != H) {
    stages.push_back(stage("fit", "image-resample",
                           Json::array({port("decode")}),
                           {{"width", W}, {"height", H}, {"fit", "stretch"},
                            {"algorithm", "lanczos"}}));
    last = "fit";
  }
  // The frames out are counted -- what the feed waits on -- and cut at
  // the clip's own: the padded tail's frames go no further.
  stages.push_back(stage("tap", ex::kTapType, Json::array({port(last)}),
                         {{ex::kTapMaxFrames, count}}));
  stages.push_back(stage("write", "avf-save-video",
                         Json::array({port("tap")}),
                         {{"path", written.string()},
                          {"codec", "hevc"},
                          {"fps", fps},
                          {"frames", count}}));
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

Result<BuiltGraph>
build_upscale_image(const JobSpec& job)
{
  if (job.inputs.size() != 1 ||
      job.inputs.front().info.type != media::MediaType::Image) {
    return make_error(Code::InvalidArgument,
                      "an upscale takes one picture (role \"source\")");
  }
  const JobInput& in = job.inputs.front();
  const int W = jget(job.params, "width", 0);
  const int H = jget(job.params, "height", 0);
  if (W <= 0 || H <= 0) {
    return make_error(Code::InvalidArgument, "an upscale needs its size");
  }
  const Json up = jget(jget(job.model.engine, "vpipe", Json::object()),
                       "upscale", Json::object());
  // The restorer's latent is in patches of two: sides of 16.
  const int grid = std::max(8, jget(up, "grid", 16));
  const int PW = ceil_to(W, grid);
  const int PH = ceil_to(H, grid);
  VALTZ_ASSIGN(media::PixelSize own, media::oriented_size(in.path));
  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.png", jid);
  g.width = W;
  g.height = H;
  RefFeed f;
  f.stage = "read";
  f.path = in.path;
  f.size = own;
  f.fitted = own;
  f.channels = 3;
  g.references.push_back(f);

  Json stages = Json::array();
  stages.push_back(stage("read", ex::kSourceType, Json::array(),
                         Json::object()));
  stages.push_back(stage("model-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.dir.string()}}));
  stages.push_back(stage("upsample", "image-resample",
                         Json::array({port("read")}),
                         {{"width", PW}, {"height", PH}, {"fit", "stretch"},
                          {"algorithm", "bicubic"}}));
  // DINOv2 reads the picture (its own folder: ModelRef::encoder), the VAE
  // encodes it; the restorer takes both.
  Json cond = {{"unload_when_idle", "park"}};
  if (!job.model.encoder.empty()) {
    cond["encoder_dir"] = job.model.encoder.string();
  }
  stages.push_back(stage("condition", "diffusion-conditioner",
                         Json::array({unwired(), unwired(),
                                      port("model-select"),
                                      port("upsample")}),
                         std::move(cond)));
  stages.push_back(stage("encode", "vae-encode",
                         Json::array({port("upsample"),
                                      port("model-select")}),
                         Json::object()));
  stages.push_back(stage("generate", "generate-image",
                         Json::array({port("condition"), unwired(),
                                      port("model-select"), unwired(),
                                      unwired(), port("encode"), unwired(),
                                      unwired()}),
                         {{"seed", jget<std::int64_t>(job.params, "seed",
                                                      0)}}));
  stages.push_back(stage("decode", "vae-decode",
                         Json::array({port("generate"),
                                      port("model-select")}),
                         {{"dtype", "f16"}}));
  std::string last = "decode";
  if (PW != W || PH != H) {
    stages.push_back(stage("fit", "image-resample",
                           Json::array({port("decode")}),
                           {{"width", W}, {"height", H}, {"fit", "stretch"},
                            {"algorithm", "lanczos"}}));
    last = "fit";
  }
  stages.push_back(stage("write", "save-image", Json::array({port(last)}),
                         {{"path", g.output.string()},
                          {"format", "png"},
                          {"bit_depth", 16}}));
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

Result<BuiltGraph>
build_fetch_model(const JobSpec& job)
{
  auto hf_path = jget<std::string>(job.params, "hf_path", "");
  auto base = jget<std::string>(job.params, "base_path", "");
  if (hf_path.empty() || base.empty()) {
    return make_error(Code::InvalidArgument,
                      "fetch-model needs hf_path and base_path");
  }
  BuiltGraph g;
  Json cfg = {
    {"base_path", base},
    {"model_path", hf_path},
    {"verify_checksums", true},
    {"skip_existing_files", true},
  };
  // Which of the repo's models: a repo may publish several.
  if (auto v = jget<std::string>(job.params, "variant", ""); !v.empty()) {
    cfg["model_variant"] = v;
  }
  // A gated repo's access token (Controller::download_model): sent by
  // the stage as the requests' bearer, in this spec in memory only.
  if (auto t = jget<std::string>(job.params, "hf_token", ""); !t.empty()) {
    cfg["hf_token"] = t;
  }
  Json stages = Json::array();
  stages.push_back(stage("model-fetch", "model-fetch", Json::array(), cfg));
  g.spec = {{"id", "valtz-" + job.id.str()},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

}
