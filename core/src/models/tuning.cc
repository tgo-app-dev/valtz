#include "valtz/models/tuning.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <utility>
#include <vector>

namespace valtz::models {

namespace {

enum class Kind { Bool, Int, Real, List };

// The most GPU cores a Mac without matrix cores has for Fast and Med to
// lend the Neural Engine a share: past it, the GPU alone keeps up.
constexpr std::uint32_t kAneGpuCores = 20;

// One option as `m` offers it.
struct Offer {
  Offer(std::string k, Kind kd, double lo = 0, double hi = 0,
        double st = 0)
      : key(std::move(k)), kind(kd), min(lo), max(hi), step(st)
  {
  }

  std::string key;
  Kind        kind = Kind::Bool;
  double      min = 0, max = 0, step = 0;
  bool        available = true;
  std::string why;            // when not
  Json        preset;         // its value under the preference
  Json        excludes = Json::array();
  Json        fixes = Json::object();
  std::string needs;
  Json        extra = Json::object();  // per-option detail for the panel
  // An extension's own option (catalog `engine.vpipe.options`): the text
  // the panel shows for it (the app has none of its own), read in the
  // UI's language when the catalog took it.
  bool        declared = false;
  std::string label, note, help, group;
};

const char*
kind_name(Kind k)
{
  switch (k) {
  case Kind::Bool: return "bool";
  case Kind::Int: return "int";
  case Kind::Real: return "real";
  case Kind::List: return "list";
  }
  return "bool";
}

Json
vpipe_block(const ModelEntry& m)
{
  return jget(m.engine, "vpipe", Json::object());
}

bool
takes(const Json& vp, std::string_view tier)
{
  for (const auto& t : jget(vp, "accel", Json::array())) {
    if (t.is_string() && t.get<std::string>() == tier) {
      return true;
    }
  }
  return false;
}

// The catalog's table for `preference` (`presets.<preference>`); {} when
// the model has none.
Json
preset_table(const Json& vp, std::string_view preference)
{
  const Json t = jget(jget(vp, "presets", Json::object()),
                      std::string(preference).c_str(), Json::object());
  return t.is_object() ? t : Json::object();
}

// Whether `preference` runs TaoMate's method: its table says so (none
// does: it is Custom's alternative, DESIGN §10a), its adapter is here,
// and the Mac has the memory its table asks (`taomate.min_ram_gb`).
bool
taomate_preset(const Json& vp, const TuningContext& ctx,
               std::string_view preference)
{
  const Json tm = jget(vp, "taomate", Json());
  return tm.is_object() && ctx.taomate_installed &&
         jget(preset_table(vp, preference), "taomate", false) &&
         ctx.ram_gb >= jget(tm, "min_ram_gb", 0u);
}

// The few-step adapter a preference runs with: its table's candidates in
// order, the first installed taken (each with its own steps and shifts);
// a model with no table for it, its `turbo` block's LoRA when installed.
struct TurboPick {
  std::string id, name, path;          // path "": none
  int         steps = 0;
  Json        config = Json::object();  // its sigma shifts
  Json        missing = Json::array();  // wanted, none installed
};

TurboPick
pick_turbo(const ModelEntry& m, const TuningContext& ctx,
           std::string_view preference)
{
  const Json vp = vpipe_block(m);
  const Json p = preset_table(vp, preference);
  TurboPick t;
  if (p.contains("turbo")) {
    const Json c = p["turbo"];
    for (const auto& cand : c.is_array() ? c : Json::array()) {
      const auto id = jget<std::string>(cand, "lora", "");
      const auto it = ctx.loras.find(id);
      if (it != ctx.loras.end() && !it->second.path.empty()) {
        t.id = id;
        t.name = it->second.name;
        t.path = it->second.path;
        t.steps = jget(cand, "steps", 0);
        t.config = jget(cand, "config", Json::object());
        t.missing = Json::array();
        return t;
      }
      t.missing.push_back(
          {{"id", id},
           {"name", it != ctx.loras.end() ? it->second.name : id}});
    }
    return t;
  }
  const Json turbo = jget(vp, "turbo", Json());
  if (turbo.is_object() && ctx.turbo_installed && !ctx.turbo_path.empty()) {
    t.id = jget<std::string>(turbo, "lora", "");
    t.name = ctx.turbo_name;
    t.path = ctx.turbo_path;
    t.config = jget(turbo, "config", Json::object());
  }
  return t;
}

// A weight file as one spelling: /tmp and /private/tmp, or a relative
// path, are one file.
std::string
file_key(const std::string& path)
{
  std::error_code ec;
  const auto c = std::filesystem::weakly_canonical(path, ec);
  return ec ? path : c.string();
}

// A folder's largest .safetensors: the weights it holds.
std::filesystem::path
largest_weights(const std::filesystem::path& dir)
{
  std::error_code ec;
  std::filesystem::path biggest;
  std::uintmax_t most = 0;
  for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
    if (e.path().extension() == ".safetensors") {
      const auto sz = e.file_size(ec);
      if (!ec && sz > most) {
        most = sz;
        biggest = e.path();
      }
    }
  }
  return biggest;
}

// The LoRAs a Custom names (its "loras"), as they can be used: each by
// its file, once; a file that is not there kept, marked missing and off.
// How many may be on -- vpipe's two slots -- is settled after
// (settle_loras).
Json
loras_of(const Json& in)
{
  Json out = Json::array();
  if (!in.is_array()) {
    return out;
  }
  std::set<std::string> seen;
  for (const auto& l : in) {
    const auto path = lora_file(jget<std::string>(l, "path", ""));
    if (path.empty() || !seen.insert(path).second) {
      continue;
    }
    std::error_code ec;
    const bool missing = !std::filesystem::exists(path, ec);
    double scale = jget(l, "scale", 1.0);
    scale = std::isfinite(scale) ? std::clamp(scale, 0.0, 2.0) : 1.0;
    out.push_back({{"path", path}, {"scale", scale},
                   {"on", !missing && jget(l, "on", false)},
                   {"missing", missing}});
  }
  return out;
}

// Community checkpoints a Custom names for one part (its "dits" or
// "vaes"): each file or folder, marked missing when it is not there; the
// one on -- at most one, and not a missing one -- replaces the model's.
Json
checkpoints_of(const Json& overrides, const char* key)
{
  Json out = Json::array();
  const Json in = jget(overrides, key, Json::array());
  if (!in.is_array()) {
    return out;
  }
  bool one_on = false;
  for (const auto& c : in) {
    const auto path = file_key(jget<std::string>(c, "path", ""));
    if (path.empty()) {
      continue;
    }
    std::error_code ec;
    const bool missing = !std::filesystem::exists(path, ec);
    const bool on = !missing && jget(c, "on", false) && !one_on;
    one_on = one_on || on;
    out.push_back({{"path", path}, {"on", on}, {"missing", missing}});
  }
  return out;
}

// A key an option may have: lower_snake, as vpipe spells its own.
bool
snake(std::string_view k)
{
  return !k.empty() && k.size() <= 64 && k.front() >= 'a' &&
         k.front() <= 'z' &&
         std::all_of(k.begin(), k.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '_';
         });
}

// The options a family declares for itself (catalog
// `engine.vpipe.options`: an extension's knobs for its own stages),
// after Valtz's: each {key, type: bool | int | real, min, max, step,
// default, presets: {speed, balanced, quality}, needs, excludes,
// requires: "matrix-cores", label, note, help, group, stage}. A key
// Valtz offers itself, or one already declared, is not taken: one key,
// one meaning.
void
declared_offers(const Json& vp, const TuningContext& ctx,
                std::string_view preference, std::vector<Offer>& out)
{
  static const std::set<std::string> kOwn = {
    "steps", "loras", "dits", "vaes", "hyperflow", "taomate", "vdn",
    "sol_attn",
    "sol_tau", "sage_attn", "i8_gemm", "ane_ffn", "ane_qkv",
    "motion_cache", "video_shift", "audio_shift", "shift", "turbo"};
  for (const auto& d : jget(vp, "options", Json::array())) {
    const auto key = jget<std::string>(d, "key", "");
    const auto type = jget<std::string>(d, "type", "bool");
    const bool taken = std::any_of(out.begin(), out.end(),
                                   [&](const Offer& o) {
                                     return o.key == key;
                                   });
    if (!snake(key) || kOwn.contains(key) || taken ||
        (type != "bool" && type != "int" && type != "real")) {
      continue;
    }
    const Kind kind = type == "int"    ? Kind::Int
                      : type == "real" ? Kind::Real
                                       : Kind::Bool;
    Offer o{key, kind, jget(d, "min", 0.0), jget(d, "max", 1.0),
            jget(d, "step", kind == Kind::Int ? 1.0 : 0.05)};
    if (o.max < o.min) {
      std::swap(o.min, o.max);
    }
    const Json fallback = kind == Kind::Bool ? Json(false) : Json(o.min);
    o.preset = jget(jget(d, "presets", Json::object()),
                    std::string(preference).c_str(),
                    jget(d, "default", fallback));
    if (kind != Kind::Bool && o.preset.is_number()) {
      const double v = std::clamp(o.preset.get<double>(), o.min, o.max);
      o.preset = kind == Kind::Int ? Json(static_cast<int>(std::lround(v)))
                                   : Json(v);
    } else if (kind == Kind::Bool && !o.preset.is_boolean()) {
      o.preset = false;
    } else if (kind != Kind::Bool && !o.preset.is_number()) {
      o.preset = fallback;
    }
    if (jget<std::string>(d, "requires", "") == "matrix-cores" &&
        !ctx.matrix_cores) {
      o.available = false;
      o.why = "needs-matrix-cores";
    }
    o.needs = jget<std::string>(d, "needs", "");
    for (const auto& x : jget(d, "excludes", Json::array())) {
      if (x.is_string()) {
        o.excludes.push_back(x);
      }
    }
    o.declared = true;
    o.label = jget<std::string>(d, "label", key);
    o.note = jget<std::string>(d, "note", "");
    o.help = jget<std::string>(d, "help", "");
    o.group = jget<std::string>(d, "group", "");
    out.push_back(std::move(o));
  }
}

Json coerce(const Offer& o, const Json& v);

// The options `m` offers, in the order a panel lists them: the steps and
// what sets them, then attention, then the rest of the arithmetic, then
// the schedule.
std::vector<Offer>
offers(const ModelEntry& m, const TuningContext& ctx,
       std::string_view preference, bool edit, const Json& overrides)
{
  const Json vp = vpipe_block(m);
  const Json cfg = jget(vp, "config", Json::object());
  const Json table = preset_table(vp, preference);
  const TurboPick pick = pick_turbo(m, ctx, preference);
  const bool turbo_on = !pick.path.empty();
  // Its steps on, and off: its own count, else the turbo block's table.
  const int steps_on = pick.steps > 0
      ? pick.steps : preset_steps(m, preference, edit, true);
  const int steps_off = preset_steps(m, preference, edit, false);
  // A song's generator (YuE2) takes no LoRA and no DiT of one's own, and
  // its flow matching schedules itself; its decoder may be another.
  const bool sound = m.role == "audio";
  std::vector<Offer> out;

  // Speech (MOSS-TTS) samples a token a frame: no steps to take.
  if (!jget(vp, "speech", Json()).is_object()) {
    Offer o{"steps", Kind::Int, 1, 100, 1};
    o.preset = turbo_on ? steps_on : steps_off;
    out.push_back(std::move(o));
  }
  if (const Json hf = jget(vp, "hyperflow", Json()); hf.is_object()) {
    Offer o{"hyperflow", Kind::Bool};
    o.available = ctx.hyperflow_installed;
    o.why = o.available ? "" : "not-installed";
    o.preset = false;
    // It takes a LoRA slot, the Turbo LoRA's (settle_loras).
    o.fixes = {{"steps", jget(hf, "steps", 8)}};
    out.push_back(std::move(o));
  }
  // TaoMate-H3: a streaming METHOD with its own adapter, not a few-step
  // LoRA -- an alternative Custom offers, which no preset picks:
  // measured slower than Fast's 4-step LoRA, and off its prompt.
  if (const Json tm = jget(vp, "taomate", Json()); tm.is_object()) {
    Offer o{"taomate", Kind::Bool};
    o.available = ctx.taomate_installed;
    o.why = o.available ? "" : "not-installed";
    o.preset = taomate_preset(vp, ctx, preference);
    // It takes a LoRA slot, the Turbo LoRA's (settle_loras); three steps
    // a chunk are the method's own.
    o.fixes = {{"steps", jget(tm, "steps", 3)}};
    // ...and its own schedules' shifts (vpipe's kVideoShift / kAudioShift),
    // shown as what runs.
    const Json fx = jget(tm, "fixes", Json::object());
    for (auto it = fx.begin(); it != fx.end(); ++it) {
      o.fixes[it.key()] = it.value();
    }
    o.excludes = Json::array({"hyperflow", "vdn", "motion_cache"});
    out.push_back(std::move(o));
  }
  const bool vdn_block = jget(vp, "vdn", Json()).is_object();
  if (vdn_block) {
    Offer o{"vdn", Kind::Bool};
    o.available = ctx.vdn_installed;
    o.why = o.available ? "" : "not-installed";
    o.preset = false;
    o.excludes = Json::array({"sol_attn"});
    out.push_back(std::move(o));
  }
  {
    Offer o{"sol_attn", Kind::Bool};
    o.available = takes(vp, "sol_attn");
    o.why = o.available ? "" : "not-for-model";
    o.preset = false;
    out.push_back(std::move(o));
    Offer t{"sol_tau", Kind::Real, 0.0, 4.0, 0.1};
    t.available = takes(vp, "sol_attn");
    t.why = t.available ? "" : "not-for-model";
    t.preset = 1.0;
    t.needs = "sol_attn";
    out.push_back(std::move(t));
  }
  {
    Offer o{"sage_attn", Kind::Bool};
    o.available = takes(vp, "sage_attn") && ctx.matrix_cores;
    o.why = !takes(vp, "sage_attn") ? "not-for-model"
            : o.available           ? ""
                                    : "needs-matrix-cores";
    o.preset = false;
    out.push_back(std::move(o));
  }
  {
    Offer o{"i8_gemm", Kind::Bool};
    o.available = takes(vp, "i8_gemm") && ctx.matrix_cores;
    o.why = !takes(vp, "i8_gemm") ? "not-for-model"
            : o.available         ? ""
                                  : "needs-matrix-cores";
    // Fast and Med run int8 GEMMs wherever the GPU has matrix cores;
    // Fine as the model's catalog says. A table says otherwise.
    o.preset = o.available &&
               (preference != "quality" ||
                jget(jget(vp, "generate", Json::object()), "i8_gemm",
                     false));
    out.push_back(std::move(o));
  }
  {
    // The Neural Engine beside the GPU: the block's feed-forward, and its
    // q|k|v projection with it (which needs the feed-forward's module:
    // vpipe books the two as one).
    //
    // Fast and Med lend it the feed-forward where the GPU has no matrix
    // cores (before M5: no int8 path) and at most kAneGpuCores of them,
    // and there is a Neural Engine -- there the ANE is a real share of
    // the work. A picture model lends it the q|k|v projection too; a
    // model that says how much memory that pair wants
    // (`ane_qkv_min_ram_gb`: MiniMax H3's 24) only with as much.
    const bool lend = preference != "quality" && !ctx.matrix_cores &&
                      ctx.ane_cores > 0 && ctx.gpu_cores > 0 &&
                      ctx.gpu_cores <= kAneGpuCores;
    Offer f{"ane_ffn", Kind::Bool};
    f.available = takes(vp, "ane_ffn");
    f.why = f.available ? "" : "not-for-model";
    f.preset = f.available && lend;
    const bool f_on = f.preset.get<bool>();
    out.push_back(std::move(f));
    Offer q{"ane_qkv", Kind::Bool};
    q.available = takes(vp, "ane_qkv");
    q.why = q.available ? "" : "not-for-model";
    const Json min_ram = jget(vp, "ane_qkv_min_ram_gb", Json());
    const bool qkv_fits = min_ram.is_number()
        ? ctx.ram_gb >= min_ram.get<double>()
        : m.role == "image";
    q.preset = q.available && f_on && qkv_fits;
    q.needs = "ane_ffn";
    out.push_back(std::move(q));
  }
  if (takes(vp, "motion_cache")) {
    Offer o{"motion_cache", Kind::Bool};
    o.preset = false;
    out.push_back(std::move(o));
  }
  if (!sound) {
    // LoRAs, the few-step one among them: a preset's is the catalog's
    // Turbo LoRA (when installed), on; others are one's own. Up to two
    // on -- vpipe's two slots, `lora` and `lora2`.
    Offer o{"loras", Kind::List, 0.0, 2.0, 0.05};
    const double scale = jget(jget(vp, "turbo", Json::object()), "scale",
                              1.0);
    // The preset's adapter, on -- or, a preset that runs without one,
    // the model's usual Turbo LoRA when installed, listed off: Custom can
    // turn it on.
    const Json block = jget(vp, "turbo", Json());
    const bool listed_off = !turbo_on && block.is_object() &&
                            ctx.turbo_installed && !ctx.turbo_path.empty();
    const std::string listed = turbo_on     ? lora_file(pick.path)
                               : listed_off ? lora_file(ctx.turbo_path)
                                            : std::string();
    o.preset = Json::array();
    if (!listed.empty()) {
      o.preset.push_back({{"path", listed}, {"scale", scale},
                          {"on", turbo_on}, {"missing", false}});
    }
    // Every installed LoRA of the family by its name, as the list shows.
    Json names = Json::object();
    for (const auto& [id, l] : ctx.loras) {
      if (!l.path.empty()) {
        names[lora_file(l.path)] = l.name;
      }
    }
    if (turbo_on) {
      names[lora_file(pick.path)] = pick.name;
    } else if (listed_off) {
      names[listed] = ctx.turbo_name;
    }
    // The steps with the preset's few-step LoRA on, and with it off: its
    // counts are what it was distilled for, not a scaling of the model's.
    // An adapter distilled at its own shift (lightx2v's 768p ones: video
    // 6.0) brings it, and gives it back when turned off.
    o.extra = {{"slots", 2},
               {"shift_on", turbo_on ? pick.config
                                     : jget(block, "config",
                                            Json::object())},
               {"shift_off",
                {{"video_shift", static_cast<int>(std::lround(
                      jget(cfg, "video_shift", 12.0)))},
                 {"audio_shift", static_cast<int>(std::lround(
                      jget(cfg, "audio_shift", 3.0)))}}},
               {"turbo", listed},
               {"turbo_id", turbo_on ? pick.id
                                     : jget<std::string>(block, "lora",
                                                         "")},
               {"turbo_scale", scale},
               {"names", std::move(names)},
               {"steps_on", steps_on},
               {"steps_off", steps_off}};
    out.push_back(std::move(o));
  }
  // Community checkpoints for the DiT and the VAE, in place of the
  // model's own: the one on in each list.
  // Speech has neither: its codec is its own.
  const bool speech = jget(vp, "speech", Json()).is_object();
  for (const char* part : {"dits", "vaes"}) {
    if (speech || (sound && std::string_view(part) == "dits")) {
      continue;
    }
    Offer o{part, Kind::List};
    o.preset = Json::array();
    out.push_back(std::move(o));
  }
  if (cfg.contains("video_shift") || cfg.contains("audio_shift")) {
    // Whole numbers: the released schedules' 12 and 3 are, and nothing
    // between them is a setting anyone has validated.
    Offer v{"video_shift", Kind::Int, 1, 30, 1};
    v.preset = static_cast<int>(std::lround(
        turbo_on ? jget(pick.config, "video_shift",
                        jget(cfg, "video_shift", 12.0))
                 : jget(cfg, "video_shift", 12.0)));
    out.push_back(std::move(v));
    Offer a{"audio_shift", Kind::Int, 1, 30, 1};
    a.preset = static_cast<int>(std::lround(jget(cfg, "audio_shift", 3.0)));
    out.push_back(std::move(a));
  } else if (!sound) {
    // An image family's scheduler shift. One that schedules itself
    // (no `scheduler` stage: FLUX.2) has none to change.
    const Json sched = jget(vp, "scheduler", Json());
    Offer s{"shift", Kind::Real, 0.05, 20.0, 0.05};
    s.available = sched.is_object();
    s.why = s.available ? "" : "own-schedule";
    // vpipe's scheduler-select default, for one that names none.
    s.preset = jget(sched, "shift", 1.15);
    out.push_back(std::move(s));
  }
  declared_offers(vp, ctx, preference, out);
  // The preset's table, by key, over the defaults above: an option this
  // Mac or install cannot have stays off.
  for (auto& o : out) {
    // (TaoMate's preset is decided above, against the memory it needs.)
    if (o.kind == Kind::List || o.key == "steps" || o.key == "taomate" ||
        !table.contains(o.key)) {
      continue;
    }
    if (Json v = coerce(o, table[o.key]); !v.is_null()) {
      o.preset = o.available || o.kind != Kind::Bool ? v : Json(false);
    }
  }
  return out;
}

// `v` as `o` takes it, held to its range; null when it is not one.
Json
coerce(const Offer& o, const Json& v)
{
  switch (o.kind) {
  case Kind::Bool:
    return v.is_boolean() ? v : Json();
  case Kind::Int: {
    if (!v.is_number()) {
      return Json();
    }
    const double d = std::round(v.get<double>());
    return static_cast<int>(std::clamp(d, o.min, o.max));
  }
  case Kind::Real: {
    if (!v.is_number()) {
      return Json();
    }
    const double d = v.get<double>();
    if (!std::isfinite(d)) {
      return Json();
    }
    return std::clamp(d, o.min, o.max);
  }
  case Kind::List:
    return Json();  // settled apart (loras_of)
  }
  return Json();
}

// The LoRA list settled:
//  * an older "turbo" (true / false / the catalog's id / a file) turns the
//    catalog's Turbo LoRA on or off, or puts that file first, on, in its
//    place (a file that is not there is listed, missing);
//  * HyperFlow takes a slot and turns the catalog's Turbo LoRA off (two
//    distillations of one model stack, uselessly);
//  * the first ones on, in list order, take the slots left; the rest are
//    off;
//  * the catalog's Turbo LoRA (or a file in its place) turned the other
//    way with no step count said: the preset's count for it, or for none.
void
settle_loras(const Offer& o, Json& values, const Json& overrides)
{
  Json& ls = values["loras"];
  const auto turbo = jget<std::string>(o.extra, "turbo", "");
  const auto turbo_id = jget<std::string>(o.extra, "turbo_id", "");
  auto find = [&](const std::string& path) -> Json* {
    for (auto& l : ls) {
      if (l["path"] == path) {
        return &l;
      }
    }
    return nullptr;
  };
  // A file named as the few-step one keeps the few-step counts.
  bool fast = false;
  if (overrides.is_object() && overrides.contains("turbo")) {
    const Json t = overrides["turbo"];
    const bool file = t.is_string() &&
                      t.get<std::string>().find('/') != std::string::npos;
    const std::string key = file ? lora_file(t.get<std::string>()) : "";
    if (file) {
      std::error_code ec;
      fast = std::filesystem::exists(key, ec);
      Json* l = find(key);
      if (!l) {
        // (Json::object: insert() would take a braced list as elements.)
        ls.insert(ls.begin(), Json::object({{"path", key}, {"scale", 1.0},
                                            {"on", fast},
                                            {"missing", !fast}}));
      } else if (fast) {
        (*l)["on"] = true;
      }
    }
    // In place of the catalog's: a file that is there, false, or none.
    const bool want = file ? !fast
                      : t.is_boolean()
                          ? t.get<bool>()
                          : t.is_string() && !t.get<std::string>().empty() &&
                                t.get<std::string>() == turbo_id;
    if (!turbo.empty() && turbo != key) {
      if (Json* l = find(turbo)) {
        (*l)["on"] = want;
      } else if (want) {
        ls.insert(ls.begin(),
                  Json::object({{"path", turbo},
                                {"scale", jget(o.extra, "turbo_scale", 1.0)},
                                {"on", true}, {"missing", false}}));
      }
    }
  }
  int slots = jget(o.extra, "slots", 2);
  // HyperFlow's adapter, or TaoMate's, takes the first slot -- and the
  // Turbo LoRA's place.
  const bool adapter = jget(values, "hyperflow", false) ||
                       jget(values, "taomate", false);
  if (adapter) {
    --slots;
    if (Json* l = find(turbo)) {
      (*l)["on"] = false;
    }
  }
  int used = 0;
  for (auto& l : ls) {
    if (l["on"].get<bool>()) {
      l["on"] = used < slots;
      used += l["on"].get<bool>() ? 1 : 0;
    }
  }
  const Json* t = find(turbo);
  const bool on = fast || (t && (*t)["on"].get<bool>());
  // Whether the preset itself runs it: what "the other way" is from.
  bool preset_on = false;
  for (const auto& l : o.preset) {
    preset_on = preset_on || (l["path"] == turbo && l["on"].get<bool>());
  }
  if (!turbo.empty() && !adapter && on != preset_on &&
      !(overrides.is_object() && overrides.contains("steps"))) {
    values["steps"] = jget(o.extra, on ? "steps_on" : "steps_off",
                           jget(values, "steps", 8));
  }
  // Its own shifts follow it the same way, unless said: on, the ones it
  // was distilled at; off, the model's.
  const Json shifts = jget(o.extra, "shift_on", Json::object());
  if (!turbo.empty() && on != preset_on && shifts.is_object()) {
    for (auto it = shifts.begin(); it != shifts.end(); ++it) {
      if (!values.contains(it.key()) ||
          (overrides.is_object() && overrides.contains(it.key()))) {
        continue;
      }
      if (on) {
        values[it.key()] = static_cast<int>(
            std::lround(it.value().get<double>()));
      } else if (const Json m = jget(o.extra, "shift_off", Json::object());
                 m.contains(it.key())) {
        values[it.key()] = m[it.key()];
      }
    }
  }
}

Json
settle(const std::vector<Offer>& offered, const Json& overrides)
{
  Json values = Json::object();
  for (const auto& o : offered) {
    values[o.key] = o.preset;
    if (o.kind == Kind::List) {
      if (o.key == "loras") {
        values[o.key] = overrides.is_object() && overrides.contains("loras")
                            ? loras_of(overrides["loras"])
                            : o.preset;
      } else {
        values[o.key] = checkpoints_of(overrides, o.key.c_str());
      }
      continue;
    }
    if (o.available && overrides.is_object() && overrides.contains(o.key)) {
      if (Json v = coerce(o, overrides[o.key]); !v.is_null()) {
        values[o.key] = std::move(v);
      }
    }
  }
  for (const auto& o : offered) {
    if (o.key == "loras") {
      settle_loras(o, values, overrides);
    }
  }
  // A switch that refines another is off while that one is (ANE QKV
  // needs the ANE FFN's module).
  for (const auto& o : offered) {
    if (o.kind == Kind::Bool && !o.needs.empty() &&
        !jget(values, o.needs.c_str(), false)) {
      values[o.key] = false;
    }
  }
  // The rules: an option on turns off what it excludes and fixes what it
  // decides (HyperFlow brings its grid; VDN's branch replaces Sol-Attn on
  // its blocks).
  for (const auto& o : offered) {
    if (o.kind != Kind::Bool || !jget(values, o.key.c_str(), false)) {
      continue;
    }
    for (const auto& x : o.excludes) {
      values[x.get<std::string>()] = false;
    }
    for (auto it = o.fixes.begin(); it != o.fixes.end(); ++it) {
      values[it.key()] = it.value();
    }
  }
  return values;
}

}

int
preset_steps(const ModelEntry& m, std::string_view preference, bool edit,
             bool turbo)
{
  const Json vp = vpipe_block(m);
  const std::string p(preference);
  // The preset's own count, without its adapter.
  if (!turbo) {
    if (const int n = jget(preset_table(vp, preference), "steps", 0);
        n > 0) {
      return n;
    }
  }
  // A per-preference table says it outright: a few-step adapter's counts
  // are what it was distilled for, not a scaling of the model's.
  Json table = jget(vp, "steps", Json());
  if (turbo) {
    table = jget(jget(vp, "turbo", Json::object()), "steps", table);
  }
  if (table.is_object()) {
    if (const int n = jget(table, p.c_str(), jget(table, "balanced", 0));
        n > 0) {
      return n;
    }
  }
  // An edit's defaults (catalog `edit.defaults`) fall back to the
  // model's own.
  Json defaults = jget(vp, "defaults", Json::object());
  if (edit) {
    defaults = jget(jget(vp, "edit", Json::object()), "defaults", defaults);
  }
  const int base = jget(defaults, "steps", 8);
  if (p == "speed") {
    return std::max(2, (base + 1) / 2);
  }
  if (p == "quality") {
    return (base * 3 + 1) / 2;
  }
  return base;
}

Json
missing_loras(const ModelEntry& m, const TuningContext& ctx,
              std::string_view preference, const Json& overrides)
{
  // TaoMate in the Turbo LoRA's place needs none -- the preset's choice,
  // unless Custom says otherwise.
  const Json vp = vpipe_block(m);
  bool taomate = taomate_preset(vp, ctx, preference);
  if (overrides.is_object() && overrides.contains("taomate") &&
      overrides["taomate"].is_boolean() &&
      jget(vp, "taomate", Json()).is_object() && ctx.taomate_installed) {
    taomate = overrides["taomate"].get<bool>();
  }
  if (taomate) {
    return Json::array();
  }
  return pick_turbo(m, ctx, preference).missing;
}

Json
resolve_tuning(const ModelEntry& m, const TuningContext& ctx,
               std::string_view preference, bool edit,
               const Json& overrides)
{
  return settle(offers(m, ctx, preference, edit, overrides), overrides);
}

Json
tuning_options(const ModelEntry& m, const TuningContext& ctx,
               std::string_view preference, bool edit,
               const Json& overrides)
{
  const auto offered = offers(m, ctx, preference, edit, overrides);
  Json values = settle(offered, overrides);
  Json opts = Json::array();
  for (const auto& o : offered) {
    Json j = {{"key", o.key}, {"type", kind_name(o.kind)},
              {"available", o.available}, {"why", o.why}};
    if (o.kind == Kind::Int || o.kind == Kind::Real || o.kind == Kind::List) {
      j["min"] = o.min;
      j["max"] = o.max;
      j["step"] = o.step;
    }
    if (!o.excludes.empty()) {
      j["excludes"] = o.excludes;
    }
    if (!o.fixes.empty()) {
      j["fixes"] = o.fixes;
    }
    if (!o.needs.empty()) {
      j["needs"] = o.needs;
    }
    for (auto it = o.extra.begin(); it != o.extra.end(); ++it) {
      j[it.key()] = it.value();
    }
    if (o.declared) {
      j["declared"] = true;
      j["label"] = o.label;
      j["note"] = o.note;
      j["help"] = o.help;
      j["group"] = o.group;
    }
    opts.push_back(std::move(j));
  }
  const TurboPick pick = pick_turbo(m, ctx, preference);
  Json turbo;
  if (!pick.path.empty()) {
    turbo = {{"id", pick.id}, {"name", pick.name},
             {"steps", jget(values, "steps", 0)}};
  }
  // TaoMate in the Turbo LoRA's place needs none. (Read before `values`
  // is moved into the result.)
  Json missing = missing_loras(m, ctx, preference, overrides);
  return {{"family", m.family}, {"preference", std::string(preference)},
          {"values", std::move(values)}, {"options", std::move(opts)},
          {"turbo", std::move(turbo)}, {"missing", std::move(missing)}};
}


namespace {

// A .safetensors file's header: its tensor names and its metadata.
bool
read_header(const std::filesystem::path& file, Json& out)
{
  std::ifstream in(file, std::ios::binary);
  std::uint64_t n = 0;
  if (!in.read(reinterpret_cast<char*>(&n), 8) || n == 0 ||
      n > (64u << 20)) {
    return false;
  }
  std::string text(n, '\0');
  if (!in.read(text.data(), static_cast<std::streamsize>(n))) {
    return false;
  }
  out = Json::parse(text, nullptr, false);
  return out.is_object();
}

std::string
kind_of_file(const std::filesystem::path& file)
{
  Json h;
  if (!read_header(file, h)) {
    return "";
  }
  const Json meta = jget(h, "__metadata__", Json::object());
  std::size_t lora = 0, vae = 0, tensors = 0;
  for (auto it = h.begin(); it != h.end(); ++it) {
    if (it.key() == "__metadata__") {
      continue;
    }
    ++tensors;
    const std::string& k = it.key();
    if (k.find("lora_") != std::string::npos ||
        k.find(".lora.") != std::string::npos ||
        k.find("lora_down") != std::string::npos) {
      ++lora;
    }
    if (k.starts_with("decoder.") || k.starts_with("encoder.") ||
        k.find("post_quant_conv") != std::string::npos ||
        k.starts_with("vae.")) {
      ++vae;
    }
  }
  if (tensors == 0) {
    return "";
  }
  if (lora * 2 >= tensors) {
    return "lora";
  }
  // A Comfy-Org component names itself in its metadata.
  for (auto it = meta.begin(); it != meta.end(); ++it) {
    if (it.key().find("vae") != std::string::npos) {
      return "vae";
    }
  }
  return vae * 2 >= tensors ? "vae" : "dit";
}

}

std::string
lora_file(const std::filesystem::path& path)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  if (!fs::is_directory(path, ec)) {
    return file_key(path.string());
  }
  if (fs::is_regular_file(path / "adapter_model.safetensors", ec)) {
    return file_key((path / "adapter_model.safetensors").string());
  }
  const auto f = largest_weights(path);
  return file_key(f.empty() ? path.string() : f.string());
}

std::string
checkpoint_kind(const std::filesystem::path& path)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) {
    return path.extension() == ".safetensors" ? kind_of_file(path) : "";
  }
  if (!fs::is_directory(path, ec)) {
    return "";
  }
  // A diffusers folder says what it is.
  std::ifstream cfg(path / "config.json");
  if (cfg) {
    const Json c = Json::parse(cfg, nullptr, false);
    const auto cls = jget<std::string>(c, "_class_name", "");
    if (cls.find("Autoencoder") != std::string::npos ||
        cls.find("VAE") != std::string::npos) {
      return "vae";
    }
    if (cls.find("Transformer") != std::string::npos) {
      return "dit";
    }
  }
  if (fs::exists(path / "adapter_config.json", ec) ||
      fs::exists(path / "adapter_model.safetensors", ec)) {
    return "lora";
  }
  // Else what its largest .safetensors is.
  const auto biggest = largest_weights(path);
  return biggest.empty() ? "" : kind_of_file(biggest);
}

}

namespace valtz::models {

namespace {

// `s` against `pat`, where '*' stands for any run of characters.
bool
glob(std::string_view s, std::string_view pat)
{
  std::size_t si = 0, pi = 0, star = std::string_view::npos, mark = 0;
  while (si < s.size()) {
    if (pi < pat.size() && pat[pi] == '*') {
      star = pi++;
      mark = si;
    } else if (pi < pat.size() && pat[pi] == s[si]) {
      ++pi;
      ++si;
    } else if (star != std::string_view::npos) {
      pi = star + 1;
      si = ++mark;
    } else {
      return false;
    }
  }
  while (pi < pat.size() && pat[pi] == '*') {
    ++pi;
  }
  return pi == pat.size();
}

std::vector<std::string>
names(const Json& v)
{
  std::vector<std::string> out;
  for (const auto& s : v.is_array() ? v : Json::array()) {
    if (s.is_string()) {
      out.push_back(s.get<std::string>());
    }
  }
  return out;
}

// What a rule can be tested against: a folder's diffusers class, and the
// header of the weights file it holds (or is).
struct Evidence {
  std::string class_name;
  Json        header;  // null when there is no weights file to read
};

// Whether every condition `rule` states holds; a rule that states none
// matches nothing.
bool
matches(const Json& rule, const Evidence& ev)
{
  bool said = false;
  if (const auto cls = names(jget(rule, "class_name", Json::array()));
      !cls.empty()) {
    said = true;
    if (std::find(cls.begin(), cls.end(), ev.class_name) == cls.end()) {
      return false;
    }
  }
  const auto any = names(jget(rule, "keys_any", Json::array()));
  const auto all = names(jget(rule, "keys_all", Json::array()));
  const Json meta_rule = jget(rule, "metadata", Json::object());
  if (!any.empty() || !all.empty() || !meta_rule.empty()) {
    said = true;
    if (!ev.header.is_object()) {
      return false;
    }
  }
  auto some_key = [&](const std::string& part) {
    for (auto it = ev.header.begin(); it != ev.header.end(); ++it) {
      if (it.key() != "__metadata__" &&
          it.key().find(part) != std::string::npos) {
        return true;
      }
    }
    return false;
  };
  if (!any.empty() &&
      std::none_of(any.begin(), any.end(), some_key)) {
    return false;
  }
  if (!std::all_of(all.begin(), all.end(), some_key)) {
    return false;
  }
  const Json meta = jget(ev.header, "__metadata__", Json::object());
  for (auto it = meta_rule.begin(); it != meta_rule.end(); ++it) {
    const auto have = jget<std::string>(meta, it.key().c_str(), "");
    if (!it->is_string() || !meta.contains(it.key()) ||
        !glob(have, it->get<std::string>())) {
      return false;
    }
  }
  return said;
}

}

Json
classify_checkpoint(const std::filesystem::path& path, const Json& rules)
{
  namespace fs = std::filesystem;
  std::error_code ec;
  Evidence ev;
  fs::path weights;
  if (fs::is_directory(path, ec)) {
    std::ifstream cfg(path / "config.json");
    if (cfg) {
      ev.class_name = jget<std::string>(
          Json::parse(cfg, nullptr, false), "_class_name", "");
    }
    weights = fs::is_regular_file(path / "adapter_model.safetensors", ec)
                  ? path / "adapter_model.safetensors"
                  : largest_weights(path);
  } else if (path.extension() == ".safetensors") {
    weights = path;
  }
  if (!weights.empty()) {
    Json h;
    if (read_header(weights, h)) {
      ev.header = std::move(h);
    }
  }
  for (const auto& r : rules.is_array() ? rules : Json::array()) {
    if (matches(r, ev)) {
      return {{"kind", jget<std::string>(r, "kind", "")},
              {"family", jget<std::string>(r, "family", "")},
              {"origin", jget<std::string>(r, "origin", "")}};
    }
  }
  return {{"kind", checkpoint_kind(path)}, {"family", ""},
          {"origin", ""}};
}

}
