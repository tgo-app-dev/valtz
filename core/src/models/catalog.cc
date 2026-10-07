#include "valtz/models/catalog.h"

#include <algorithm>
#include <array>
#include <format>
#include <fstream>
#include <iterator>
#include <set>

namespace valtz::resources {
extern const char kModelCatalogJson[];
}

namespace valtz::models {

namespace {

struct CapName {
  Capability  cap;
  const char* id;
  const char* label;
};

constexpr std::array<CapName, 15> kCapNames = {{
  {Capability::TextToImage, "text-to-image", "Text to image"},
  {Capability::ImageEdit, "image-edit", "Image edit"},
  {Capability::TextToVideo, "text-to-video", "Text to video"},
  {Capability::ImageToVideo, "image-to-video", "Image to video"},
  {Capability::ReferenceToVideo, "reference-to-video", "Reference to video"},
  {Capability::UpscaleImage, "upscale-image", "Image upscale"},
  {Capability::UpscaleVideo, "upscale-video", "Video upscale"},
  {Capability::LivePreview, "live-preview", "Live preview"},
  {Capability::PromptEnhance, "prompt-enhance", "Prompt enhancement"},
  {Capability::Intent, "intent", "Intent detection"},
  {Capability::Caption, "caption", "Image captioning"},
  {Capability::AlphaOutput, "alpha-output", "Transparent output"},
  {Capability::AudioOutput, "audio-output", "Video with audio"},
  {Capability::TextToAudio, "text-to-audio", "Text to audio"},
  {Capability::TextToSpeech, "text-to-speech", "Text to speech"},
}};

}

const char*
to_str(Capability c)
{
  for (const auto& n : kCapNames) {
    if (n.cap == c) {
      return n.id;
    }
  }
  return "?";
}

const char*
label(Capability c)
{
  for (const auto& n : kCapNames) {
    if (n.cap == c) {
      return n.label;
    }
  }
  return "?";
}

std::optional<Capability>
capability_from_str(std::string_view s)
{
  for (const auto& n : kCapNames) {
    if (s == n.id) {
      return n.cap;
    }
  }
  return std::nullopt;
}

bool
ModelEntry::has(Capability c) const
{
  return std::find(capabilities.begin(), capabilities.end(), c) !=
         capabilities.end();
}

int
ModelEntry::rank_in(Capability c) const
{
  for (const auto& [cap, r] : rank_for) {
    if (cap == c) {
      return r;
    }
  }
  return rank;
}

namespace {

// Where a contribution's file is: inside its package, or nowhere (a path
// that climbs out of it, or an absolute one, is not the package's).
std::filesystem::path
inside(const std::filesystem::path& dir, const std::string& rel)
{
  const std::filesystem::path p(rel);
  if (rel.empty() || p.is_absolute()) {
    return {};
  }
  const auto n = p.lexically_normal();
  if (n.empty() || *n.begin() == "..") {
    return {};
  }
  return dir / n;
}

// The text of a tuning option a contribution declares, read in its
// language now: the panel shows what the core settled.
Json
localized_options(Json engine, const std::string& lang)
{
  if (!engine.is_object() || !engine.contains("vpipe") ||
      !engine["vpipe"].is_object() ||
      !engine["vpipe"].contains("options") ||
      !engine["vpipe"]["options"].is_array()) {
    return engine;
  }
  for (auto& o : engine["vpipe"]["options"]) {
    for (const char* k : {"label", "note", "help"}) {
      if (o.is_object() && o.contains(k)) {
        o[k] = text_in(o[k], lang);
      }
    }
  }
  return engine;
}

// One model as the catalog lists it; false (and `w`) when it cannot be
// one.
bool
read_entry(const Json& m, const Contributor& who, ModelEntry& e,
           Withheld& w)
{
  e.id = jget<std::string>(m, "id", "");
  e.hf_path = jget<std::string>(m, "hf_path", "");
  w.item = "model:" + e.id;
  if (e.id.empty() || e.hf_path.empty()) {
    w.why = "bad-entry";
    w.detail = "entry without id or hf_path";
    return false;
  }
  const std::string& lang = who.language;
  e.name = m.contains("name") ? text_in(m["name"], lang) : e.id;
  if (e.name.empty()) {
    e.name = e.id;
  }
  e.subdir = jget<std::string>(m, "subdir", "");
  e.file = jget<std::string>(m, "file", "");
  e.fetch_variant = jget<std::string>(m, "fetch_variant", "");
  e.family = jget<std::string>(m, "family", "");
  e.role = jget<std::string>(m, "role", "");
  for (const auto& c : jget(m, "capabilities",
                            std::vector<std::string>{})) {
    auto cap = capability_from_str(c);
    if (!cap) {
      w.why = "unknown-capability";
      w.detail = std::format("{} has unknown capability '{}'", e.id, c);
      return false;
    }
    e.capabilities.push_back(*cap);
  }
  e.disk_gb = jget(m, "disk_gb", 0.0);
  e.disk_measured = jget(m, "disk_measured", false);
  e.min_ram_gb = jget<std::uint32_t>(m, "min_ram_gb", 16);
  e.rank = jget(m, "rank", 0);
  const Json rf = jget(m, "rank_for", Json::object());
  for (auto it = rf.begin(); it != rf.end(); ++it) {
    auto cap = capability_from_str(it.key());
    if (!cap || !it->is_number_integer()) {
      w.why = "bad-entry";
      w.detail = std::format("{} has a bad rank_for '{}'", e.id, it.key());
      return false;
    }
    e.rank_for.emplace_back(*cap, it->get<int>());
  }
  e.preview_with = jget<std::string>(m, "preview_with", "");
  e.requires_models = jget(m, "requires", std::vector<std::string>{});
  e.license = jget<std::string>(m, "license", "");
  e.gated = jget(m, "gated", false);
  e.notes = m.contains("notes") ? text_in(m["notes"], lang) : "";
  e.engine = localized_options(jget(m, "engine", Json::object()), lang);
  e.prompting = jget(m, "prompting", Json::object());
  if (const Json q = jget(m, "quantize", Json()); q.is_object()) {
    e.quantize_from = jget<std::string>(q, "from", "");
    e.quantize_bits = jget(q, "bits", 8);
    e.quantize_group = jget(q, "group_size", 64);
    if (e.quantize_from.empty() || e.quantize_from == e.id ||
        (e.quantize_bits != 4 && e.quantize_bits != 8) ||
        e.quantize_group <= 0) {
      w.why = "bad-entry";
      w.detail = std::format("{} has a bad quantize block", e.id);
      return false;
    }
    // What it leaves out is its source's (Catalog::merge_).
    e.min_ram_gb = jget<std::uint32_t>(m, "min_ram_gb", 0);
  }
  e.origin = who.id;
  e.origin_version = who.version;
  return true;
}

// A quantized variant takes from its source what it leaves out: it runs
// as that one does (ModelEntry::quantize_from).
void
inherit_from_source(ModelEntry& e, const ModelEntry& src)
{
  if (e.capabilities.empty()) {
    e.capabilities = src.capabilities;
  }
  if (e.engine.empty()) {
    e.engine = src.engine;
  }
  if (e.prompting.empty()) {
    e.prompting = src.prompting;
  }
  if (e.requires_models.empty()) {
    e.requires_models = src.requires_models;
  }
  if (e.license.empty()) {
    e.license = src.license;
  }
  if (e.family.empty()) {
    e.family = src.family;
  }
  if (e.role.empty()) {
    e.role = src.role;
  }
  if (e.min_ram_gb == 0) {
    e.min_ram_gb = src.min_ram_gb;
  }
}

// The first model `e` names that the catalog lacks: its preview, what it
// requires, the supplements it runs with (few-step adapters, a VDN
// branch) and its decoder (YuE2's VAE); "" when all are there.
std::string
missing_reference(const Catalog& cat, const ModelEntry& e,
                  std::string* what)
{
  if (!e.preview_with.empty() && !cat.find(e.preview_with)) {
    *what = "previews with";
    return e.preview_with;
  }
  for (const auto& r : e.requires_models) {
    if (!cat.find(r)) {
      *what = "requires";
      return r;
    }
  }
  const Json vp = jget(e.engine, "vpipe", Json::object());
  for (const auto& [block, key] :
       {std::pair{"turbo", "lora"}, std::pair{"hyperflow", "lora"},
        std::pair{"vdn", "branch"}}) {
    const auto t = jget<std::string>(jget(vp, block, Json::object()), key,
                                     "");
    if (!t.empty() && !cat.find(t)) {
      *what = std::format("runs with {}", block);
      return t;
    }
  }
  if (const auto d = jget<std::string>(vp, "decoder", "");
      !d.empty() && !cat.find(d)) {
    *what = "decodes with";
    return d;
  }
  if (!e.quantize_from.empty() && !cat.find(e.quantize_from)) {
    *what = "is quantized from";
    return e.quantize_from;
  }
  return {};
}

// The longest skill an extension may bring: instructions, not a book.
constexpr std::uintmax_t kMaxSkillBytes = 256 * 1024;

}

Result<Catalog>
Catalog::parse(const Json& doc)
{
  if (!doc.is_object() || !doc.contains("models") ||
      !doc["models"].is_array()) {
    return make_error(Code::Corrupt, "model catalog: no models array");
  }
  Catalog cat;
  VALTZ_TRY(cat.merge_(doc, Contributor{}, /*strict=*/true));
  return cat;
}

std::vector<Withheld>
Catalog::add(const Json& contribution, const Contributor& who)
{
  auto r = merge_(contribution, who, /*strict=*/false);
  return r.ok() ? std::move(*r) : std::vector<Withheld>{};
}

std::vector<std::string>&
Catalog::auto_list_(const std::string& modality, const std::string& op)
{
  for (auto& [key, ids] : _auto) {
    if (key.first == modality && key.second == op) {
      return ids;
    }
  }
  _auto.push_back({{modality, op}, {}});
  return _auto.back().second;
}

Result<std::vector<Withheld>>
Catalog::merge_(const Json& doc, const Contributor& who, bool strict)
{
  std::vector<Withheld> out;
  // Strict: the first problem is the catalog's error, worded as before.
  auto withhold = [&](Withheld w) -> Status {
    if (strict) {
      return make_error(Code::Corrupt,
                        std::format("model catalog: {}", w.detail));
    }
    out.push_back(std::move(w));
    return {};
  };
  if (!doc.is_object()) {
    VALTZ_TRY(withhold({"catalog", "bad-entry", "not a map"}));
    return out;
  }

  // Models: each read, its id unique across everything here.
  const std::size_t first_new = _models.size();
  for (const auto& m : jget(doc, "models", Json::array())) {
    ModelEntry e;
    Withheld w;
    if (!read_entry(m, who, e, w)) {
      VALTZ_TRY(withhold(std::move(w)));
      continue;
    }
    if (find(e.id)) {
      VALTZ_TRY(withhold({"model:" + e.id, "duplicate-id",
                          std::format("duplicate id {}", e.id)}));
      continue;
    }
    _models.push_back(std::move(e));
  }
  // A quantized variant runs as its source: what it leaves out, taken.
  for (std::size_t i = first_new; i < _models.size(); ++i) {
    if (_models[i].quantize_from.empty()) {
      continue;
    }
    if (const ModelEntry* src = find(_models[i].quantize_from)) {
      const ModelEntry copy = *src;
      inherit_from_source(_models[i], copy);
    }
  }
  // Every cross-reference must resolve -- against the whole catalog, so
  // an extension's model may preview with a built-in TAE. One withheld
  // can strand another that named it: until nothing changes.
  for (bool changed = true; changed;) {
    changed = false;
    for (std::size_t i = first_new; i < _models.size(); ++i) {
      std::string what;
      const std::string id = missing_reference(*this, _models[i], &what);
      if (id.empty()) {
        continue;
      }
      VALTZ_TRY(withhold({"model:" + _models[i].id, "unknown-reference",
                          std::format("{} {} unknown {}", _models[i].id,
                                      what, id)}));
      _models.erase(_models.begin() + static_cast<std::ptrdiff_t>(i));
      changed = true;
      break;
    }
  }

  // auto: {"image": {"generate": [ids] | {"add": [ids], "before": id}}}
  const Json au = jget(doc, "auto", Json::object());
  for (auto mo = au.begin(); mo != au.end(); ++mo) {
    if (!mo->is_object()) {
      continue;
    }
    for (auto op = mo->begin(); op != mo->end(); ++op) {
      const Json add = op->is_object() ? jget(*op, "add", Json::array())
                                       : *op;
      const auto before = op->is_object()
                              ? jget<std::string>(*op, "before", "")
                              : std::string();
      auto& list = auto_list_(mo.key(), op.key());
      for (const auto& id : add.is_array() ? add : Json::array()) {
        const std::string s = id.is_string() ? id.get<std::string>() : "";
        const std::string item =
            std::format("auto:{}/{}:{}", mo.key(), op.key(), s);
        if (!find(s)) {
          VALTZ_TRY(withhold({item, "unknown-reference", std::format(
              "auto {} {} names unknown model '{}'", mo.key(), op.key(),
              s)}));
          continue;
        }
        if (std::find(list.begin(), list.end(), s) != list.end()) {
          continue;
        }
        auto at = std::find(list.begin(), list.end(), before);
        list.insert(before.empty() ? list.end() : at, s);
      }
    }
  }

  // families: each model in one at most, every feature a known one. A
  // family already here takes the new members and features.
  std::set<std::string> placed;
  for (const auto& f : _families) {
    for (const auto& mem : f.members) {
      placed.insert(mem.model);
    }
  }
  for (const auto& f : jget(doc, "families", Json::array())) {
    const auto id = jget<std::string>(f, "id", "");
    if (id.empty()) {
      VALTZ_TRY(withhold({"family:", "bad-entry", "family without id"}));
      continue;
    }
    auto it = std::find_if(_families.begin(), _families.end(),
                           [&](const Family& x) { return x.id == id; });
    Family fresh;
    Family& fam = it != _families.end() ? *it : fresh;
    if (it == _families.end()) {
      fam.id = id;
      fam.name = f.contains("name") ? text_in(f["name"], who.language)
                                    : id;
    }
    for (const auto& ft : jget(f, "features", Json::array())) {
      const std::string s = ft.is_string() ? ft.get<std::string>() : "";
      if (feature_capabilities(s).empty()) {
        VALTZ_TRY(withhold({"family:" + id, "unknown-feature",
                            std::format("family {} has unknown feature "
                                        "'{}'", id, s)}));
        continue;
      }
      if (std::find(fam.features.begin(), fam.features.end(), s) ==
          fam.features.end()) {
        fam.features.push_back(s);
      }
    }
    for (const auto& m : jget(f, "members", Json::array())) {
      Family::Member mem;
      mem.model = m.is_string() ? m.get<std::string>()
                                : jget<std::string>(m, "model", "");
      const ModelEntry* e = find(mem.model);
      if (!e) {
        VALTZ_TRY(withhold({"family:" + id, "unknown-reference",
                            std::format("family {} names unknown model "
                                        "'{}'", id, mem.model)}));
        continue;
      }
      if (!placed.insert(mem.model).second) {
        VALTZ_TRY(withhold({"family:" + id, "two-families",
                            std::format("{} is in two families",
                                        mem.model)}));
        continue;
      }
      mem.label = m.is_object() && m.contains("label")
                      ? text_in(m["label"], who.language)
                      : e->name;
      fam.members.push_back(std::move(mem));
    }
    if (it == _families.end()) {
      _families.push_back(std::move(fresh));
    }
  }

  // recognize: rules that file a dropped weight file, ahead of those
  // already here (an extension knows its own checkpoints better than any
  // heuristic); in a contribution, in its own order.
  Json rules = Json::array();
  std::size_t n = 0;
  for (const auto& r : jget(doc, "recognize", Json::array())) {
    const auto kind = jget<std::string>(r, "kind", "");
    if (!r.is_object() || (kind != "lora" && kind != "dit" &&
                           kind != "vae")) {
      VALTZ_TRY(withhold({std::format("recognize:{}", n), "bad-entry",
                          std::format("rule {} files as '{}'", n, kind)}));
    } else {
      Json rule = r;
      rule["origin"] = who.id;
      rules.push_back(std::move(rule));
    }
    ++n;
  }
  for (auto& r : _recognize) {
    rules.push_back(std::move(r));
  }
  _recognize = std::move(rules);

  // skills: enhancement instructions, read from the package now (a
  // package is read once, at launch).
  for (const auto& s : jget(doc, "skills", Json::array())) {
    const auto id = jget<std::string>(s, "id", "");
    const auto file = inside(who.dir, jget<std::string>(s, "file", ""));
    const std::string item = "skill:" + id;
    if (id.empty() || skill(id)) {
      VALTZ_TRY(withhold({item, id.empty() ? "bad-entry" : "duplicate-id",
                          std::format("skill '{}' unnamed or taken", id)}));
      continue;
    }
    std::error_code ec;
    if (file.empty()) {
      VALTZ_TRY(withhold({item, "bad-path",
                          std::format("skill {} is not in its package",
                                      id)}));
      continue;
    }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec) {
      VALTZ_TRY(withhold({item, "missing-file",
                          std::format("skill {}: {} is missing", id,
                                      file.string())}));
      continue;
    }
    if (size > kMaxSkillBytes) {
      VALTZ_TRY(withhold({item, "too-large",
                          std::format("skill {} is {} bytes", id, size)}));
      continue;
    }
    std::ifstream in(file, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    _skills.emplace_back(id, std::move(text));
  }
  return out;
}

const std::string*
Catalog::skill(std::string_view id) const
{
  for (const auto& [k, text] : _skills) {
    if (k == id) {
      return &text;
    }
  }
  return nullptr;
}

std::vector<Capability>
feature_capabilities(std::string_view f)
{
  using C = Capability;
  if (f == "video-gen") { return {C::TextToVideo, C::ImageToVideo}; }
  if (f == "video-edit") { return {C::ReferenceToVideo}; }
  if (f == "image-gen") { return {C::TextToImage}; }
  if (f == "image-edit") { return {C::ImageEdit}; }
  if (f == "audio-gen") { return {C::TextToAudio}; }
  if (f == "speech-gen") { return {C::TextToSpeech}; }
  if (f == "helper") { return {C::PromptEnhance, C::Intent}; }
  if (f == "video-upscale") { return {C::UpscaleVideo}; }
  if (f == "image-upscale") { return {C::UpscaleImage}; }
  return {};
}

const std::vector<std::string>&
Catalog::auto_order(std::string_view modality, std::string_view op) const
{
  static const std::vector<std::string> kNone;
  for (const auto& [key, ids] : _auto) {
    if (key.first == modality && key.second == op) { return ids; }
  }
  return kNone;
}

Result<Catalog>
Catalog::builtin()
{
  Json doc = Json::parse(resources::kModelCatalogJson, nullptr,
                         /*allow_exceptions=*/false);
  if (doc.is_discarded()) {
    return make_error(Code::Corrupt, "built-in model catalog is not JSON");
  }
  return parse(doc);
}

const ModelEntry*
Catalog::find(std::string_view id) const
{
  for (const auto& m : _models) {
    if (m.id == id) {
      return &m;
    }
  }
  return nullptr;
}

const ModelEntry*
Catalog::find_by_hf_path(std::string_view p) const
{
  for (const auto& m : _models) {
    if (m.hf_path == p) {
      return &m;
    }
  }
  return nullptr;
}

std::vector<const ModelEntry*>
Catalog::serving(Capability c) const
{
  std::vector<const ModelEntry*> out;
  for (const auto& m : _models) {
    if (m.has(c)) {
      out.push_back(&m);
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [c](const ModelEntry* a, const ModelEntry* b) {
                     return a->rank_in(c) > b->rank_in(c);
                   });
  return out;
}

void
to_json(Json& j, const ModelEntry& e)
{
  std::vector<std::string> caps;
  for (auto c : e.capabilities) {
    caps.emplace_back(to_str(c));
  }
  j = {
    {"id", e.id},
    {"name", e.name},
    {"family", e.family},
    {"role", e.role},
    {"hf_path", e.hf_path},
    {"capabilities", caps},
    {"disk_gb", e.disk_gb},
    {"disk_measured", e.disk_measured},
    {"min_ram_gb", e.min_ram_gb},
    {"rank", e.rank},
    {"preview_with", e.preview_with},
    {"requires", e.requires_models},
    {"license", e.license},
    {"gated", e.gated},
    {"notes", e.notes},
    // The grid the model makes pictures on (the engine rounds a size to
    // it): the app shows a typed size as it will be made.
    {"size_align",
     std::max(16, jget(jget(e.engine, "vpipe", Json::object()), "align",
                       16))},
    // What its prompts call an edit's n-th picture: the app puts a
    // suggested prompt's pictures back where the tags are.
    {"reference_tag", jget<std::string>(e.prompting, "reference_tag", "")},
    // How a continuation's prompt opens (Ref2VA): the app starts one so.
    {"continuation", jget<std::string>(e.prompting, "continuation", "")},
    // It has a prompt outline: an empty box can start from it.
    {"has_outline", !jget(e.prompting, "outline", Json()).is_null()},
  };
  // A clip's lengths: frames = offset + n * step (17n + 5 for H3), at the
  // model's own rate -- the app offers what will be made.
  const Json vp = jget(e.engine, "vpipe", Json::object());
  if (const Json g = jget(vp, "frame_grid", Json()); g.is_object()) {
    const Json d = jget(vp, "defaults", Json::object());
    j["frame_grid"] = {{"step", jget(g, "step", 1)},
                       {"offset", jget(g, "offset", 1)}};
    j["fps"] = jget(d, "fps", 24.0);
    j["frames"] = jget(d, "frames", 0);
    j["max_frames"] = jget(vp, "max_frames", 0);
  }
  // A sound's longest: the model decides a song's length, up to this.
  if (const double s = jget(vp, "max_seconds", 0.0); s > 0) {
    j["max_seconds"] = s;
  }
  // A quantized variant of another model: the app lists the two as one
  // (Favor picks the file: Controller::weights_variant_).
  if (!e.quantize_from.empty()) {
    j["quantize_from"] = e.quantize_from;
    j["quantize_bits"] = e.quantize_bits;
  }
  // The extension it came from.
  if (!e.origin.empty()) {
    j["origin"] = e.origin;
    j["origin_version"] = e.origin_version;
  }
}

}
