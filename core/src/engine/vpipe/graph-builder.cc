#include "engine/vpipe/graph-builder.h"
#include "engine/vpipe/encoding-cache.h"
#include "engine/vpipe/job-progress.h"

#include "valtz/assist/video-summary.h"
#include "valtz/base/log.h"
#include "valtz-vpipe/exchange.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>
#include <set>
#include <tuple>

namespace valtz::engine::vp {

namespace fs = std::filesystem;

namespace {

// The helper's K/V pages, as its chat and a video summary both load it:
// the page sizing is part of vpipe's model cache key, so alike, a model
// kept warm by one is handed to the other. 16 tokens a page (text-chat's
// own), as many as the job's "context_tokens" -- the model's own
// (Qwen3.5's and 3.8's 262,144) as far as the Mac's memory holds its K/V,
// models::helper_context. The pages ARE its context: 65,536 for every
// Mac had cut an agent's long session at a quarter of the model's; that
// many still for a model whose config says none. Qwen's pool grows as it
// fills, so the cap costs nothing until the tokens come.
constexpr int kHelperPageTokens = 16;
constexpr int kHelperDefaultTokens = 65536;

int
helper_max_pages(const JobSpec& job)
{
  const std::int64_t tokens = std::max<std::int64_t>(
      1, jget<std::int64_t>(job.params, "context_tokens",
                            kHelperDefaultTokens));
  return static_cast<int>((tokens + kHelperPageTokens - 1) /
                          kHelperPageTokens);
}

// A picture's models kept BUILT between jobs (DESIGN §4n): vpipe hands
// the text encoder, the DiT and its VAE to its model manager as the
// graph goes, where the box has room for them, and the next graph asking
// for the same checkpoint and settings takes them back resident -- a
// Qwen-Image 2.1 Turbo picture's 0.85 s of blocks read back, 0.69 s of
// VAE built and, for new words, 1.7 s of encoder loaded. How long: the
// engine's, by the Mac (param "keep_models", seconds; 0 lets them go).
double
keep_models_s(const JobSpec& job)
{
  return std::max(0.0, jget(job.params, "keep_models", 0.0));
}

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

// Favor's acceleration options (settled tuning, models/tuning.h) as the
// generating stage spells them -- the keys of vpipe's acceleration
// vocabulary; a recipe from before them has none.
Json
accel_from(const Json& t)
{
  Json out = Json::object();
  for (const char* k : {"sol_attn", "sol_tau", "sage_attn", "i8_gemm",
                        "motion_cache", "ane_ffn", "ane_qkv"}) {
    if (t.is_object() && t.contains(k)) {
      out[k] = t[k];
    }
  }
  // A threshold a STEP (H3's schedule, "1,1.5,1.75": tuning.h) is vpipe's
  // `sol_tau_steps`, its first entry the `sol_tau` beside it.
  if (t.is_object() && t.contains("sol_tau") && t["sol_tau"].is_string()) {
    const auto steps = t["sol_tau"].get<std::string>();
    out["sol_tau"] = std::strtod(steps.c_str(), nullptr);
    out["sol_tau_steps"] = steps;
  }
  // Sol's exact flavour, only with Sol on: diag is vpipe's own, so a job
  // without it has the graph it always had. (A vpipe from before it
  // ignores the key -- and runs diag.)
  if (jget(t, "sol_attn", false) &&
      jget<std::string>(t, "sol_threshold", "diag") == "exact") {
    out["sol_threshold"] = "exact";
  }
  // The int8 GEMMs' tier, only with them on and only when it is not the
  // one vpipe runs anyway: an Accurate job's graph is as it always was. (A
  // vpipe from before the tiers ignores the key -- and runs Accurate.)
  const auto tier = jget<std::string>(t, "i8_gemm_tier", "accurate");
  if (jget(t, "i8_gemm", false) && tier != "accurate") {
    out["i8_gemm_tier"] = tier;
  }
  // SageAttention's int8 P*V, only on, and only with SageAttention: a job
  // without it has the graph it always had. (A vpipe from before it
  // ignores the key -- and runs SageAttention alone.)
  if (jget(t, "sage_attn", false) && jget(t, "sage_pv8", false)) {
    out["sage_pv8"] = true;
    // ...on every row, H3's prompt and soundtrack too.
    if (jget(t, "sage_pv8_all_rows", false)) {
      out["sage_pv8_all_rows"] = true;
    }
  }
  return out;
}

// The job's own (the recipe's "tuning").
Json
accel_of(const JobSpec& job)
{
  return accel_from(jget(job.params, "tuning", Json::object()));
}

// Favor's CODEC int8 (tuning `vae_i8_gemm`, `vae_i8_gemm_tier`) as
// vae-decode and vae-encode spell it: their own `i8_gemm` and tier. Empty
// when off, so an f16 job's graph -- and its kept latents' key -- are as
// they always were. (A vpipe whose codec has no int8 ignores both.)
Json
codec_accel_of(const JobSpec& job)
{
  const Json t = jget(job.params, "tuning", Json::object());
  Json out = Json::object();
  if (jget(t, "vae_i8_gemm", false)) {
    out["i8_gemm"] = true;
    const auto tier = jget<std::string>(t, "vae_i8_gemm_tier", "accurate");
    if (tier != "accurate") { out["i8_gemm_tier"] = tier; }
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

// The text encoder's and the VAE's WEIGHTS where they are linked apart
// (Draw Things checkpoints: ModelRef::encoder_weights / vae_weights), on
// the stage that reads them -- its config, tokenizer and latent
// statistics still hf_dir's.
Json
with_encoder(Json cfg, const JobSpec& job)
{
  if (!job.model.encoder_weights.empty()) {
    cfg["encoder_dir"] = job.model.encoder_weights.string();
  }
  return cfg;
}

Json
with_vae(Json cfg, const JobSpec& job)
{
  if (!job.model.vae_weights.empty()) {
    cfg["vae_dir"] = job.model.vae_weights.string();
  }
  return cfg;
}

// How a clip Valtz makes is kept: the job's "intermediate" (the
// project's, OutputSettings) -- vpipe's codec, the file's extension, its
// sound's. One not known (a newer Valtz's) is the default, ProRes 422 HQ.
// Its writers ask for no alpha: what a model or a restorer makes is
// opaque, and a ProRes 4444 then has no alpha plane (DESIGN §15 0zo:
// before it, vpipe's writer coded one of 1s, the encoder fed RGBA).
struct ClipFormat {
  std::string       codec;
  std::string_view  ext;
  media::SoundCodec sound;
};

ClipFormat
clip_format(const JobSpec& job)
{
  std::string f = jget<std::string>(job.params, "intermediate", "");
  if (!media::is_intermediate_format(f)) {
    f = std::string(media::kDefaultIntermediate);
  }
  const bool prores = media::is_prores(f);
  return {f, prores ? "mov" : "mp4",
          prores ? media::SoundCodec::Pcm : media::SoundCodec::Aac};
}

// A movie export's writer (avf-save-video) as the request's ENCODING
// asks (engine::VideoEncoding, param "video"): its codec -- a ProRes
// flavour, HEVC 8-bit -- and H.264's / HEVC's rate, keyframes, B-frames,
// profile, level and entropy coding; the keyframe interval from seconds
// at `fps`.
Json
video_writer(Json write, const Json& params, std::string_view format,
             double fps)
{
  const VideoEncoding e = video_encoding_from_json(
      jget(params, "video", Json::object()));
  write["codec"] = video_codec(format, e);
  if (format.starts_with("prores")) {
    return write;
  }
  if (e.bitrate > 0) { write["bitrate"] = e.bitrate; }
  if (e.max_bitrate > 0) { write["max_bitrate"] = e.max_bitrate; }
  if (e.quality > 0) { write["quality"] = e.quality; }
  if (e.keyframe_seconds > 0 && fps > 0) {
    write["keyframe_interval"] = std::max<std::int64_t>(
        1, std::llround(e.keyframe_seconds * fps));
  }
  if (e.b_frames >= 0) {
    write["frame_reordering"] = e.b_frames > 0 ? "on" : "off";
  }
  if (format == "h264") {
    if (!e.profile.empty()) { write["profile"] = e.profile; }
    if (!e.level.empty()) { write["level"] = e.level; }
    if (!e.entropy.empty()) { write["entropy"] = e.entropy; }
  }
  return write;
}

// The run-time LoRAs on the stage that takes them -- generate-image, an
// H3 config beat: `lora` the first, `lora2` .. `lora8` the slots after it,
// each at its strength ("lora_scale", "lora<k>_scale"). A slot is its
// index in vpipe (a strength is addressed by it), so one left empty keeps
// its place.
void
lay_loras(Json& cfg, const JobSpec& job)
{
  if (!job.model.lora.empty()) {
    cfg["lora"] = job.model.lora.string();
    cfg["lora_scale"] = jget(job.params, "lora_scale", 1.0);
  }
  const std::size_t more = std::min<std::size_t>(
      job.model.more_loras.size(), kLoraSlots - 1);
  for (std::size_t i = 0; i < more; ++i) {
    const auto& f = job.model.more_loras[i];
    if (f.empty()) {
      continue;
    }
    const std::string key = std::format("lora{}", i + 2);
    cfg[key] = f.string();
    cfg[key + "_scale"] =
        jget(job.params, (key + "_scale").c_str(), 1.0);
  }
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
  // Laid once, however many decoders read it (a regenerated clip's
  // draft and its refine).
  for (const Json& s : stages) {
    if (jget<std::string>(s, "id", "") == "vae-select") {
      return "vae-select";
    }
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

// The size a model was trained at for an aspect (catalog `reference_fit`
// "trained_size", `trained_area`): Bria's get_image_size for Fibo 2.0, as
// its pipeline crops each picture -- of the sides divisible by `multiple`,
// at least four of it, within 10% of `area`, the one cropping least from
// a picture of that aspect, then the one nearest `area`, then the wider.
media::PixelSize
trained_size(double aspect, double area, int multiple)
{
  const long long a0 = static_cast<long long>(area);
  const long long m = multiple, lo = 4 * m;
  media::PixelSize best{0, 0};
  double best_crop = 0;
  long long best_dist = 0;
  bool have = false;
  for (long long w = lo; w < 2 * a0 / lo; w += m) {
    for (long long h = lo; h < 2 * a0 / w; h += m) {
      const long long a = w * h;
      if (a < 0.9 * area || a > 1.1 * area) { continue; }
      const double r = static_cast<double>(w) / static_cast<double>(h);
      // Compared at nine decimals, as the reference rounds them.
      const double crop =
          std::nearbyint((1.0 - std::min(r / aspect, aspect / r)) * 1e9) /
          1e9;
      const long long dist = std::llabs(a - a0);
      if (!have || crop < best_crop ||
          (crop == best_crop &&
           (dist < best_dist || (dist == best_dist && w > best.width)))) {
        best = {static_cast<int>(w), static_cast<int>(h)};
        best_crop = crop;
        best_dist = dist;
        have = true;
      }
    }
  }
  return best;
}

// Text-to-image and edit share one graph. An edit adds, per picture, a
// valtz-source the engine fills at the picture's own size, an
// `image-resample` that fits it (Lanczos), a vae-encode of the fitted
// picture for the DiT, and -- where the family's text encoder sees
// pictures -- the SAME fitted picture into the conditioner. One resize
// feeds both: Qwen-Image-2.1's DiT drops a reference whose latent grid
// is not exactly twice the conditioner's merged grid.
// What a picture fed to an encoder is made from (encoding-cache.h): its
// bytes, the look and crop laid on as it is decoded, the size it is fed
// at, and the resample that fits it -- the pixels the encoder reads.
Json
picture_doc(const ContentHash& hash, const RefFeed& f, const Json& fit)
{
  return {{"hash", hash.hex()},
          {"adjust", media::to_json(f.adjust)},
          {"crop", media::to_json(f.crop)},
          {"fed", Json::array({f.size.width, f.size.height})},
          {"channels", f.channels},
          {"linear", f.linear},
          {"fit", fit}};
}

// A family config beat as an encoder reads it: the live preview's keys
// change nothing it makes.
Json
without_preview(Json cfg)
{
  if (cfg.is_object()) {
    for (const char* k : {"preview_vae", "preview_every",
                          "preview_max_edge"}) {
      cfg.erase(k);
    }
  }
  return cfg;
}

// An encoding's key document: vpipe's build, the model's files, what it
// is, then what it is made from.
Json
encoding_doc(const EncodingCache& c, const JobSpec& job,
             std::string_view what, Json made_from)
{
  made_from["engine"] = c.engine;
  made_from["model"] = c.model;
  // The weights it is made by, linked apart: a conditioning the text
  // encoder's, a latent the VAE's, Ref2VA's references both.
  for (const char* part : {"encoder", "vae"}) {
    const bool reads = what == "references" ||
                       (std::string_view(part) == "encoder"
                            ? what == "conditioning"
                            : what == "latents" || what == "keyframe");
    if (reads && c.weights.contains(part)) {
      made_from["weights"][part] = c.weights[part];
    }
  }
  made_from["family"] = job.model.family;
  made_from["what"] = std::string(what);
  return made_from;
}

Result<BuiltGraph>
build_image(const JobSpec& job, const std::vector<RefImage>& refs,
            const EncodingCache* cache)
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
  // A model trained at a few sizes (catalog `reference_fit`
  // "trained_size": Fibo 2.0) makes the one for the shape asked, as its
  // own pipeline makes the first picture's.
  const bool trained = edit && jget<std::string>(ed, "reference_fit", "") ==
                                   "trained_size";
  const double trained_area = jget(ed, "trained_area", 1024.0 * 1024.0);
  if (trained) {
    const auto t = trained_size(static_cast<double>(width) / height,
                                trained_area, align);
    if (t.width > 0) {
      width = t.width;
      height = t.height;
    }
  }
  width = round_to(width, align);
  height = round_to(height, align);
  int steps = jget(job.params, "steps", jget(defaults, "steps", 8));
  // A checkpoint distilled to a schedule of its own (catalog
  // `steps_fixed`: Qwen-Image 2.1 Turbo's sample_sigmas) runs its own
  // count -- vpipe ignores another, with a warning -- so it is asked for
  // that and nothing else.
  if (jget(vp, "steps_fixed", false)) {
    steps = jget(defaults, "steps", 8);
  }
  auto seed = jget<std::int64_t>(job.params, "seed", 0);

  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.png", jid);

  // Pictures go in as ONE LIST on each side (vpipe's tensor-list, after
  // each picture's own resample), so the count is the model's
  // `max_references`, up to what tensor-list declares.
  std::vector<Json> fits;  // each picture's image-resample config
  // The pictures as fed (RefFeed), in order -- into g.references only
  // where the graph reads them (kept encodings may make them unread).
  std::vector<RefFeed> feeds;
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
    // How a reference is fitted (catalog `reference_fit`): padded right
    // and bottom to the grid ("pad", the default), or as Black Forest
    // Labs' own pipeline fits FLUX.3's ("crop_to_grid": shrunk under the
    // area, each side cut down to the grid, centred).
    const bool crop_grid =
        jget<std::string>(ed, "reference_fit", "pad") == "crop_to_grid";
    // The base's area at most (catalog `base_area`: FLUX.3's 4 MP, as
    // BFL fits one): a reference is tokens beside the picture made, and
    // a 4096-pixel square made from a base its size doubles a sequence
    // whose attention grows with its square. Without one, the base is
    // fitted at the output's own size.
    const double base_area = jget(ed, "base_area", 0.0);
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
        // (alignment-sized) difference -- at the output's shape, under
        // the model's base area where it has one.
        f.fitted = {width, height};
        if (base_area > 0 &&
            static_cast<double>(width) * height > base_area) {
          f.fitted = fit_area(f.fitted, base_area, align);
        }
        fit = {{"width", f.fitted.width}, {"height", f.fitted.height},
               {"fit", "crop"}, {"algorithm", "lanczos"}};
      } else if (trained) {
        // Covered and centre-cropped to the size trained for its aspect.
        f.fitted = trained_size(static_cast<double>(f.size.width) /
                                    f.size.height,
                                trained_area, align);
        fit = {{"width", f.fitted.width}, {"height", f.fitted.height},
               {"fit", "crop"}, {"algorithm", "lanczos"}};
      } else if (crop_grid) {
        // Shrunk (sides floored) under the area, then each side cut to
        // the grid about the centre: vpipe's image-resample does it.
        const double area =
            static_cast<double>(f.size.width) * f.size.height;
        const double k = area > budget ? std::sqrt(budget / area) : 1.0;
        const int nw = std::max(align, static_cast<int>(f.size.width * k));
        const int nh = std::max(align, static_cast<int>(f.size.height * k));
        f.fitted = {nw / align * align, nh / align * align};
        fit = {{"fit", "crop_to_grid"},
               {"max_pixels", static_cast<std::int64_t>(budget)},
               {"grid", align}, {"algorithm", "lanczos"}};
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
      feeds.push_back(std::move(f));
    }
  }
  const bool ref_to_cond = edit && jget(ed, "reference_to_conditioner",
                                        false);
  const bool have_refs = !feeds.empty();

  // The family's config source carries the preview keys.
  std::string cfg_stage = jget<std::string>(vp, "config_stage", "");
  Json cfg = Json::object();
  if (!cfg_stage.empty()) {
    cfg = jget(vp, "config", Json::object());
    if (edit) {
      cfg = merged(cfg, jget(ed, "config", Json::object()));
    }
    if (ref_to_cond && jget(ed, "vl_bounds", false) && have_refs) {
      // The conditioner's vision tower resizes on its own: down to
      // `vl_pixel_budget` (1024^2 by default) and `vl_max_pixels`, up to
      // `vl_min_pixels` (256^2). Bounded around the fitted pictures, it
      // reads them as they are -- the same pixels as the VAE.
      std::int64_t most = 0;
      std::int64_t least = std::numeric_limits<std::int64_t>::max();
      for (const auto& f : feeds) {
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
    // The schedule's shift where the family's config stage takes it
    // (catalog `config_shift`: MOSS-Image), as Favor's Custom moves it.
    const Json t = jget(job.params, "tuning", Json::object());
    if (vp.contains("config_shift") && t.is_object() &&
        t.contains("shift") && t["shift"].is_number()) {
      cfg["shift"] = t["shift"];
    }
  }
  // The conditioner's second output is the negative conditioning
  // (catalog `negative`: MOSS-Image's empty prompt's, which its CFG runs
  // against -- unwired, generate-image runs it without guidance).
  const bool cond_negative =
      jget<std::string>(vp, "negative", "") == "conditioner";

  // KEPT ENCODINGS (encoding-cache.h): the conditioning -- the words, the
  // family's config beat it reads, and the pictures where the text
  // encoder sees them -- and the pictures' latents, each named by what it
  // is made from; both need every picture's hash.
  std::string cond_key, latents_key;
  if (cache) {
    Json pics = Json::array();
    bool hashed = true;
    for (std::size_t i = 0; i < feeds.size(); ++i) {
      hashed = hashed && !refs[i].hash.is_zero();
      pics.push_back(picture_doc(refs[i].hash, feeds[i], fits[i]));
    }
    if (hashed) {
      Json c = {{"stage", "diffusion-conditioner"}, {"prompt", prompt}};
      if (!cfg_stage.empty()) {
        c["config_stage"] = cfg_stage;
        c["config"] = without_preview(cfg);
      }
      if (ref_to_cond && have_refs) {
        c["pictures"] = pics;
      }
      cond_key = encoding_key("conditioning",
                              encoding_doc(*cache, job, "conditioning",
                                           std::move(c)));
      if (have_refs) {
        // The codec's int8 makes other latents: keyed apart.
        Json l = {{"stage", "vae-encode"}, {"pictures", pics}};
        if (const Json ca = codec_accel_of(job); !ca.empty()) {
          l["config"] = ca;
        }
        latents_key = encoding_key(
            "latents", encoding_doc(*cache, job, "latents", std::move(l)));
      }
    }
  }
  const bool cond_kept = !cond_key.empty() && cache->kept(cond_key);
  const bool latents_kept = !latents_key.empty() &&
                            cache->kept(latents_key);
  // The pictures are read where something still encodes them: the VAE,
  // or the text encoder that sees them.
  const bool read_pics = have_refs &&
                         (!latents_kept || (!cond_kept && ref_to_cond));

  Json stages = Json::array();
  stages.push_back(stage("model-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.dir.string()}}));
  if (!cond_kept) {
    stages.push_back(stage("text-prompt", "text-prompt", Json::array(),
                           {{"text", prompt}}));
  }
  // Per picture a source and its fit; then ONE list of the fitted
  // pictures, which both the conditioner and one vae-encode read -- so
  // latent k is always made from the picture the conditioner sees as k.
  if (read_pics) {
    Json items = Json::array();
    for (std::size_t i = 0; i < feeds.size(); ++i) {
      const auto& f = feeds[i];
      stages.push_back(stage(f.stage, ex::kSourceType, Json::array(),
                             Json::object()));
      stages.push_back(stage(f.stage + "-fit", "image-resample",
                             Json::array({port(f.stage)}), fits[i]));
      items.push_back(port(f.stage + "-fit"));
    }
    stages.push_back(stage(kRefList, "tensor-list", items, Json::object()));
    g.references = feeds;
  }
  // The latents: vae-encode's (0 image, unwired; 1 model; 2 images, the
  // list; its oport 1 the latents list) -- tapped -- or kept.
  Json latents = unwired();
  if (have_refs && latents_kept) {
    stages.push_back(stage(kKeptLatents, ex::kSourceType, Json::array(),
                           Json::object()));
    latents = port(kKeptLatents);
    g.encodings.push_back({latents_key, "latents", true,
                           {{"latents", kKeptLatents, true}}});
  } else if (have_refs) {
    stages.push_back(stage(kRefLatents, "vae-encode",
                           Json::array({unwired(), port("model-select"),
                                        port(kRefList)}),
                           with_vae(codec_accel_of(job), job)));
    latents = port(kRefLatents, 1);
    if (!latents_key.empty()) {
      stages.push_back(sink(kLatentsTap, latents, false));
      g.encodings.push_back({latents_key, "latents", false,
                             {{"latents", kLatentsTap, true}}});
    }
  }
  if (!cfg_stage.empty()) {
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
  // encoder sees pictures (a VLM). Tapped -- or kept.
  Json cond = port("diffusion-conditioner");
  Json negative = cond_negative ? port("diffusion-conditioner", 1)
                                : unwired();
  if (cond_kept) {
    stages.push_back(stage(kKeptConditioning, ex::kSourceType,
                           Json::array(), Json::object()));
    cond = port(kKeptConditioning);
    EncodingIo io{cond_key, "conditioning", true,
                  {{"conditioning", kKeptConditioning}}};
    if (cond_negative) {
      stages.push_back(stage(kKeptNegative, ex::kSourceType, Json::array(),
                             Json::object()));
      negative = port(kKeptNegative);
      io.ports.push_back({"negative", kKeptNegative});
    }
    g.encodings.push_back(std::move(io));
  } else {
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
                           cond_in,
                           with_encoder({{"keep_loaded", keep_models_s(job)}},
                                        job)));
    if (!cond_key.empty()) {
      stages.push_back(sink(kConditioningTap, cond, false));
      EncodingIo io{cond_key, "conditioning", false,
                    {{"conditioning", kConditioningTap}}};
      if (cond_negative) {
        stages.push_back(sink(kNegativeTap, negative, false));
        io.ports.push_back({"negative", kNegativeTap});
      }
      g.encodings.push_back(std::move(io));
    }
  }

  // generate-image: 0 cond, 1 neg, 2 model, 3 sampler, 4 scheduler,
  // 5/6 single ref latents (unused), 7 model_config, 8 ref_latents --
  // the list, from vae-encode's oport 1.
  Json gen_in = Json::array({cond, negative, port("model-select"),
                             unwired(),
                             have_sched ? port("scheduler-select")
                                        : unwired()});
  if (!cfg_stage.empty() || edit) {
    gen_in.push_back(unwired());
    gen_in.push_back(unwired());
    gen_in.push_back(cfg_stage.empty() ? unwired() : port(cfg_stage));
    if (have_refs) {
      gen_in.push_back(latents);
    }
  }
  Json gen_cfg = {{"height", height}, {"width", width}, {"steps", steps},
                  {"seed", seed}, {"keep_loaded", keep_models_s(job)}};
  gen_cfg = merged(gen_cfg, accel_of(job));
  // A community DiT in place of the model's (Favor's Custom).
  if (!job.model.dit.empty()) {
    gen_cfg["dit_dir"] = job.model.dit.string();
  }
  // Run-time LoRAs (Favor's Custom): generate-image takes every slot for
  // every image family.
  lay_loras(gen_cfg, job);
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
                         with_vae(merged({{"dtype", "f16"},
                                          {"keep_loaded", keep_models_s(job)}},
                                         codec_accel_of(job)),
                                  job)));
  // save-image: 0 the image, 1 metadata -- the base's EXIF, carried to
  // the result. Software names Valtz, with vpipe's own line in brackets.
  Json save_in = Json::array({port("vae-decode")});
  for (const auto& f : feeds) {
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
build_text_to_image(const JobSpec& job, const EncodingCache* cache)
{
  return build_image(job, {}, cache);
}

Result<BuiltGraph>
build_image_edit(const JobSpec& job, const std::vector<RefImage>& refs,
                 const EncodingCache* cache)
{
  return build_image(job, refs, cache);
}

namespace {

// The refiner's catalog block (`engine.vpipe.regenerate` of the model
// that names it, laid on the job by the controller as "regenerate_spec").
Json
regenerate_spec(const JobSpec& job)
{
  return jget(job.params, "regenerate_spec", Json::object());
}

// A clip REGENERATED at a larger size (Open-H3-Regenerate-2K, DESIGN §4q;
// vpipe's minimax-h3-regenerate-2k graph): its latent enlarged by the
// latent upscaler -- told the ratio, cut back to the size by a crop box
// where it is not whole latent cells -- put back to the refine's first
// sigma and denoised again by the same DiT under the release's merged
// LoRA, then decoded:
//
//   <latent> -> latent-upscale -11-> generate-video (the refine)
//   regenerate-config (the merged LoRA) -9-> generate-video
//   <cond> -0-> generate-video -> vae-decode
//
// <cond> is the draft's own conditioning -- unless the job carries words
// for the refine (param "regenerate_prompt": Controller::refine_words_),
// read by a conditioner of their own; and where the refine runs another
// model than the draft (ModelRef::regenerate_base: a Ref2VA draft's
// refine is FL2VA's), that model's model-select, DiT and prompt encoder
// take the refine (the VAE stays the job's: one codec).
//
// Its arithmetic `t` (the refiner's tuning: Settings › Upscaling's, or
// a clip's Custom): the DiT's int8 GEMMs and tier, the Neural Engine's
// share, Sol, Sage and Sage's int8 P·V -- the tiers vpipe checked under
// the merged LoRA, Sol and Sage on its banded forward too (vpipe
// 51273173) -- and, off Accurate, the same tier on the upscaler and the
// fast decoder, as vpipe's -fast-m5 graph runs them. The refine writes
// no soundtrack: it is conditioned on an encoded silence. Returns the
// decode's id.
std::string
regenerate_stages(const JobSpec& job, const Json& cond, const Json& latent,
                  int width, int height, int frames, double fps,
                  const Json& t, Json& stages)
{
  const Json spec = regenerate_spec(job);
  const std::string cfg_stage = jget<std::string>(
      jget(job.model.engine, "vpipe", Json::object()), "config_stage",
      "minimax-h3-model-config");
  const bool i8 = jget(t, "i8_gemm", false);
  const auto tier = jget<std::string>(t, "i8_gemm_tier", "accurate");
  Json tiered = Json::object();
  if (i8 && tier != "accurate") {
    tiered = {{"i8_gemm", true}, {"i8_gemm_tier", tier}};
  }
  stages.push_back(stage("latent-upscale", "latent-upscale",
                         Json::array({latent}),
                         merged({{"hf_dir", job.model.latent_upscaler.string()},
                                 {"width", width},
                                 {"height", height},
                                 {"frames", frames}},
                                tiered)));
  // The model the refine runs: the job's, or the refiner's base.
  const bool other = !job.model.regenerate_base.empty();
  const Json bvp = other ? jget(job.model.regenerate_base_engine, "vpipe",
                                Json::object())
                         : jget(job.model.engine, "vpipe", Json::object());
  std::string model = "model-select";
  if (other) {
    model = "regenerate-model-select";
    stages.push_back(stage(model, "model-select", Json::array(),
                           {{"hf_dir", job.model.regenerate_base.string()}}));
  }
  // Its words, where it has its own: that model's prompt encoder.
  Json refine_cond = cond;
  if (const auto words = jget<std::string>(job.params, "regenerate_prompt",
                                           "");
      !words.empty()) {
    stages.push_back(stage("regenerate-prompt", "text-prompt",
                           Json::array(), {{"text", words}}));
    Json ccfg = jget(bvp, "conditioner", Json::object());
    const fs::path enc = other ? job.model.regenerate_base_encoder
                               : job.model.encoder_weights;
    if (!enc.empty()) {
      ccfg["encoder_dir"] = enc.string();
    }
    stages.push_back(stage("regenerate-conditioner", "diffusion-conditioner",
                           Json::array({port("regenerate-prompt"),
                                        unwired(), port(model)}),
                           std::move(ccfg)));
    refine_cond = port("regenerate-conditioner");
  }
  stages.push_back(stage("regenerate-config",
                         jget<std::string>(bvp, "config_stage", cfg_stage),
                         Json::array(),
                         {{"lora", job.model.regenerate_lora.string()},
                          {"lora_scale", 1.0}}));
  Json gen_cfg = merged({{"width", width}, {"height", height},
                         {"frames", frames}, {"fps", fps},
                         {"steps", jget(spec, "steps", 3)},
                         {"seed",
                          jget<std::int64_t>(job.params, "seed", 0)},
                         {"unload_when_idle", "always"}},
                        accel_from(t));
  // A DiT of Custom's (or linked apart) is the job's model's: another's
  // refine runs that model's own -- or its own linked apart.
  if (const fs::path dit = other ? job.model.regenerate_base_dit
                                 : job.model.dit;
      !dit.empty()) {
    gen_cfg["dit_dir"] = dit.string();
  }
  // generate-video: 0 cond, 2 model, 9 model_config, 11 init_latent.
  Json gen_in = Json::array();
  for (int k = 0; k < 12; ++k) {
    gen_in.push_back(k == 0    ? refine_cond
                     : k == 2  ? port(model)
                     : k == 9  ? port("regenerate-config")
                     : k == 11 ? port("latent-upscale")
                               : unwired());
  }
  stages.push_back(stage("regenerate", "generate-video", std::move(gen_in),
                         std::move(gen_cfg)));
  // The released decoder, or -- its Fast Decoder on -- the release's
  // fast one, with the tier the refine runs.
  Json dec = with_vae({{"dtype", "f16"}}, job);
  if (!job.model.fast_decoder.empty()) {
    dec["fast_decoder"] = job.model.fast_decoder.string();
    dec = merged(dec, tiered);
    if (jget(t, "ane_ffn", false)) {
      dec["ane_ffn"] = true;
    }
  }
  const std::string vae = vae_source(job, stages);
  stages.push_back(stage("regenerate-decode", "vae-decode",
                         Json::array({port("regenerate"), port(vae)}),
                         std::move(dec)));
  return "regenerate-decode";
}

// The most a draft's preview holds, its frames 8-bit: past it they are
// made smaller. (The TAE's preview of a 124-frame clip is 55 MB.)
constexpr double kDraftPreviewBytes = 256.0 * 1024 * 1024;

// A regenerated clip's DRAFT as the stage shows it while the refine runs
// (DESIGN §4q), in place of the TAE's last step: its latent goes on to
// the refine undecoded, so it is decoded here too -- by the decoder the
// refine's decode uses, let go once done -- made small enough to hold
// whole (kDraftPreviewBytes) and stacked into ONE clip, [F, 3, H, W],
// for its sink. Returns the sink's id.
std::string
draft_preview_stages(const JobSpec& job, const Json& latent, int width,
                     int height, int frames, Json& stages)
{
  Json dec = with_vae({{"unload_when_idle", "destroy"}}, job);
  if (!job.model.fast_decoder.empty()) {
    dec["fast_decoder"] = job.model.fast_decoder.string();
  }
  const std::string vae = vae_source(job, stages);
  stages.push_back(stage("draft-decode", "vae-decode",
                         Json::array({latent, port(vae)}), std::move(dec)));
  std::string clip = "draft-decode";
  const double bytes = 3.0 * width * height * std::max(frames, 1);
  if (bytes > kDraftPreviewBytes) {
    const double s = std::sqrt(kDraftPreviewBytes / bytes);
    stages.push_back(stage(
        "draft-fit", "image-resample", Json::array({port(clip)}),
        {{"width", std::max(2, static_cast<int>(width * s) / 2 * 2)},
         {"height", std::max(2, static_cast<int>(height * s) / 2 * 2)},
         {"fit", "stretch"},
         {"algorithm", "bilinear"}}));
    clip = "draft-fit";
  }
  // The whole stream, however many frames the decoder makes of it.
  stages.push_back(stage(
      "draft-stack", "temporal-stack", Json::array({port(clip)}),
      {{"mode", "video"},
       {"group_size", 0},
       {"max_mb", static_cast<int>(kDraftPreviewBytes / (1 << 20)) + 64}}));
  stages.push_back(sink("draft-sink", port("draft-stack"),
                        /*latest_only=*/false));
  return "draft-sink";
}

}

// The shape of vpipe's MiniMax H3 reference graphs
// (docs/pipelines/minimax-h3-text-to-video-turbo / -preview /
// -first-last-to-video), with the output written Apple-native:
//
//   text-prompt -> diffusion-conditioner -0-> generate-video
//   [first -> first-fit -> vae-encode] -5-> generate-video
//   [last -> last-fit -> vae-encode] -6-> generate-video
//   <family>-model-config -9-> generate-video
//   generate-video -0-> vae-decode (F16) -> avf-save-video (HEVC 10-bit)
//                  -1-> audio-vae-decode -> save-audio (WAV)
//                  -2-> preview-sink
Result<BuiltGraph>
build_video(const JobSpec& job, const std::optional<RefImage>& first,
            const std::vector<RefMedia>& refs,
            const std::optional<RefImage>& last,
            const EncodingCache* cache)
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
  // A last frame anchors the end of a clip that opens on a first (vpipe
  // reads none alone).
  if (last && !first) {
    return make_error(Code::InvalidArgument,
                      "a clip ends on a picture only if it opens on one");
  }
  if (last && !jget(vp, "last_frame", false)) {
    return make_error(Code::Unsupported, std::format(
        "{} cannot end a clip on a picture", job.model.id));
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
  // Past 768P the clip is DRAFTED at 768 on its short side (param
  // "draft") and REGENERATED at the size asked (DESIGN §4q): the draft's
  // latent itself enlarged and refined -- never decoded -- its
  // soundtrack the clip's.
  const Json draft = jget(job.params, "draft", Json());
  const bool regen = draft.is_object() && !job.model.regenerate_lora.empty();
  if (draft.is_object() && !regen) {
    return make_error(Code::InvalidArgument,
                      "a draft regenerated needs the refiner's files");
  }
  // References (Ref2VA) are the draft's to read; the refine runs on
  // the refiner's model against words of its own. A continuation is not
  // regenerated: its seam is matched to a guide of the clip's own size.
  if (regen && (job.model.latent_upscaler.empty() ||
                std::ranges::any_of(refs, &RefMedia::continues))) {
    return make_error(Code::InvalidArgument,
                      "a continuation is not regenerated");
  }
  // The size the model makes: the draft's, or the clip's.
  const int gen_w = regen ? ceil_to(jget(draft, "width", width), align)
                          : width;
  const int gen_h = regen ? ceil_to(jget(draft, "height", height), align)
                          : height;

  BuiltGraph g;
  const std::string jid = job.id.str();
  const ClipFormat kept = clip_format(job);
  g.output = job.output_dir / std::format("{}.{}", jid, kept.ext);
  g.video_part = job.output_dir / std::format("{}-picture.{}", jid,
                                              kept.ext);
  g.audio_part = job.output_dir / std::format("{}-sound.wav", jid);
  g.sound = kept.sound;
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
  // A reference model asked for a clip from words (catalog
  // `references.prompt_only`): vpipe's prompt-only request, its encoder
  // handed an EMPTY reference list -- the Ref2VA weights refuse a
  // conditioning that carries none (diffusion-conditioner's), and its
  // sequence has no keyframe.
  const bool prompt_only = refs.empty() && rv.is_object() &&
                           jget(rv, "prompt_only", false);
  if (prompt_only && first) {
    return make_error(Code::InvalidArgument, std::format(
        "{} does not open on a picture", job.model.id));
  }

  Json stages = Json::array();
  stages.push_back(stage("model-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.dir.string()}}));
  // The words, read by whichever encoder runs (none, both kept).
  Json prompt_stage = stage("text-prompt", "text-prompt", Json::array(),
                            {{"text", prompt}});
  bool prompt_read = false;
  // What conditions the denoise, and the reference rows when there are
  // references.
  Json cond = port("diffusion-conditioner");
  Json ref_video = unwired(), ref_audio = unwired();
  // The first sound reference, when the clip keeps its sound.
  const RefMedia* song = nullptr;
  // The clip continued, stacked: what the new clip's seam is matched to.
  std::string seam_ref;

  // The pictures the clip opens and ends on: each decoded into a lease
  // at its own size (capped), cropped with Lanczos to fill the frame
  // exactly -- vpipe drops an anchor of another size, and the clip
  // silently becomes text-to-video. The first takes the look the request
  // lays on it (as an edit's base); the last comes as its asset looks.
  const Json keyframe_fit = {{"width", gen_w}, {"height", gen_h},
                             {"fit", "crop"}, {"algorithm", "lanczos"}};
  auto keyframe_feed = [&](const RefImage& pic, const std::string& id,
                           bool opens) {
    RefFeed f;
    f.stage = id;
    f.path = pic.path;
    f.base = true;
    f.channels = 3;
    if (opens) {
      f.adjust = media::adjustments_from_json(
          jget(job.params, "base_adjust", Json::object()));
      f.crop = media::crop_from_json(
          jget(job.params, "base_crop", Json::object()));
    }
    const media::PixelSize own_size = picture_size(pic.size, f.crop);
    const double own =
        static_cast<double>(own_size.width) * own_size.height;
    f.size = own > kMaxFeedArea ? fit_area(own_size, kMaxFeedArea, 1)
                                : own_size;
    f.fitted = {gen_w, gen_h};
    return f;
  };
  // Its picture as fitted, laid once whoever reads it: the VAE that
  // anchors the denoise on it, the prompt encoder that is shown it.
  std::set<std::string> keyframes_laid;
  auto keyframe_picture = [&](const RefImage& pic, const std::string& id,
                              bool opens) {
    if (keyframes_laid.insert(id).second) {
      RefFeed f = keyframe_feed(pic, id, opens);
      stages.push_back(stage(f.stage, ex::kSourceType, Json::array(),
                             Json::object()));
      stages.push_back(stage(id + "-fit", "image-resample",
                             Json::array({port(f.stage)}), keyframe_fit));
      g.references.push_back(std::move(f));
    }
    return port(id + "-fit");
  };
  // Pika H3 Flex's recipe (vpipe's pika-h3-flex graphs): the keyframes
  // are SHOWN to the prompt encoder too, ahead of the words -- FL2VA's
  // own presentation, "<Picture 1>: ", the picture its prompts name.
  const bool show_keyframes =
      refs.empty() && !prompt_only && (first || last) &&
      jget(jget(job.params, "tuning", Json::object()), "pika_flex", false);
  if (refs.empty() && !prompt_only) {
    // The prompt encoder (Qwen3-VL-32B for H3) streams, and is dropped
    // before the denoise: the two never share the machine. KEPT
    // (encoding-cache.h): the words and its config are all it reads --
    // and the keyframes, shown it.
    const Json ccfg = jget(vp, "conditioner", Json::object());
    std::string key;
    if (cache) {
      Json doc = {{"stage", "diffusion-conditioner"},
                  {"config", ccfg},
                  {"prompt", prompt}};
      bool hashed = true;
      if (show_keyframes) {
        Json shown = Json::array();
        for (const auto& [pic, id, opens] :
             {std::tuple{first, "first", true},
              std::tuple{last, "last", false}}) {
          if (pic) {
            hashed = hashed && !pic->hash.is_zero();
            shown.push_back(picture_doc(pic->hash,
                                        keyframe_feed(*pic, id, opens),
                                        keyframe_fit));
          }
        }
        doc["keyframes"] = std::move(shown);
      }
      if (hashed) {
        key = encoding_key("conditioning",
                           encoding_doc(*cache, job, "conditioning", doc));
      }
    }
    if (!key.empty() && cache->kept(key)) {
      stages.push_back(stage(kKeptConditioning, ex::kSourceType,
                             Json::array(), Json::object()));
      cond = port(kKeptConditioning);
      g.encodings.push_back({key, "conditioning", true,
                             {{"conditioning", kKeptConditioning}}});
    } else {
      prompt_read = true;
      // 0 text, 1 negative, 2 model, 3 / 4 the keyframes shown.
      Json cin = Json::array({port("text-prompt"), unwired(),
                              port("model-select")});
      if (show_keyframes) {
        cin.push_back(first ? keyframe_picture(*first, "first", true)
                            : unwired());
        cin.push_back(last ? keyframe_picture(*last, "last", false)
                           : unwired());
      }
      stages.push_back(stage("diffusion-conditioner",
                             "diffusion-conditioner", std::move(cin),
                             with_encoder(ccfg, job)));
      if (!key.empty()) {
        stages.push_back(sink(kConditioningTap, cond, false));
        g.encodings.push_back({key, "conditioning", false,
                               {{"conditioning", kConditioningTap}}});
      }
    }
  } else {
    // vpipe's video-ref-encoder: the prompt and the references, read in
    // order -- its `references` list first, then its six ports, which is
    // the order the job lists them in (the controller's). Each port's
    // stages are gathered apart: KEPT, the encoder and what only it reads
    // go, and the seam's clip stays.
    Json files = Json::array();
    Json enc_in = Json::array({port("text-prompt"), port("model-select")});
    Json attach = Json::array();
    std::vector<std::pair<std::string, Json>> port_stages;  // id, stages
    Json made = Json::array();  // what each reference is, for its key
    bool hashed = true;
    int ref = 0;  // reference ports used, 1..6
    // The references the encoder keeps WHOLE, by their number in the
    // order it reads them -- the list, then the ports, a clip's sound
    // with it: the order they come in. It cuts every other one to the
    // clip's length (`frames`), as the model's makers' code does.
    Json whole = Json::array();
    for (std::size_t k = 0; k < refs.size(); ++k) {
      if (refs[k].whole) {
        whole.push_back(static_cast<int>(k) + 1);
      }
    }
    for (const auto& r : refs) {
      if (r.kind == "audio" && !song) {
        song = &r;
      }
      hashed = hashed && !r.hash.is_zero();
      made.push_back({{"hash", r.hash.hex()}, {"kind", r.kind},
                      {"list", r.list}, {"audio", r.audio},
                      {"first", r.first}, {"count", r.count},
                      {"fps", r.fps}, {"start_s", r.start_s},
                      {"duration_s", r.duration_s}, {"whole", r.whole}});
      if (r.list) {
        files.push_back(r.path.string());
        continue;
      }
      if (r.kind == "video") {
        const std::string id = std::format("ref{}", ++ref);
        Json ps = Json::array();
        ps.push_back(stage(id + "-read", "load-video", Json::array(),
                           {{"input_url", r.path.string()},
                            {"enable_audio", false}}));
        ps.push_back(stage(id + "-rgb", "video-to-rgb",
                           Json::array({port(id + "-read")}),
                           {{"output_dtype", "u8"}}));
        // Exact to the frame (a seek lands on a keyframe).
        std::string last = id + "-rgb";
        if (r.first > 0 || r.count > 0) {
          Json slice = {{"start", r.first}};
          if (r.count > 0) {
            slice["end"] = r.first + r.count;
          }
          ps.push_back(stage(id + "-span", "temporal-slice",
                             Json::array({port(last)}), slice));
          last = id + "-span";
        }
        // The whole span in one beat: its bytes, with room.
        const double mb = static_cast<double>(std::max<std::int64_t>(
                              r.count > 0 ? r.count : 362, 1)) *
                          3.0 * std::max(r.width, 1) *
                          std::max(r.height, 1) / (1024.0 * 1024.0);
        ps.push_back(stage(id, "temporal-stack",
                           Json::array({port(last)}),
                           {{"mode", "video"}, {"group_size", 0},
                            {"max_mb", std::max(256, static_cast<int>(
                                                         mb) + 64)}}));
        port_stages.emplace_back(id, std::move(ps));
        enc_in.push_back(port(id));
        if (r.continues) {
          seam_ref = id;
        }
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
      Json ps = Json::array();
      ps.push_back(stage(id + "-read", "load-audio", Json::array(), read));
      ps.push_back(stage(id + "-pcm", "audio-to-pcm",
                         Json::array({port(id + "-read")}),
                         {{"output_sample_rate", 32000},
                          {"channels", 2},
                          {"chunk_duration_s",
                           r.duration_s > 0 ? r.duration_s : 30.0},
                          {"max_chunk_duration_s", 60.0}}));
      ps.push_back(stage(id, "temporal-stack",
                         Json::array({port(id + "-pcm")}),
                         {{"mode", "audio"}, {"group_size", 0}}));
      port_stages.emplace_back(id, std::move(ps));
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
    if (!attach.empty()) {
      enc["attach_audio"] = attach;
    }
    if (!whole.empty()) {
      enc["whole_references"] = whole;
    }
    // Its key: the words, its config, every reference as content (the
    // files' paths left out: their bytes say what they are).
    std::string key;
    if (cache && hashed) {
      key = encoding_key("references",
                         encoding_doc(*cache, job, "references",
                                      {{"stage", "video-ref-encoder"},
                                       {"config", enc},
                                       {"prompt", prompt},
                                       {"references", made}}));
    }
    // Prompt only, the list is there and empty: left out, the encoder
    // would take the request for an unwired graph and skip it.
    if (!files.empty() || prompt_only) {
      enc["references"] = files;
    }
    static constexpr std::array<const char*, 3> kRefPorts = {
        "conditioning", "ref_video", "ref_audio"};
    if (!key.empty() && cache->kept(key)) {
      EncodingIo io{key, "references", true, {}};
      std::array<Json, 3> fed;
      for (std::size_t k = 0; k < kRefPorts.size(); ++k) {
        const std::string id = std::format("references-kept-{}",
                                           kRefPorts[k]);
        stages.push_back(stage(id, ex::kSourceType, Json::array(),
                               Json::object()));
        io.ports.push_back({kRefPorts[k], id});
        fed[k] = port(id);
      }
      g.encodings.push_back(std::move(io));
      cond = fed[0];
      ref_video = fed[1];
      ref_audio = fed[2];
      // The clip continued is still read: its seam is matched to it.
      for (auto& [id, ps] : port_stages) {
        if (id == seam_ref) {
          for (auto& st : ps) {
            stages.push_back(std::move(st));
          }
        }
      }
    } else {
      prompt_read = true;
      for (auto& [id, ps] : port_stages) {
        for (auto& st : ps) {
          stages.push_back(std::move(st));
        }
      }
      stages.push_back(stage("video-ref-encoder", "video-ref-encoder",
                             std::move(enc_in),
                             with_vae(with_encoder(std::move(enc), job),
                                      job)));
      cond = port("video-ref-encoder", 0);
      ref_video = port("video-ref-encoder", 1);
      ref_audio = port("video-ref-encoder", 2);
      if (!key.empty()) {
        EncodingIo io{key, "references", false, {}};
        for (std::size_t k = 0; k < kRefPorts.size(); ++k) {
          const std::string id = std::format("references-tap-{}",
                                             kRefPorts[k]);
          stages.push_back(sink(id, port("video-ref-encoder",
                                         static_cast<int>(k)), false));
          io.ports.push_back({kRefPorts[k], id});
        }
        g.encodings.push_back(std::move(io));
      }
    }
  }
  if (prompt_read) {
    stages.insert(stages.begin() + 1, std::move(prompt_stage));
  }
  const bool keep_song = song && jget(job.params, "reference_sound", false);

  // The family's own knobs -- its sigma shifts, the run-time adapter, the
  // preview TAE -- ride one config beat into generate-video's port 9.
  Json cfg = jget(vp, "config", Json::object());
  lay_loras(cfg, job);
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
    // runs the streaming method when on, and nothing else ever does --
    // but as a plain LoRA (`taomate_lora`): the ordinary denoise with it.
    if (t.is_object() && t.contains("taomate") &&
        t["taomate"].is_boolean()) {
      const bool method = t["taomate"].get<bool>() &&
                          !jget(t, "taomate_lora", false);
      cfg["taomate"] = method ? "on" : "off";
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

  // Each keyframe VAE-encoded, its latent the anchor.
  auto keyframe = [&](const RefImage& pic, const std::string& id,
                      bool opens) {
    // KEPT (encoding-cache.h): the picture as fitted is all it reads.
    std::string key;
    if (cache && !pic.hash.is_zero()) {
      key = encoding_key("keyframe",
                         encoding_doc(*cache, job, "keyframe",
                                      {{"stage", "vae-encode"},
                                       {"picture",
                                        picture_doc(pic.hash,
                                                    keyframe_feed(pic, id,
                                                                  opens),
                                                    keyframe_fit)}}));
    }
    if (!key.empty() && cache->kept(key)) {
      const std::string kept = id + "-latent-kept";
      stages.push_back(stage(kept, ex::kSourceType, Json::array(),
                             Json::object()));
      g.encodings.push_back({key, "keyframe", true, {{"latent", kept}}});
      return port(kept);
    }
    stages.push_back(stage(id + "-latent", "vae-encode",
                           Json::array({keyframe_picture(pic, id, opens),
                                        port("model-select")}),
                           with_vae({{"unload_when_idle", "always"}}, job)));
    if (!key.empty()) {
      const std::string tap = id + "-latent-tap";
      stages.push_back(sink(tap, port(id + "-latent"), false));
      g.encodings.push_back({key, "keyframe", false, {{"latent", tap}}});
    }
    return port(id + "-latent");
  };
  Json anchor = unwired(), anchor_last = unwired();
  if (first) {
    anchor = keyframe(*first, "first", true);
  }
  if (last) {
    anchor_last = keyframe(*last, "last", false);
  }

  // generate-video: 0 cond, 1 neg (inert: H3 is guidance-distilled),
  // 2 model, 3 sampler, 4 scheduler (H3 brings its own), 5 / 6 first /
  // last keyframe, 7 / 8 Ref2VA rows, 9 model_config.
  Json gen_in = Json::array({cond, unwired(), port("model-select"),
                             unwired(), unwired(), anchor, anchor_last,
                             ref_video, ref_audio, port(cfg_stage)});
  Json gen_cfg = {{"width", gen_w}, {"height", gen_h}, {"frames", frames},
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

  // The picture: decoded in F16 and written in the project's intermediate
  // format -- ProRes (12 bits in 4444, 10 in 422) or HEVC Main10, never
  // the eight the FFmpeg path rounds to -- tagged BT.709, which is what
  // the model's sRGB-encoded BT.709 samples are taken as in video. A
  // draft is not decoded: its latent is regenerated at the size asked,
  // and that is decoded.
  std::string decoded = "vae-decode";
  if (regen) {
    decoded = regenerate_stages(job, cond, port("generate-video"), width,
                                height, frames, fps,
                                jget(job.params, "regenerate_tuning",
                                     Json::object()),
                                stages);
    if (jget(job.params, "preview", true) &&
        jget(job.params, "draft_preview", true)) {
      g.draft_sink = draft_preview_stages(job, port("generate-video"),
                                          gen_w, gen_h, frames, stages);
    }
  } else {
    const std::string vae = vae_source(job, stages);
    stages.push_back(stage("vae-decode", "vae-decode",
                           Json::array({port("generate-video"), port(vae)}),
                           with_vae({{"dtype", "f16"}}, job)));
  }
  // A continuation's SEAM: its frames matched to the tail the model was
  // given -- the step in tone (the shadows most) and in framing it came
  // back with, measured on the frames either side and taken out of every
  // frame (vpipe's content-aware-continuation, catalog
  // `references.seam`). What does not look like one shot it leaves as
  // it is, and says so.
  std::string picture = decoded;
  if (const Json seam = jget(rv, "seam", Json());
      !seam_ref.empty() && seam.is_object()) {
    stages.push_back(stage("seam", "content-aware-continuation",
                           Json::array({port(decoded),
                                        port(seam_ref)}),
                           seam));
    picture = "seam";
  }
  Json save = {{"path", g.video_part.string()},
               {"codec", kept.codec},
               {"alpha", "drop"},
               {"fps", fps},
               {"color_primaries", 1},
               {"color_transfer", 1},
               {"color_matrix", 1}};
  if (!media::is_prores(kept.codec)) {
    save["quality"] = jget(vp, "quality", 0.8);
  }
  stages.push_back(stage("save-video", "avf-save-video",
                         Json::array({port(picture)}), save));
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
                           with_vae(Json::object(), job)));
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
// back in memory -- and MiniMax-Music3's (minimax-music3-text-to-music),
// the same shape: its description the style, its lyrics the beat, its
// vocoder the model's own folder (catalog `decoder`: itself), no score
// planned, so the score sink hears nothing:
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
  // A model that plans no score (MiniMax-Music3: catalog song.score
  // false) is given no plan; YuE2's is its `cot`.
  const Json sb = jget(vp, "song", Json::object());
  const bool plans = jget(sb, "score", true);
  if (jget(sb, "lyrics_required", false) && lyrics.empty()) {
    return make_error(Code::InvalidArgument, std::format(
        "{} needs lyrics", job.model.id));
  }
  const std::string cot = jget<std::string>(
      job.params, "cot", jget<std::string>(defaults, "cot", "full"));
  if (plans && cot != "full" && cot != "melody" && cot != "off") {
    return make_error(Code::InvalidArgument, std::format(
        "'{}' is not a score plan (full, melody or off)", cot));
  }
  const std::string abc = jget<std::string>(job.params, "abc", "");
  if (!abc.empty() && (!plans || cot == "off")) {
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
                  {"seed", jget<std::int64_t>(job.params, "seed", 0)},
                  {"ode_steps", jget(job.params, "steps",
                                     jget(defaults, "steps", 32))}};
  if (plans) {
    gen_cfg["cot"] = cot;
  }
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
      const std::string k =
          it.key() == "sol_tau" || it.key() == "sol_threshold"
              ? "sol_attn"
              : it.key();
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
namespace {

// DRAMA 3 (vpipe's drama3-scene graph): the script in as one beat --
// read as written, its speaker tags and directions the model's own -- its
// voices from their files with the words said in them, one a speaker in
// order (a transcript's leading tag names its speaker; speaker 0 needs
// none), spoken in parts as Fish Audio's own server cuts a long text, a
// part's history the first and the latest, written as one WAV:
//
//   text-prompt (the script) -0-> text-to-speech -0-> save-audio (WAV)
//   sampler (the seed; the makers' own sampling) -2-> text-to-speech
//
// The model decides each part's length; max_frames caps a part (600:
// 28 s, the server's), so a take that never ends costs at most that.
Result<BuiltGraph>
build_drama3(const JobSpec& job, const Json& sp,
             const std::vector<fs::path>& voices)
{
  const std::string text = jget<std::string>(job.params, "text", "");
  if (text.empty()) {
    return make_error(Code::InvalidArgument, "speech needs its words");
  }
  const Json vs = jget(job.params, "voices", Json::array());
  if (voices.size() != (vs.is_array() ? vs.size() : 0)) {
    return make_error(Code::InvalidArgument,
                      "each voice needs its file and its words");
  }
  const double rate = jget(sp, "sample_rate", 44100.0);
  BuiltGraph g;
  const std::string jid = job.id.str();
  g.output = job.output_dir / std::format("{}.wav", jid);

  Json refs = Json::array();
  double longest = jget(sp, "voice_seconds", 12.0);
  for (std::size_t i = 0; i < voices.size(); ++i) {
    const std::string words = jget<std::string>(vs[i], "text", "");
    if (words.empty()) {
      return make_error(Code::InvalidArgument,
                        "a voice to clone needs the words said in it");
    }
    refs.push_back({{"audio", voices[i].string()},
                    {"text", i == 0 ? words
                                    : std::format("<|speaker:{}|> {}", i,
                                                  words)}});
    longest = std::max(longest, jget(vs[i], "seconds", 0.0));
  }
  Json stages = Json::array();
  stages.push_back(stage("text", "text-prompt", Json::array(),
                         {{"text", text}}));
  // The makers' own sampling (their server's request defaults), seeded.
  stages.push_back(stage("sampler", "sampler-select", Json::array(),
                         {{"temperature", 1.0},
                          {"top_p", 0.9},
                          {"top_k", 30},
                          {"seed", jget<std::int64_t>(job.params, "seed",
                                                      0)}}));
  Json cfg = {{"hf_dir", job.model.dir.string()},
              {"chunk_text", true},
              {"context", "text"},
              {"max_frames", jget(sp, "max_frames", 600)},
              // A voice is cut here to the words said in it; the stage
              // keeps it whole.
              {"voice_ref_seconds", std::ceil(longest + 0.5)},
              {"stream_chunk_frames", 0},
              {"interrupt_on_new_text", false}};
  if (!refs.empty()) {
    cfg["references"] = std::move(refs);
  }
  cfg = merged(cfg, declared_of(job, "speak"));
  stages.push_back(stage("speak", "text-to-speech",
                         Json::array({port("text"), nullptr,
                                      port("sampler")}),
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

}

Result<BuiltGraph>
build_speech(const JobSpec& job, const std::vector<fs::path>& voices)
{
  const Json vp = jget(job.model.engine, "vpipe", Json::object());
  const Json sp = jget(vp, "speech", Json());
  if (!sp.is_object()) {
    return make_error(Code::Unsupported, std::format(
        "{} has no vpipe recipe for {}", job.model.id, job.op));
  }
  if (jget<std::string>(sp, "format", "") == "drama3") {
    return build_drama3(job, sp, voices);
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
// so vpipe neither infers nor registers it. Its scales written in BF16,
// the dtype every model here runs in, and the codes fit to them (vpipe
// 9f74a0c2's `run_dtype`): an F16 pair fit to the f32 scale, rounded to
// bf16 at load, left the codes up to half a step off the scale the
// dequant uses -- MOSS-Image's w8 pack twice as far from its reference.
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
  const Json base = {
      {"bits", jget(job.params, "bits", 8)},
      {"group_size", jget(job.params, "group_size", 64)},
      {"run_dtype", jget<std::string>(job.params, "run_dtype", "bf16")},
      {"skip_existing", false}};
  const Json parts = jget(job.params, "parts", Json::array());
  if (!parts.is_array() || parts.empty()) {
    Json cfg = base;
    cfg["src_model"] = job.model.dir.string();
    cfg["output_name"] = out;
    stages.push_back(stage("quantize", "model-quantize", Json::array(),
                           std::move(cfg)));
  }
  std::string src = job.model.dir.string();
  for (std::size_t i = 0; parts.is_array() && i < parts.size(); ++i) {
    const std::string target = jget<std::string>(parts[i], "target", "");
    if (target.empty()) {
      return make_error(Code::InvalidArgument,
                        "a compressed part names no target");
    }
    const std::string to = i + 1 == parts.size()
        ? out : std::format("{}.pass{}", out, i + 1);
    // The part's own keys (AWQ, the modulation) under the job's: the
    // source, the output and the width.
    Json cfg = jget(parts[i], "options", Json::object());
    if (!cfg.is_object()) {
      cfg = Json::object();
    }
    cfg.update(base);
    cfg["src_model"] = src;
    cfg["output_name"] = to;
    cfg["target"] = target;
    const std::string id = std::format("quantize-{}", i + 1);
    stages.push_back(stage(id, "model-quantize",
                           i == 0 ? Json::array()
                                  : Json::array({port(std::format(
                                        "quantize-{}", i))}),
                           std::move(cfg)));
    src = to;
  }
  g.spec = {{"id", "valtz-" + job.id.str()},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

namespace {

// text-chat as the helper runs: loaded, kept and decoding as Settings
// say, its K/V pages alike for every use (one model warm for all).
Json
helper_chat_config(const JobSpec& job)
{
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
    // chunks for text-to-speech): the reply is watched as it is written.
    {"stream_words", 0},
    // Its K/V pages as every use of the helper has them: one model warm
    // for all.
    {"page_tokens", kHelperPageTokens},
    {"max_pages", helper_max_pages(job)},
  };
  // A long prefill's GEMMs in int8 (vpipe's i8_prefill), where the model
  // asks: Flash-Next's 7k-token prompt 2,900 -> 3,000 tok/s on an M5 Ultra.
  if (jget(job.params, "i8_prefill", false)) {
    chat_cfg["i8_prefill"] = true;
  }
  // The most it may hold (the agent's Memory: vpipe's memory_limit_mb,
  // DESIGN §15 0zzd) -- a Mixture of Experts' cache sized within it.
  if (const auto mb = jget<std::int64_t>(job.params, "memory_limit_mb", 0);
      mb > 0) {
    chat_cfg["memory_limit_mb"] = mb;
  }
  // Its experts' cache as asked (the Agent's Expert Cache: vpipe's
  // expert_cache_mb), at most that within the Memory and the Mac.
  if (const auto mb = jget<std::int64_t>(job.params, "expert_cache_mb", 0);
      mb > 0) {
    chat_cfg["expert_cache_mb"] = mb;
  }
  // The dtype it computes in where the model wants one (Ternary-Bonsai's
  // f16, its scales' own); else vpipe's bf16.
  if (const auto d = jget<std::string>(job.params, "compute_dtype", "");
      !d.empty()) {
    chat_cfg["compute_dtype"] = d;
  }
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
  return chat_cfg;
}

// The helper's own sampler (catalog engine.vpipe.sampling) on text-chat's
// sampler port, its stage pushed onto `stages`; false when it has none
// (greedy).
bool
helper_sampler(const JobSpec& job, Json& stages)
{
  const Json smp = jget(job.params, "sampling", Json::object());
  if (!smp.is_object() || smp.empty()) {
    return false;
  }
  Json cfg = Json::object();
  for (const char* k : {"temperature", "top_k", "top_p", "min_p",
                        "repetition_penalty", "presence_penalty"}) {
    if (smp.contains(k) && smp[k].is_number()) {
      cfg[k] = smp[k];
    }
  }
  cfg["seed"] = jget<std::uint64_t>(smp, "seed", 0);
  stages.push_back(stage("sampler", "sampler-select", Json::array(), cfg));
  return true;
}

}  // namespace

// An agent's turn (kOpAgentTurn): the whole conversation as text-chat's
// conversation beat -- a FlexData object, through text-prompt's `value`
// -- its pictures in the last user message as media markers.
Result<BuiltGraph>
build_agent_turn(const JobSpec& job)
{
  Json conv = jget(job.params, "conversation", Json::object());
  if (!conv.is_object() || !conv.contains("messages") ||
      !conv["messages"].is_array() || conv["messages"].empty()) {
    return make_error(Code::InvalidArgument, "the conversation is empty");
  }
  // Each message's own pictures (its "images": indexes among the job's
  // image inputs) ahead of its words -- the same text every turn, which
  // is how vpipe knows the context it kept holds that message.
  std::vector<const JobInput*> images;
  for (const auto& in : job.inputs) {
    if (in.role == "image") {
      images.push_back(&in);
    }
  }
  for (Json& m : conv["messages"]) {
    std::string pictures;
    for (const auto& i : jget(m, "images", Json::array())) {
      const auto k = i.is_number_integer() ? i.get<std::size_t>()
                                           : images.size();
      if (k < images.size()) {
        pictures += std::format("<|__vpipe_fs_im_start__|>{}"
                                "<|__vpipe_fs_im_end__|>",
                                images[k]->path.string());
      }
    }
    m.erase("images");
    if (!pictures.empty()) {
      m["content"] = pictures + "\n" + jget<std::string>(m, "content", "");
    }
  }
  BuiltGraph g;
  g.text_sink = "text-sink";
  g.result_sink = "result-sink";
  Json chat_cfg = helper_chat_config(job);
  chat_cfg["max_new_tokens"] = jget(conv, "max_new_tokens",
                                    jget(job.params, "max_new_tokens", 2048));
  Json stages = Json::array();
  stages.push_back(stage("turn", "text-prompt", Json::array(),
                         {{"value", conv}}));
  Json chat_in = Json::array({port("turn")});
  if (helper_sampler(job, stages)) {
    chat_in.push_back(port("sampler"));
  }
  stages.push_back(stage("text-chat", "text-chat", chat_in, chat_cfg));
  stages.push_back(sink(g.result_sink, port("text-chat", 0), false));
  stages.push_back(sink(g.text_sink, port("text-chat", 1), false));
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

  const Json chat_cfg = helper_chat_config(job);
  Json stages = Json::array();
  stages.push_back(stage("text-prompt", "text-prompt", Json::array(),
                         {{"text", text}}));
  // The model's own sampler (catalog engine.vpipe.sampling: Qwen's
  // recommendation), on text-chat's sampler port; none, greedy. Its seed
  // 0 is a fresh one each time: asked again, the assistant may answer
  // otherwise.
  Json chat_in = Json::array({port("text-prompt")});
  if (helper_sampler(job, stages)) {
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
  const std::string read = kExportRead + suffix;
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
  stages.push_back(stage(kExportWrite + suffix, "save-image",
                         std::move(save_in), std::move(cfg)));
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
    m.stage = kExportRead;
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
    Json stages = Json::array();
    stages.push_back(stage(kExportRead, ex::kSourceType, Json::array(),
                           Json::object()));
    stages.push_back(stage(kExportWrite, "avf-save-video",
                           Json::array({port(kExportRead)}),
                           video_writer({{"path", written.string()},
                                         {"fps", rate.to_double()},
                                         {"frames", g.frames},
                                         {"progress", kExportReport}},
                                        job.params, name,
                                        rate.to_double())));
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
      m.stage = kExportRead;
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
      stages.push_back(stage(kExportRead, ex::kSourceType, Json::array(),
                             Json::object()));
    } else if (!adj.identity() || !crop.identity() || in.info.turn > 0) {
      // Turned (a phone's portrait clip), it is drawn upright by Valtz:
      // vpipe's reader takes its picture as stored, its writer writes no
      // turn.
      MovieFeed m;
      m.stage = kExportRead;
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
      stages.push_back(stage(kExportRead, ex::kSourceType, Json::array(),
                             Json::object()));
    } else {
      stages.push_back(stage(kExportRead, "avf-load-video", Json::array(),
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
    Json write = {{"path", written.string()}, {"fps", fps},
                  {"progress", kExportReport}};
    if (g.frames > 0) {
      write["frames"] = g.frames;
    }
    stages.push_back(stage(kExportWrite, "avf-save-video",
                           Json::array({port(kExportRead)}),
                           video_writer(std::move(write), job.params, name,
                                        fps)));
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
    f.stage = kExportRead;
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
    stages.push_back(stage(kExportRead, ex::kSourceType, Json::array(),
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
    Json save_in = Json::array({port(kExportRead)});
    if (name != "exr") {
      g.exif_from = in.path;
      stages.push_back(stage(kExifSource, ex::kSourceType, Json::array(),
                             Json::object()));
      save_in.push_back(port(kExifSource));
    }
    stages.push_back(stage(kExportWrite, "save-image", std::move(save_in),
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
  // A group is a unit of memory, not a clip: the source encoder carries
  // one STREAM across the groups -- its projection, the denoiser's
  // windows and the decode's carries -- or, where a stream does not fit
  // the box, cuts the source into overlapping CLIPS of `clip_frames`
  // whose shared `blend_frames` the decode cross-fades (vpipe decides:
  // `continuity` auto). Either way it cuts the source itself, so the
  // groups are consecutive, any size, and the last as short as the clip
  // leaves it: the encoder pads the source's end and drops what that
  // makes.
  const int G = std::max(8, jget(job.params, "group", jget(up, "group", 24)));
  // A release's cadence, which no tensor says: the source frames its
  // stream's opening chunk reads, then each chunk after -- v1.1 25 then
  // 8, FlashVSR+ 17 then 16 (four latent frames a chunk from the first).
  const int open = std::max(5, jget(up, "open_frames", 25));
  const int chunk = std::max(4, jget(up, "chunk_frames", 8));
  const int L = std::max(open, jget(up, "clip_frames", 41));
  const int B = std::clamp(jget(up, "blend_frames", 8), 0, L - open);
  // What it holds back of the frames given: a stream's opening chunk
  // reads `open` frames, a clip L, before any comes out.
  const int lag = L - 1;
  // The most source frames one beat restores -- what the denoiser is
  // planned for: a clip; or a stream's first beat (its opening chunk
  // and the chunks the first groups complete past it), or a later one
  // (a group's chunks).
  const int first_beat = [&] {
    const int fed = (open + G - 1) / G * G;
    return open + chunk * ((fed - open) / chunk);
  }();
  const int beat_frames =
      std::max({L, first_beat, chunk * ((G + chunk - 1) / chunk)});
  // FlashVSR+'s tiny decoder reads the source pictures beside each
  // latent -- they travel from the encoder to the decode on a port of
  // their own (`color_reference`, the wavelet colour fix's too); unwired,
  // vpipe decodes with the Wan VAE, about three times slower. v1.1 sends
  // none: unwired, as vpipe's own v1.1 graph leaves it.
  const bool pictures = jget(up, "color_reference", false);
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

  BuiltGraph g;
  const std::string jid = job.id.str();
  const ClipFormat kept = clip_format(job);
  g.output = job.output_dir / std::format("{}.{}", jid, kept.ext);
  g.sound = kept.sound;
  fs::path written = g.output;
  if (in.info.has_audio) {
    g.video_part = job.output_dir / std::format("{}-picture.{}", jid,
                                                kept.ext);
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
  // FlashVSR+'s pictures to the decode are its frames at the model's
  // size once more.
  const std::uint64_t group_bytes =
      (pictures ? 3 : 2) * G * px * 3 +
      static_cast<std::uint64_t>((G - 1) / 4 + 1) * (px / 256) * hidden * 2 +
      static_cast<std::uint64_t>(G) * 3 * 2 *
          (px + static_cast<std::uint64_t>(W) * H);
  m.gate = {"tap", G, lag,
            std::max(0, jget(job.params, "ahead", jget(up, "ahead", 0))),
            group_bytes};
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
  // No overlap: the encoder cuts the source itself (one it was given
  // twice it would drop, by the stack's `repeated`).
  stages.push_back(stage("stack", "temporal-stack",
                         Json::array({port("upsample")}),
                         {{"mode", "video"}, {"group_size", G},
                          {"overlap", 0}, {"max_mb", group_mb}}));
  // Its colour fix (Settings › Upscaling, a declared option on this
  // stage): each frame's colour taken back toward its source's -- v1.1's
  // AdaIN, FlashVSR+'s wavelet.
  stages.push_back(stage("encode", "flashvsr-src-encoder",
                         Json::array({port("stack"), port("model-select")}),
                         merged({{"clip_frames", L}, {"blend_frames", B}},
                                declared_of(job, "encode"))));
  stages.push_back(stage("generate", "generate-video",
                         Json::array({port("encode"), unwired(),
                                      port("model-select")}),
                         merged({{"width", PW},
                                 {"height", PH},
                                 {"frames", beat_frames},
                                 {"seed", jget<std::int64_t>(job.params,
                                                             "seed", 0)},
                                 {"unload_when_idle", "auto"}},
                                accel_of(job))));
  Json decode_in = Json::array({port("generate"), port("model-select")});
  if (pictures) {
    decode_in.push_back(port("encode", 1));
  }
  stages.push_back(stage("decode", "vae-decode", std::move(decode_in),
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
  // the clip's own (the encoder drops its padding's: a guard).
  stages.push_back(stage("tap", ex::kTapType, Json::array({port(last)}),
                         {{ex::kTapMaxFrames, count}}));
  stages.push_back(stage("write", "avf-save-video",
                         Json::array({port("tap")}),
                         {{"path", written.string()},
                          {"codec", kept.codec},
                          {"alpha", "drop"},
                          {"fps", fps},
                          {"frames", count}}));
  g.spec = {{"id", "valtz-" + jid},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

Result<BuiltGraph>
build_regenerate_video(const JobSpec& job)
{
  if (job.inputs.size() != 1 ||
      job.inputs.front().info.type != media::MediaType::Video) {
    return make_error(Code::InvalidArgument,
                      "a regenerate takes one clip (role \"source\")");
  }
  if (job.model.regenerate_lora.empty() ||
      job.model.latent_upscaler.empty()) {
    return make_error(Code::InvalidArgument,
                      "a regenerate needs the refiner's files");
  }
  const JobInput& in = job.inputs.front();
  const int W = jget(job.params, "width", 0);
  const int H = jget(job.params, "height", 0);
  const Json enc = jget(job.params, "encode", Json::object());
  const int ew = jget(enc, "width", 0);
  const int eh = jget(enc, "height", 0);
  if (W <= 0 || H <= 0 || ew <= 0 || eh <= 0) {
    return make_error(Code::InvalidArgument,
                      "a regenerate needs its size and the size it reads");
  }
  const std::string prompt = jget<std::string>(job.params, "prompt", "");
  if (prompt.empty()) {
    return make_error(Code::InvalidArgument, "prompt is empty");
  }
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

  BuiltGraph g;
  const std::string jid = job.id.str();
  const ClipFormat kept = clip_format(job);
  g.output = job.output_dir / std::format("{}.{}", jid, kept.ext);
  g.sound = kept.sound;
  fs::path written = g.output;
  if (in.info.has_audio) {
    g.video_part = job.output_dir / std::format("{}-picture.{}", jid,
                                                kept.ext);
    g.audio_part = in.path;
    g.audio_is_source = true;
    g.audio_start = static_cast<double>(first) / fps;
    g.audio_duration = static_cast<double>(count) / fps;
    written = g.video_part;
  }
  g.width = W;
  g.height = H;
  g.frames = count;

  // The clip whole, in 8-bit, upright, at its own size: the VAE reads it
  // in one beat (its posterior sampled, as the release samples a draft's).
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
  g.movie = std::move(m);

  const Json vp = jget(job.model.engine, "vpipe", Json::object());
  const Json spec = regenerate_spec(job);
  Json stages = Json::array();
  stages.push_back(stage("read", ex::kSourceType, Json::array(),
                         Json::object()));
  stages.push_back(stage("model-select", "model-select", Json::array(),
                         {{"hf_dir", job.model.dir.string()}}));
  // Fitted to the size it is read at -- its short side at most 768, as
  // the release fits a draft (cover-scaled, bilinear, centre-cropped).
  std::string fitted = "read";
  if (ew != in.info.frame.width || eh != in.info.frame.height) {
    stages.push_back(stage("fit", "image-resample",
                           Json::array({port("read")}),
                           {{"width", ew}, {"height", eh}, {"fit", "crop"},
                            {"algorithm", "bilinear"}}));
    fitted = "fit";
  }
  const std::int64_t clip_mb =
      count * ew * eh * 3 / (1 << 20) + 64;
  stages.push_back(stage("stack", "temporal-stack",
                         Json::array({port(fitted)}),
                         {{"mode", "video"}, {"group_size", 0},
                          {"max_mb", clip_mb}}));
  stages.push_back(stage("encode", "vae-encode",
                         Json::array({port("stack"), port("model-select")}),
                         with_vae({{"posterior", "sample"},
                                   {"posterior_seed",
                                    jget(spec, "posterior_seed", 42)},
                                   {"unload_when_idle", "destroy"}},
                                  job)));
  // Its words, read by the model's own encoder (dropped before the
  // refine: the two never share the machine).
  stages.push_back(stage("text-prompt", "text-prompt", Json::array(),
                         {{"text", prompt}}));
  stages.push_back(stage("diffusion-conditioner", "diffusion-conditioner",
                         Json::array({port("text-prompt"), unwired(),
                                      port("model-select")}),
                         with_encoder(jget(vp, "conditioner",
                                           Json::object()),
                                      job)));
  const std::string decoded = regenerate_stages(
      job, port("diffusion-conditioner"), port("encode"), W, H,
      static_cast<int>(count), fps,
      jget(job.params, "tuning", Json::object()), stages);
  // The upscaler makes whole 32-pixel cells, rounded as Python rounds: a
  // size rounded up comes back cut to the size (its crop box), one
  // rounded down is fitted to it here.
  auto cells = [](int v) {
    return static_cast<int>(std::nearbyint(std::nearbyint(v / 32.0) * 2)) *
           16;
  };
  std::string shown = decoded;
  if (cells(W) < W || cells(H) < H) {
    stages.push_back(stage("fit-out", "image-resample",
                           Json::array({port(decoded)}),
                           {{"width", W}, {"height", H}, {"fit", "stretch"},
                            {"algorithm", "lanczos"}}));
    shown = "fit-out";
  }
  // Cut at the clip's own frames (the encoder pads a length the VAE does
  // not take; the refine says how many are real): a guard.
  stages.push_back(stage("tap", ex::kTapType, Json::array({port(shown)}),
                         {{ex::kTapMaxFrames, count}}));
  // The model's samples, tagged as a clip it makes is: BT.709.
  Json write = {{"path", written.string()},
                {"codec", kept.codec},
                {"alpha", "drop"},
                {"fps", fps},
                {"frames", count},
                {"color_primaries", 1},
                {"color_transfer", 1},
                {"color_matrix", 1}};
  if (!media::is_prores(kept.codec)) {
    write["quality"] = jget(vp, "quality", 0.8);
  }
  stages.push_back(stage("write", "avf-save-video",
                         Json::array({port("tap")}), std::move(write)));
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
                         // Settings › Upscaling's arithmetic (the
                         // recipe's tuning): int8 GEMMs, Sage, Sol.
                         merged({{"seed", jget<std::int64_t>(job.params,
                                                             "seed", 0)}},
                                accel_of(job))));
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

// The sound's packets (load-audio) decoded into 32 ms pieces of PCM
// (audio-to-pcm), which the voice detector (audio-segment: Silero) cuts
// into stretches of speech for the speech model (audio-transcribe:
// Qwen3-ASR), a line each to the transcript's sink; the same PCM, with
// a tagger, into AudioSet's labels every 8 s over 10 s (audio-tagging:
// BEATs) to the events' sink. A file is read faster than it plays: the
// transcriber keeps the longest PCM it can (300 s) and takes every
// stretch, however late it comes, rather than drop one.
Result<BuiltGraph>
build_transcribe(const JobSpec& job, const fs::path& wav)
{
  const auto vad = jget<std::string>(job.params, "vad_path", "");
  const auto tagger = jget<std::string>(job.params, "tagger_path", "");
  if (job.model.dir.empty() || vad.empty()) {
    return make_error(Code::InvalidArgument,
                      "a transcription needs its speech model and its "
                      "voice detector");
  }
  BuiltGraph g;
  g.transcript_sink = "transcript-sink";
  Json stages = Json::array();
  stages.push_back(stage("load", "load-audio", Json::array(),
                         {{"input_url", wav.string()}}));
  // A file decodes far faster than the voice detector reads its 32 ms
  // chunks: the PCM waits for it, a few seconds ahead at most (on the
  // default depth it ran 2048 chunks -- 65 s -- ahead, gave up, and the
  // transcription never ended: §15 0zr).
  stages.push_back(stage("pcm", "audio-to-pcm",
                         Json::array({port("load")}),
                         {{"output_sample_rate", 16000},
                          {"chunk_duration_s", 0.032},
                          {"oport_capacity", 256},
                          {"channels", 1}}));
  stages.push_back(stage("voice", "audio-segment",
                         Json::array({port("pcm")}),
                         {{"model_path", vad}}));
  Json asr = {{"hf_dir", job.model.dir.string()},
              {"pcm_buffer_s", 300.0},
              {"late_marker_skip", false}};
  if (const auto lang = jget<std::string>(job.params, "language", "");
      !lang.empty()) {
    asr["language_hint"] = lang;
  }
  stages.push_back(stage("asr", "audio-transcribe",
                         Json::array({port("pcm"), port("voice")}),
                         std::move(asr)));
  stages.push_back(sink(g.transcript_sink, port("asr"), false));
  if (!tagger.empty()) {
    g.events_sink = "events-sink";
    stages.push_back(stage("tags", "audio-tagging",
                           Json::array({port("pcm")}),
                           {{"model_path", tagger},
                            {"model_kind", "beats"},
                            {"top_k", 5},
                            {"score_threshold", 0.1}}));
    stages.push_back(sink(g.events_sink, port("tags"), false));
  }
  g.spec = {{"id", "valtz-" + job.id.str()},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

// The sound's packets (load-audio) decoded into 10 s pieces of PCM at
// the model's rate (audio-to-pcm: the wav is that rate already, two
// channels), separated as they come by vpipe's audio-separate -- MSST's
// overlapping chunks cross-faded, the output trailing the input by about
// a chunk -- its two outputs streamed to sinks of their own: what the
// model takes out (`stem`) and the mix without it (`residual`).
Result<BuiltGraph>
build_separate(const JobSpec& job, const fs::path& wav)
{
  const Json sep = jget(jget(job.model.engine, "vpipe", Json::object()),
                        "separate", Json::object());
  const int rate = jget(sep, "rate", 44100);
  if (job.model.dir.empty() || wav.empty()) {
    return make_error(Code::InvalidArgument,
                      "a separation needs its model and a sound");
  }
  BuiltGraph g;
  g.stem_sink = "stem-sink";
  g.residual_sink = "residual-sink";
  Json stages = Json::array();
  stages.push_back(stage("load", "load-audio", Json::array(),
                         {{"input_url", wav.string()}}));
  stages.push_back(stage("pcm", "audio-to-pcm",
                         Json::array({port("load")}),
                         {{"output_sample_rate", rate},
                          {"channels", 2},
                          {"chunk_duration_s", 10.0},
                          {"flush_on_eos", true}}));
  // The checkpoint by its folder and its file: one repository may hold
  // several models (BS-RoFormer Resurrection: vocals and instrumental).
  Json cfg = {{"hf_dir", job.model.dir.string()}};
  if (const auto ckpt = jget<std::string>(job.params, "checkpoint", "");
      !ckpt.empty()) {
    cfg["checkpoint"] = ckpt;
  }
  stages.push_back(stage("separate", "audio-separate",
                         Json::array({port("pcm")}), std::move(cfg)));
  stages.push_back(sink(g.stem_sink, port("separate", 0), false));
  stages.push_back(sink(g.residual_sink, port("separate", 1), false));
  g.spec = {{"id", "valtz-" + job.id.str()},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

Result<BuiltGraph>
build_summarize_video(const JobSpec& job, media::PixelSize size)
{
  if (job.inputs.size() != 1 ||
      job.inputs.front().info.type != media::MediaType::Video) {
    return make_error(Code::InvalidArgument,
                      "a summary takes one clip (role \"source\")");
  }
  if (job.model.dir.empty() || size.width <= 0 || size.height <= 0) {
    return make_error(Code::InvalidArgument,
                      "a summary needs its model and a frame size");
  }
  const double every = std::max(0.1, jget(job.params, "every", 1.0));
  BuiltGraph g;
  g.summary_sink = "summary-sink";
  g.samples = SampleFeed{"frames", job.inputs.front().path, every, size};

  Json cfg = {{ex::kSummaryModel, job.model.dir.string()},
              {ex::kSummaryEvery, every},
              {ex::kSummaryLanguage,
               jget<std::string>(job.params, "language", "English")},
              {ex::kSummaryScenePrompt, assist::kSummaryScenePrompt},
              {ex::kSummaryOverallPrompt, assist::kSummaryOverallPrompt},
              {ex::kSummaryKeepLoaded, jget(job.params, "keep_loaded", 0.0)},
              {ex::kSummaryWireWeights, true},
              {ex::kSummaryPageTokens, kHelperPageTokens},
              {ex::kSummaryMaxPages, helper_max_pages(job)}};
  // Loaded as the helper's chat loads it, so either is handed the model
  // the other kept warm.
  if (auto mm = jget<std::string>(job.params, "mtp_model", "");
      !mm.empty()) {
    cfg[ex::kSummaryMtpModel] = mm;
  }
  if (auto dm = jget<std::string>(job.params, "draft_model", "");
      !dm.empty()) {
    cfg[ex::kSummaryDraftModel] = dm;
    cfg[ex::kSummaryDraftBits] = jget(job.params, "draft_bits", 8);
  }
  if (const auto mb = jget<std::int64_t>(job.params, "memory_limit_mb", 0);
      mb > 0) {
    cfg[ex::kSummaryMemoryLimitMb] = mb;
  }
  if (const auto mb = jget<std::int64_t>(job.params, "expert_cache_mb", 0);
      mb > 0) {
    cfg[ex::kSummaryExpertCacheMb] = mb;
  }
  if (const auto d = jget<std::string>(job.params, "compute_dtype", "");
      !d.empty()) {
    cfg[ex::kSummaryComputeDtype] = d;
  }
  // A scene's frames at most: ~180 tokens each at 576 x 320, so 24 keep a
  // prefill near 4,500 -- at one every two seconds 48 s of a shot, at
  // eight a second 3 s. Larger frames, more tokens each (a token is 32 x
  // 32 pixels on Qwen3-VL's grid: 720 at 1152 x 640), so no more than
  // two fifths of the helper's context a scene.
  const std::int64_t frame_tokens = std::max<std::int64_t>(
      1, static_cast<std::int64_t>(size.width / 32) * (size.height / 32));
  const std::int64_t context = static_cast<std::int64_t>(
      helper_max_pages(job)) * kHelperPageTokens;
  int scene = static_cast<int>(std::clamp<std::int64_t>(
      context * 2 / 5 / frame_tokens, 4, 24));
  if (const int sf = jget(job.params, "scene_frames", 0); sf > 0) {
    scene = sf;
  }
  cfg[ex::kSummarySceneFrames] = scene;
  // A scene's frames at least before a cut ends it: two, or a second's
  // where more come a second -- a flash, a fast turn, is not a scene.
  cfg[ex::kSummaryMinFrames] = std::max(
      2, static_cast<int>(std::lround(1.0 / every)));
  Json stages = Json::array();
  stages.push_back(stage("frames", ex::kSourceType, Json::array(),
                         Json::object()));
  Json in = Json::array({port("frames")});
  // The model's own sampler (catalog engine.vpipe.sampling), as the chat
  // has it; none, greedy.
  if (const Json smp = jget(job.params, "sampling", Json::object());
      smp.is_object() && !smp.empty()) {
    Json sc = Json::object();
    for (const char* k : {"temperature", "top_k", "top_p", "min_p",
                          "repetition_penalty", "presence_penalty"}) {
      if (smp.contains(k) && smp[k].is_number()) {
        sc[k] = smp[k];
      }
    }
    sc["seed"] = jget<std::uint64_t>(smp, "seed", 0);
    stages.push_back(stage("sampler", "sampler-select", Json::array(), sc));
    in.push_back(port("sampler"));
  }
  stages.push_back(stage("summary", ex::kSummaryType, std::move(in),
                         std::move(cfg)));
  stages.push_back(sink(g.summary_sink, port("summary"), false));
  g.spec = {{"id", "valtz-" + job.id.str()},
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
  // Where from (Controller::download_model): a model-source name -- the
  // same model, the same folder, whichever serves it.
  if (auto s = jget<std::string>(job.params, "source", ""); !s.empty()) {
    cfg["source"] = s;
  }
  Json stages = Json::array();
  stages.push_back(stage("model-fetch", "model-fetch", Json::array(), cfg));
  g.spec = {{"id", "valtz-" + job.id.str()},
            {"stages", stages},
            {"subpipelines", Json::array()}};
  return g;
}

}
