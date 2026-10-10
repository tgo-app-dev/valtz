// valtz-video-summary: a clip watched by a vision-language model and told
// in words, a scene at a time (DESIGN §4i).
//
// THE SHAPE is vpipe's realtime-vqa's -- frames gathered into scenes, a
// scene's frames prefilled once as one video, an answer decoded -- for a
// FILE rather than a camera: the frames' times are the clip's own, a
// scene ends where the picture CHANGES (scene-cut.h) rather than where a
// camera went quiet, there is one question (what happens) and no fan-out,
// and nothing is written to a database. The model is driven through
// vpipe's VideoTurn (generative-models/video-turn.h): its own tower, its
// own chat template, which a plugin cannot see.
//
// WHAT VALTZ DOES, WHAT THE STAGE DOES. Valtz decodes the clip SPARSE AND
// SMALL -- one frame a second (one every two on a Mac whose memory moves
// less than 200 GB/s), at 576 x 320 at most -- since a frame is ~180 of
// the model's tokens and every one of them is prefilled. The stage takes
// the frames as they come: each is encoded by the tower on arrival, so a
// scene's prefill finds it ready.
//
// A scene too long for one prefill (`scene_frames`) goes on in the next,
// its last frame shown again first -- the model sees the action that
// crosses the split. A cut is never carried: the next scene is another
// shot.

#include "stages.h"

#include "scene-cut.h"
#include "valtz-vpipe/exchange.h"

#include "apple-silicon/tensor-beat.h"
#include "common/beat-keys.h"
#include "common/flex-bag.h"
#include "common/flex-data.h"
#include "common/job.h"
#include "common/vpipe-format.h"
#include "generative-models/generative-model-manager.h"
#include "generative-models/loaded-language-model.h"
#include "generative-models/video-turn.h"
#include "interfaces/session-context-intf.h"
#include "interfaces/session-services-intf.h"
#include "pipeline/memory-plan.h"
#include "pipeline/runtime-context.h"
#include "pipeline/typed-stage.h"
#include "plugin/plugin-context.h"
#include "stages/model-memory.h"
#include "stages/model-registry.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace valtz::plugin {

namespace {

namespace ex = valtz::exchange;
using vpipe::ConfigKey;
using vpipe::ConfigType;
using vpipe::FlexData;
using vpipe::PortSpec;
using vpipe::StageSpec;
namespace genai = vpipe::genai;

// What the model is told, unless the host says otherwise. Plain words,
// in the clip's order; the language is the reader's.
constexpr char kScenePrompt[] =
    "These are frames from {start} to {end} of a video, one every {every} "
    "seconds. Describe what happens in this part: the place, who or what "
    "is seen, what they do, any words on screen, and how the camera "
    "moves. Write two to four sentences of plain prose in {language}. Do "
    "not mention frames or timestamps.";
constexpr char kOverallPrompt[] =
    "A video was described part by part, in order:\n\n{scenes}\n\n"
    "Write a summary of the whole video in {language}: what it shows and "
    "how it unfolds, in one paragraph of three to six sentences of plain "
    "prose. Do not list the parts or their times.";

const PortSpec kIports[] = {
  {.name = "frames",
   .doc = "planar u8 RGB [3, H, W], sideband pts_us: the frame's time",
   .type = &typeid(vpipe::TensorBeatPayload)},
  {.name = "sampler",
   .doc = "optional: a sampler-select spec, latched (unwired: greedy)",
   .type = &typeid(vpipe::FlexDataPayload)},
};
const PortSpec kOports[] = {
  {.name = "summary",
   .doc = "a FlexData beat a scene {kind, index, start, end, frames, "
          "at_cut, text}, then the whole clip's {kind: overall, text}",
   .type = &typeid(vpipe::FlexDataPayload)},
};

const ConfigKey kAttrs[] = {
  {.key = ex::kSummaryModel, .type = ConfigType::String, .required = true,
   .doc = "the vision-language model (models-DB key or directory)",
   .suggest_db = "model-registry"},
  {.key = ex::kSummaryMtpModel, .type = ConfigType::String,
   .doc = "an MTP drafter shipped apart from the model"},
  {.key = ex::kSummaryDraftModel, .type = ConfigType::String,
   .doc = "a DFlash / DFlash 2 block drafter"},
  {.key = ex::kSummaryDraftBits, .type = ConfigType::Int,
   .doc = "the DFlash drafter's bits in memory: 8, 4, 0 as stored",
   .def_int = 8},
  {.key = ex::kSummaryPageTokens, .type = ConfigType::Int,
   .doc = "K/V tokens per page (the model's cache key)", .def_int = 16},
  {.key = ex::kSummaryMaxPages, .type = ConfigType::Int,
   .doc = "K/V pages at most (the model's cache key)", .def_int = 2048},
  {.key = ex::kSummaryKeepLoaded, .type = ConfigType::Real,
   .doc = "seconds the model stays loaded after the graph (0: unloaded)"},
  {.key = ex::kSummaryWireWeights, .type = ConfigType::Bool,
   .doc = "wire the model's buffers as they load", .def_bool = true},
  {.key = ex::kSummaryScenePrompt, .type = ConfigType::Text,
   .doc = "said after a scene's frames; {start} {end} {every} {language}",
   .def_str = kScenePrompt},
  {.key = ex::kSummaryOverallPrompt, .type = ConfigType::Text,
   .doc = "said with the scenes' words; {scenes} {language}",
   .def_str = kOverallPrompt},
  {.key = ex::kSummaryLanguage, .type = ConfigType::String,
   .doc = "the language to write in, named for the model",
   .def_str = "English"},
  {.key = ex::kSummaryEvery, .type = ConfigType::Real,
   .doc = "seconds between frames (a frame without pts_us: its count "
          "times this)", .def_real = 1.0},
  {.key = ex::kSummarySceneFrames, .type = ConfigType::Uint,
   .doc = "a scene's frames at most; a longer one goes on in the next",
   .def_uint = 24},
  {.key = ex::kSummaryMinFrames, .type = ConfigType::Uint,
   .doc = "a scene's frames at least before a cut ends it", .def_uint = 2},
  {.key = ex::kSummaryCutAt, .type = ConfigType::Real,
   .doc = "the frame distance (0..1) that may cut", .def_real = 0.1},
  {.key = ex::kSummaryMaxTokens, .type = ConfigType::Uint,
   .doc = "tokens a scene's words take at most", .def_uint = 256},
  {.key = ex::kSummaryOverall, .type = ConfigType::Bool,
   .doc = "the whole clip in a paragraph at the end", .def_bool = true},
};

const StageSpec kSpec = {
  .type_name = ex::kSummaryType,
  .doc = "Video summary: frames cut into scenes where the picture "
         "changes; each scene watched by a vision-language model as one "
         "video and told in a few sentences; the whole clip in a "
         "paragraph at the end.",
  .display_name = "Valtz Video Summary",
  .category = vpipe::StageCategory::Vision,
  .iports = kIports,
  .oports = kOports,
  .attrs = kAttrs,
};

// m:ss, or h:mm:ss past the hour.
std::string
clock_text(double seconds)
{
  const auto s = static_cast<long long>(std::max(0.0, std::floor(seconds)));
  char buf[32];
  if (s >= 3600) {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld", s / 3600,
                  s / 60 % 60, s % 60);
  } else {
    std::snprintf(buf, sizeof(buf), "%lld:%02lld", s / 60, s % 60);
  }
  return buf;
}

std::string
number_text(double v)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", v);
  return buf;
}

// `text` with each {name} in `values` filled in.
std::string
filled(std::string text,
       const std::vector<std::pair<std::string, std::string>>& values)
{
  for (const auto& [name, value] : values) {
    const std::string key = "{" + name + "}";
    for (std::size_t at = text.find(key); at != std::string::npos;
         at = text.find(key, at + value.size())) {
      text.replace(at, key.size(), value);
    }
  }
  return text;
}

struct Scene {
  double      start = 0;
  double      end = 0;
  std::string text;
};

class SummaryStage final : public vpipe::TypedStage<SummaryStage> {
public:
  static constexpr const char* kTypeName = ex::kSummaryType;

  SummaryStage(const vpipe::SessionContextIntf* s, std::string id,
               std::vector<vpipe::InEdge> iports, FlexData config)
    : vpipe::TypedStage<SummaryStage>(s, std::move(id), std::move(iports),
                                      std::move(config))
  {
    allocate_oports(1);
    _hf_dir = attr_str(ex::kSummaryModel);
    _mtp_model = attr_str(ex::kSummaryMtpModel);
    _draft_model = attr_str(ex::kSummaryDraftModel);
    _draft_bits = static_cast<int>(attr_int(ex::kSummaryDraftBits));
    _page_tokens = static_cast<int>(attr_int(ex::kSummaryPageTokens));
    _max_pages = static_cast<int>(attr_int(ex::kSummaryMaxPages));
    _keep_loaded = attr_real(ex::kSummaryKeepLoaded);
    _wire = attr_bool(ex::kSummaryWireWeights);
    _scene_prompt = attr_str(ex::kSummaryScenePrompt);
    _overall_prompt = attr_str(ex::kSummaryOverallPrompt);
    _language = attr_str(ex::kSummaryLanguage);
    _every = attr_real(ex::kSummaryEvery);
    _scene_frames = static_cast<int>(
        std::max<std::uint64_t>(2, attr_uint(ex::kSummarySceneFrames)));
    _min_frames = static_cast<int>(
        std::max<std::uint64_t>(1, attr_uint(ex::kSummaryMinFrames)));
    _cut = attr_real(ex::kSummaryCutAt);
    _cutter = SceneCutter(_cut);
    _max_tokens = static_cast<int>(
        std::max<std::uint64_t>(16, attr_uint(ex::kSummaryMaxTokens)));
    _overall = attr_bool(ex::kSummaryOverall);
    if (_hf_dir.empty()) {
      fail_config(vpipe::fmt("{}('{}'): no model (hf_dir)", kTypeName,
                             this->id()));
    }
    if (_page_tokens < 1 || _max_pages < 1 || !(_every > 0)) {
      fail_config(vpipe::fmt("{}('{}'): page_tokens, max_pages and every "
                             "must be positive", kTypeName, this->id()));
    }
  }

  const StageSpec& spec() const noexcept override { return kSpec; }

  vpipe::StageMemory
  declare_memory() const override
  {
    vpipe::StageMemory m;
    if (_hf_dir.empty()) {
      return m;
    }
    const std::string dir = vpipe::resolve_model_dir(session(), _hf_dir);
    // Held for the run, and no smaller form -- as realtime-vqa's.
    m.hold(dir, vpipe::model_memory::dir_weights_bytes(dir));
    return m;
  }

  std::vector<vpipe::ResourceClaim>
  declare_resources() const override
  {
    if (_hf_dir.empty()) {
      return {};
    }
    std::vector<std::string> dirs{
        vpipe::resolve_model_dir(session(), _hf_dir)};
    if (!_draft_model.empty()) {
      dirs.push_back(vpipe::resolve_model_dir(session(), _draft_model));
    }
    return vpipe::model_memory::weight_claims(std::move(dirs));
  }

  vpipe::Job
  initialize(vpipe::RuntimeContext& ctx) override
  {
    (void)ctx;
    auto* mgr = session() ? session()->services()->generative_model_manager()
                          : nullptr;
    if (!mgr) {
      session()->error(vpipe::fmt("{}('{}'): no model manager", kTypeName,
                                  this->id()));
      co_return;
    }
    // The model as the helper's chat loads it (text-chat's LoadSpec),
    // so one kept warm by either is handed to the other.
    genai::LoadSpec spec;
    spec.hf_dir = vpipe::resolve_model_dir(session(), _hf_dir);
    spec.compute_dtype = "bf16";
    spec.page_tokens = _page_tokens;
    spec.max_pages = static_cast<std::uint32_t>(_max_pages);
    if (!_mtp_model.empty()) {
      vpipe::bag::set_text(spec.extra, genai::load_spec::kMtpDir,
                           vpipe::resolve_model_dir(session(), _mtp_model));
    }
    if (!_draft_model.empty()) {
      vpipe::bag::set_text(
          spec.extra, genai::load_spec::kDraftDir,
          vpipe::resolve_model_dir(session(), _draft_model));
      vpipe::bag::set_integer(spec.extra, genai::load_spec::kDraftBits,
                              _draft_bits);
    }
    if (_wire) {
      vpipe::bag::set_flag(spec.extra, genai::load_spec::kWireWeights, true);
    }
    _lm = mgr->load(spec);
    if (!_lm || !_lm->valid()) {
      session()->error(vpipe::fmt("{}('{}'): the model at '{}' did not "
                                  "load", kTypeName, this->id(), _hf_dir));
      _lm.reset();
      co_return;
    }
    if (_keep_loaded > 0) {
      mgr->keep_warm(_lm, spec.hf_dir, _keep_loaded);
    }
    _turn = genai::VideoTurn::make(_lm, session());
    if (!_turn) {
      session()->error(vpipe::fmt("{}('{}'): '{}' cannot watch a video "
                                  "(see above)", kTypeName, this->id(),
                                  _hf_dir));
    }
    co_return;
  }

  void
  reset_run_state() override
  {
    _sampler = FlexData();
    _sampler_latched = false;
    _cutter = SceneCutter(_cut);
    _scenes.clear();
    _count = 0;
    _index = 0;
    _start = _last = 0;
    _carried = false;
    if (_turn) {
      _turn->clear();
    }
  }

  vpipe::Job
  process(vpipe::RuntimeContext& ctx) override
  {
    // The sampler's spec, once, before the first scene.
    if (!_sampler_latched) {
      _sampler_latched = true;
      if (ctx.num_iports() > 1 && ctx.iport_connected(1)) {
        auto b = co_await ctx.read(1);
        if (const auto* fd = b ? dynamic_cast<const vpipe::FlexDataPayload*>(
                                     b.get())
                               : nullptr) {
          _sampler = fd->data;
        }
      }
    }
    auto beat = ctx.stop_requested() ? nullptr : co_await ctx.read(0);
    if (!beat) {
      if (!ctx.stop_requested()) {
        co_await finish_(ctx);
      }
      ctx.signal_done();
      co_return;
    }
    const auto* tb = dynamic_cast<const vpipe::TensorBeatPayload*>(
        beat.get());
    if (!tb || tb->dtype != vpipe::TensorBeat::DType::U8 ||
        tb->shape.size() != 3 || tb->shape[0] != 3 || !_turn) {
      if (!_warned) {
        _warned = true;
        session()->warn(vpipe::fmt(
            "{}('{}'): a beat that is not planar u8 RGB [3, H, W] (or no "
            "model) -- passed over", kTypeName, this->id()));
      }
      co_return;
    }
    const int h = static_cast<int>(tb->shape[1]);
    const int w = static_cast<int>(tb->shape[2]);
    const std::uint8_t* rgb = tb->as_u8();
    const bool timed = tb->sideband.is_object() &&
        tb->sideband.as_object().contains(vpipe::sideband::kPtsUs);
    const double t =
        timed ? static_cast<double>(tb->sideband.as_object()
                                        .at(vpipe::sideband::kPtsUs)
                                        .as_uint(0)) * 1e-6
              : static_cast<double>(_count) * _every;
    ++_count;

    const bool cut = _cutter.next(signature(rgb, h, w));
    session()->log_normal(vpipe::fmt(
        "{}('{}'): frame at {:.2f} s, distance {:.3f}{}", kTypeName,
        this->id(), t, _cutter.last_distance(), cut ? " -- a cut" : ""));
    // A scene carried on into another shot: the carried frame is not
    // this one's.
    if (cut && _carried && _turn->frames() == 1) {
      _turn->clear();
      _carried = false;
    }
    // The scene's own frames: a carried one is the last scene's.
    const int own = _turn->frames() - (_carried ? 1 : 0);
    if (own > 0) {
      if (cut && own >= _min_frames) {
        co_await close_scene_(ctx, /*at_cut=*/true);
      } else if (own >= _scene_frames) {
        co_await close_scene_(ctx, /*at_cut=*/false);
        // The scene goes on: its last frame again, first.
        if (!_last_rgb.empty() && _last_h == h && _last_w == w) {
          _turn->add_frame(_last_rgb.data(), h, w, _last);
          _carried = true;
        }
        _cutter.restart();
      }
    }
    if (_turn->frames() == 0 || (_carried && _turn->frames() == 1)) {
      _start = t;
    }
    if (_turn->add_frame(rgb, h, w, t)) {
      _last = t;
      _last_h = h;
      _last_w = w;
      _last_rgb.assign(rgb, rgb + static_cast<std::size_t>(3) * h * w);
    }
  }

private:
  // The scene's frames watched, its words sent on.
  vpipe::Job
  close_scene_(vpipe::RuntimeContext& ctx, bool at_cut)
  {
    const int frames = _turn->frames() - (_carried ? 1 : 0);
    const double start = _start;
    const double end = _last + _every;
    _carried = false;
    if (frames <= 0) {
      _turn->clear();
      co_return;
    }
    const std::string prompt = filled(
        _scene_prompt, {{"start", clock_text(start)},
                        {"end", clock_text(end)},
                        {"every", number_text(_every)},
                        {"language", _language}});
    std::string text = _turn->reply(
        {}, prompt, _sampler.is_object() ? &_sampler : nullptr, _max_tokens,
        [&ctx](std::string_view) { return !ctx.stop_requested(); });
    if (ctx.stop_requested()) {
      co_return;
    }
    text = trimmed_(std::move(text));
    FlexData out = FlexData::make_object();
    {
      auto o = out.as_object();
      o.insert(ex::kKind, FlexData::make_string(ex::kSummaryKindScene));
      o.insert(ex::kIndex, FlexData::make_uint(_index));
      o.insert(ex::kStart, FlexData::make_real(start));
      o.insert(ex::kEnd, FlexData::make_real(end));
      o.insert(ex::kFrames,
               FlexData::make_uint(static_cast<std::uint64_t>(frames)));
      o.insert(ex::kAtCut, FlexData::make_bool(at_cut));
      o.insert(ex::kText, FlexData::make_string(text));
    }
    ++_index;
    if (!text.empty()) {
      _scenes.push_back({start, end, text});
    }
    co_await ctx.write(0, vpipe::make_payload<vpipe::FlexDataPayload>(
                              std::move(out)));
  }

  // The last scene, then the whole clip from its scenes' words.
  vpipe::Job
  finish_(vpipe::RuntimeContext& ctx)
  {
    if (_turn && _turn->frames() > (_carried ? 1 : 0)) {
      co_await close_scene_(ctx, /*at_cut=*/false);
    }
    if (!_turn || !_overall || _scenes.size() < 2 ||
        ctx.stop_requested()) {
      co_return;
    }
    std::string lines;
    for (const auto& s : _scenes) {
      lines += "[" + clock_text(s.start) + "-" + clock_text(s.end) + "] " +
               s.text + "\n";
    }
    const std::string prompt = filled(
        _overall_prompt, {{"scenes", lines}, {"language", _language}});
    std::string text = trimmed_(_turn->reply(
        {}, prompt, _sampler.is_object() ? &_sampler : nullptr,
        _max_tokens * 2,
        [&ctx](std::string_view) { return !ctx.stop_requested(); }));
    if (ctx.stop_requested() || text.empty()) {
      co_return;
    }
    FlexData out = FlexData::make_object();
    {
      auto o = out.as_object();
      o.insert(ex::kKind, FlexData::make_string(ex::kSummaryKindOverall));
      o.insert(ex::kText, FlexData::make_string(text));
    }
    co_await ctx.write(0, vpipe::make_payload<vpipe::FlexDataPayload>(
                              std::move(out)));
  }

  static std::string
  trimmed_(std::string s)
  {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
      return {};
    }
    s.erase(0, first);
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    return s;
  }

  // Config.
  std::string _hf_dir;
  std::string _mtp_model;
  std::string _draft_model;
  int         _draft_bits = 8;
  int         _page_tokens = 16;
  int         _max_pages = 2048;
  double      _keep_loaded = 0;
  bool        _wire = true;
  std::string _scene_prompt;
  std::string _overall_prompt;
  std::string _language;
  double      _every = 1;
  int         _scene_frames = 24;
  int         _min_frames = 2;
  double      _cut = 0.1;
  int         _max_tokens = 256;
  bool        _overall = true;

  // The model, and its turn.
  std::shared_ptr<genai::LoadedLanguageModel> _lm;
  std::unique_ptr<genai::VideoTurn>           _turn;

  // The run.
  FlexData                  _sampler;
  bool                      _sampler_latched = false;
  SceneCutter               _cutter;
  std::vector<Scene>        _scenes;
  std::uint64_t             _count = 0;   // frames come so far
  std::uint64_t             _index = 0;   // scenes sent so far
  double                    _start = 0;   // the scene's first frame's time
  double                    _last = 0;    // its last frame's
  bool                      _carried = false;  // its first is the last's
  std::vector<std::uint8_t> _last_rgb;
  int                       _last_h = 0;
  int                       _last_w = 0;
  bool                      _warned = false;
};

}

void
register_summary(vpipe::VpipePluginContext* ctx)
{
  ctx->register_stage<SummaryStage>(&kSpec);
}

}
