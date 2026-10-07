#include "valtz/controller/controller.h"

#include "valtz/base/log.h"
#include "valtz/base/text.h"
#include "valtz/media/layers.h"
#include "valtz/media/model-input.h"
#include "valtz/media/sound.h"
#include "valtz/media/probe.h"
#include "valtz/media/thumbnail.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <random>

namespace valtz {

namespace fs = std::filesystem;

namespace {

// Seconds on the steady clock: a job's phase clock (job-timing.h).
double
steady_seconds()
{
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// A settings file in the support folder (assistant.json): a JSON object,
// read tolerantly -- missing or unreadable is empty -- and written whole,
// through a rename, so a crash leaves the old one or the new.
Json
read_settings(const fs::path& file)
{
  std::ifstream in(file);
  const Json j = in ? Json::parse(in, nullptr, /*allow_exceptions=*/false)
                    : Json();
  return j.is_object() ? j : Json::object();
}

Status
write_settings(const fs::path& file, const Json& j)
{
  std::error_code ec;
  fs::create_directories(file.parent_path(), ec);
  const fs::path tmp = file.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    out << to_text(j, 2) << "\n";
    if (!out) {
      return make_error(Code::Io, std::format("cannot write {}",
                                              tmp.string()));
    }
  }
  fs::rename(tmp, file, ec);
  if (ec) {
    return make_error(Code::Io, std::format("cannot write {}: {}",
                                            file.string(), ec.message()));
  }
  return {};
}

std::int64_t
random_seed()
{
  std::random_device rd;
  // Seeds stay in 31 bits: every family's sampler takes them as int.
  return static_cast<std::int64_t>(rd() & 0x7fffffff);
}

// The most pictures an edit gives the model (catalog `max_references`).
int
max_pictures(const models::ModelEntry& m)
{
  const Json ed = jget(jget(m.engine, "vpipe", Json::object()), "edit",
                       Json::object());
  return std::max(1, jget(ed, "max_references", 1));
}

// What the model calls each inline picture of a prompt: its number among
// `pictures` (the order the edit gives them, the base first), while it
// is one of the first `most` -- the rest are not given to the model.
std::vector<assist::InlinePicture>
inline_numbers(const std::vector<std::optional<AssetId>>& at,
               const std::vector<AssetId>& pictures,
               const std::optional<AssetId>& base, int most)
{
  std::vector<assist::InlinePicture> out;
  for (const auto& id : at) {
    assist::InlinePicture ip;
    if (id) {
      auto it = std::find(pictures.begin(), pictures.end(), *id);
      const auto n = it - pictures.begin();
      if (it != pictures.end() && n < most) {
        ip.number = static_cast<int>(n) + 1;
        ip.base = base && *id == *base;
      }
    }
    out.push_back(ip);
  }
  return out;
}

}

namespace {

// "18.2 GB": a size a person reads, as text (DESIGN §10b: no digit
// grouping from the app's formatter).
std::string
gigabytes(std::uint64_t bytes)
{
  return std::format("{:.1f} GB", static_cast<double>(bytes) / (1 << 30));
}

}

Json
out_of_memory_report(const Json& m, const project::Recipe* r)
{
  const Json refusal = jget(m, "refusal", Json::object());
  std::uint64_t need = jget<std::uint64_t>(refusal, "need", 0);
  std::uint64_t have = 0;
  for (const auto& g : jget(refusal, "gates", Json::array())) {
    if (!jget(g, "ok", true)) {
      need = jget<std::uint64_t>(g, "need", need);
      have = jget<std::uint64_t>(g, "have", 0);
      break;
    }
  }
  Json out = message_fields(msg::kOutOfMemory,
                            {{"need", gigabytes(need)},
                             {"have", gigabytes(have)}});
  out["memory"] = m;
  Json suggest = Json::object();
  if (r) {
    const Json& p = r->params;
    const int w = jget(p, "width", 0);
    const int h = jget(p, "height", 0);
    if (w > 0 && h > 0) {
      suggest["resolution"] = {{"width", w}, {"height", h}};
    }
    const int frames = jget(p, "frames", 0);
    const double fps = jget(p, "fps", 24.0);
    if (r->op == engine::kOpGenerateVideo && frames > 0) {
      suggest["length"] = {{"frames", frames},
                           {"seconds", fps > 0 ? frames / fps : 0.0}};
    }
    int refs = 0;
    for (const auto& in : r->inputs) {
      refs += in.role == "reference" || in.role == "continue" ? 1 : 0;
    }
    if (refs > 0) {
      suggest["references"] = {{"count", refs}};
    }
  }
  out["suggest"] = std::move(suggest);
  return out;
}

namespace {

// The words a model is given: its catalog `prompting.template` around
// them ("{prompt}" where they go -- a model trained on captions in a
// frame of its own), else the words as they are.
std::string
templated(const models::ModelEntry& m, std::string words)
{
  const auto t = jget<std::string>(m.prompting, "template", "");
  const auto at = t.find("{prompt}");
  if (at == std::string::npos) {
    return words;
  }
  return t.substr(0, at) + words + t.substr(at + 8);
}

// A model from an extension is recorded with its package and version: a
// project opened without it can say what made it, and what to install.
void
stamp_origin(project::Recipe& r, const models::ModelEntry& m)
{
  if (!m.origin.empty()) {
    r.params["extension"] = {{"id", m.origin},
                             {"version", m.origin_version}};
  }
}

}

// ---- lifecycle --------------------------------------------------------

Result<std::unique_ptr<Controller>>
Controller::create(ControllerConfig cfg)
{
  AppPaths paths = AppPaths::standard(cfg.support_root);
  std::error_code ec;
  for (const auto& d : {paths.support, paths.models, paths.engine}) {
    fs::create_directories(d, ec);
    if (ec) {
      return make_error(Code::Io, std::format("cannot create {}: {}",
                                              d.string(), ec.message()));
    }
  }

  std::unique_ptr<Controller> c(new Controller());
  c->_cfg = cfg;
  c->_paths = paths;
  c->_keep_working_copies = cfg.keep_working_copies;
  cache::CacheStore::Options copts;
  copts.budget_bytes = cfg.cache_budget_bytes;
  VALTZ_ASSIGN(c->_cache, cache::CacheStore::open(paths.cache, copts));
  c->_hw = models::probe_hardware();
  VALTZ_ASSIGN(c->_catalog, models::Catalog::builtin());
  c->_store = std::make_unique<models::ModelStore>(
      cfg.model_roots.empty()
          ? models::ModelStore::default_roots(paths.models)
          : cfg.model_roots,
      paths.support / "model-links.json");
  {
    const Json helper = read_settings(paths.support / "assistant.json");
    c->_assistant_choice = jget<std::string>(helper, "model", "");
    c->_assistant_keep = std::max(
        0.0, jget(helper, "keep_loaded", kDefaultKeepLoaded));
    c->_assistant_drafter =
        jget<std::string>(helper, "drafter", "") == "dflash" ? "dflash"
                                                              : "mtp";
    c->_assistant_drafter_bits =
        jget(helper, "drafter_bits", 8) == 4 ? 4 : 8;
  }

  char host[256] = {};
  gethostname(host, sizeof(host) - 1);
  c->_host = host;

  // Warnings and errors also reach the UI.
  Controller* self = c.get();
  set_log_sink([self](LogLevel lvl, std::string_view cat,
                      std::string_view msg) {
    std::string line = std::format("[{}] {}", cat, msg);
    std::fprintf(stderr, "valtz %-5s %s\n", to_str(lvl), line.c_str());
    self->_logbook.add(lvl == LogLevel::Error  ? "error"
                       : lvl == LogLevel::Warn ? "warn"
                       : lvl == LogLevel::Info ? "info"
                                               : "debug",
                       "valtz", line);
    if (lvl >= LogLevel::Warn) {
      self->post_("log", JobId{},
                  {{"level", to_str(lvl)},
                   {"category", std::string(cat)},
                   {"message", std::string(msg)}});
    }
  });

  if (cfg.with_engine) {
    engine::EngineConfig ecfg;
    ecfg.state_dir = paths.engine;
    VALTZ_ASSIGN(ecfg.temp_dir, c->_cache->scratch_dir("vpipe"));
    ecfg.log_level = cfg.engine_log_level;
    // A dev knob: vpipe's own detail (`normal` carries its streaming
    // profiles, `debug` everything) without a build of its own.
    if (const char* lv = std::getenv("VALTZ_ENGINE_LOG");
        lv != nullptr && *lv != '\0') {
      ecfg.log_level = lv;
    }
    ecfg.on_log = [self](int level, std::string_view text) {
      self->_logbook.add(level == 0   ? "error"
                         : level == 1 ? "warn"
                         : level <= 3 || level == 6 ? "info"
                                                    : "debug",
                         "vpipe", text);
    };
    ecfg.language = cfg.language;
    c->load_extensions_(ecfg);
    c->_engine = cfg.engine_factory ? cfg.engine_factory()
                                    : engine::make_default_engine(ecfg);
  } else if (cfg.engine_factory) {
    engine::EngineConfig none;
    c->load_extensions_(none);
    c->_engine = cfg.engine_factory();
  } else {
    engine::EngineConfig none;
    c->load_extensions_(none);
    c->_engine = engine::make_null_engine();
  }
  c->take_extensions_();
  VALTZ_LOG_INFO("controller", "valtz {} on {} ({} GB, {} GPU cores{}), "
                 "engine: {}", VALTZ_VERSION, c->_hw.chip, c->_hw.ram_gb(),
                 c->_hw.gpu_cores,
                 c->_hw.gpu_matrix_cores ? ", matrix cores" : "",
                 c->_engine->description());
  return c;
}

Controller::~Controller()
{
  shutdown();
}

void
Controller::shutdown()
{
  if (_shutting_down.exchange(true)) {
    return;
  }
  // A recording left running: stopped, its file let go.
  {
    std::lock_guard lk(_capture_mu);
    if (_capture) {
      const auto f = _capture->file();
      (void)_capture->stop();
      _capture.reset();
      std::error_code ec;
      fs::remove(f, ec);
    }
  }
  if (_engine) {
    _engine->shutdown();
  }
  std::vector<std::thread> bg;
  {
    std::lock_guard lk(_bg_mu);
    bg.swap(_bg);
  }
  for (auto& t : bg) {
    if (t.joinable()) {
      t.join();
    }
  }
  set_log_sink(nullptr);
  _bus.close();
  std::lock_guard lk(_projects_mu);
  // A working copy with unsaved changes stays, to be resumed -- every
  // one, where they live on between runs (valtzctl).
  for (auto& [id, op] : _projects) {
    if (op.ws && !_keep_working_copies) {
      (void)op.ws->close(false);
    }
  }
  _projects.clear();
}

std::string
Controller::version() const
{
  return VALTZ_VERSION;
}

std::string
Controller::engine_description() const
{
  return _engine ? _engine->description() : "none";
}

Json
Controller::log_since(std::uint64_t seq, std::size_t max) const
{
  return _logbook.since(seq, max);
}

void
Controller::clear_log()
{
  _logbook.clear();
}

Json
Controller::machine_status()
{
  Json j = _engine ? _engine->machine_status() : Json::object();
  if (!j.is_object()) {
    j = Json::object();
  }
  // The whole Mac's use beside Valtz's own footprint: the OS's, not the
  // engine's, so it is read here, with or without one.
  if (const std::uint64_t used = models::used_ram_bytes(); used > 0) {
    j["sys_used_bytes"] = used;
  }
  if (!j.contains("phys_total_bytes")) {
    j["phys_total_bytes"] = _hw.ram_bytes;
  }
  return j;
}

Json
Controller::gpu_thermal(int window_ms)
{
  return _engine ? _engine->gpu_thermal(window_ms) : Json::object();
}

void
Controller::post_(std::string kind, JobId job, Json data,
                  engine::TensorPtr tensor)
{
  Event ev;
  ev.kind = std::move(kind);
  ev.job = job;
  ev.data = std::move(data);
  ev.tensor = std::move(tensor);
  _bus.post(std::move(ev));
}

void
Controller::finish_job_(JobId id, JobState st, std::string message)
{
  _jobs.update(id, [&](JobRecord& r) {
    r.state = st;
    r.message = std::move(message);
    r.finished_ms = project::now_ms();
    if (st == JobState::Finished) {
      r.progress = 1.0f;
    }
  });
  {
    std::lock_guard lk(_job_commands_mu);
    _withdrawn.erase(id);
  }
  std::lock_guard lk(_timing_mu);
  _timing.erase(id);
}

// ---- extensions -------------------------------------------------------

void
Controller::load_extensions_(engine::EngineConfig& ecfg)
{
  if (!_cfg.extensions) {
    return;
  }
  std::vector<ext::Root> roots;
  if (_cfg.extension_roots.empty()) {
    roots = ext::standard_roots(_paths.support);
  } else {
    for (const auto& r : _cfg.extension_roots) {
      roots.push_back({r, false});
    }
  }
  _extensions = ext::discover(
      roots, ext::read_disabled(_paths.support / "extensions.json"));
  for (const auto& e : _extensions) {
    if (!e.admitted()) {
      if (e.state != "disabled") {
        VALTZ_LOG_WARN("extensions", "{} not taken: {} {} ({})",
                       e.id.empty() ? e.dir.filename().string() : e.id,
                       e.why, to_text(e.why_args), e.dir.string());
      }
      continue;
    }
    for (const auto& p : e.plugins) {
      ecfg.backends.push_back({e.id, p.file, p.abi, p.required, p.stages});
    }
  }
}

void
Controller::take_extensions_()
{
  // Whether the engine could have loaded a backend at all: without one,
  // nothing runs anyway, and the package is still listed whole.
  const bool engine_up = _engine && _engine->available();
  const Json backs = _engine ? _engine->backends() : Json::array();
  for (auto& e : _extensions) {
    if (!e.admitted()) {
      continue;
    }
    // One plugin that did not load and nothing the package declares can
    // run: none of it is offered.
    for (const auto& b : engine_up ? backs : Json::array()) {
      if (jget<std::string>(b, "extension", "") == e.id &&
          jget<std::string>(b, "state", "") != "loaded") {
        e.state = "backend-failed";
        e.why = jget<std::string>(b, "why", "");
        e.why_args = jget(b, "args", Json::object());
        break;
      }
    }
    if (e.state == "backend-failed") {
      VALTZ_LOG_WARN("extensions", "{}: its backend did not load ({}); "
                     "none of it is offered", e.id, e.why);
      continue;
    }
    const models::Contributor who{e.id, e.version, e.dir, _cfg.language};
    for (auto& w : _catalog.add(e.doc, who)) {
      e.withheld.push_back(std::move(w));
    }
    if (!e.withheld.empty()) {
      e.state = "partial";
    }
    for (const auto& w : e.withheld) {
      VALTZ_LOG_WARN("extensions", "{}: {} withheld: {} ({})", e.id,
                     w.item, w.why, w.detail);
    }
    VALTZ_LOG_INFO("extensions", "{} {} taken ({} model(s), interface "
                   "{})", e.id, e.version,
                   jget(e.doc, "models", Json::array()).size(),
                   e.interface);
  }
}

Json
Controller::extensions() const
{
  const auto& host = ext::Interface::host();
  const Json backs = _engine ? _engine->backends() : Json::array();
  Json list = Json::array();
  for (const auto& e : _extensions) {
    Json j = ext::to_json(e, _cfg.language);
    Json mine = Json::array();
    for (const auto& b : backs) {
      if (jget<std::string>(b, "extension", "") == e.id) {
        mine.push_back(b);
      }
    }
    j["backends"] = std::move(mine);
    list.push_back(std::move(j));
  }
  return {{"interface", {{"current", host.current},
                         {"oldest", host.oldest},
                         {"features", host.features},
                         {"shapes", host.shapes}}},
          {"extensions", std::move(list)}};
}

Status
Controller::set_extension_enabled(const std::string& id, bool on)
{
  const bool known = std::any_of(_extensions.begin(), _extensions.end(),
                                 [&](const ext::Extension& e) {
                                   return e.id == id;
                                 });
  if (!known) {
    return make_error(Code::NotFound,
                      std::format("no extension {}", id));
  }
  const fs::path file = _paths.support / "extensions.json";
  auto off = ext::read_disabled(file);
  if (on) {
    off.erase(id);
  } else {
    off.insert(id);
  }
  return ext::write_disabled(file, off);
}

Json
Controller::classify_weights(const fs::path& path) const
{
  return models::classify_checkpoint(path, _catalog.recognize_rules());
}

std::string
Controller::skill_text(std::string_view name) const
{
  if (name.empty()) {
    return {};
  }
  if (const std::string* s = _catalog.skill(name)) {
    return *s;
  }
  // A maker's own text, downloaded where it may not ship (a license that
  // bars commercial use): read from where it lies, each time -- it is a
  // few KB, and a download or a removal takes effect at once.
  const std::string key(name);
  for (const auto& e : _catalog.models()) {
    const Json provides = jget(e.prompting, "provides", Json::object());
    if (!provides.is_object() || !provides.contains(key) ||
        !provides[key].is_string()) {
      continue;
    }
    const auto info = _store->info(e);
    if (info.state != models::InstallState::Installed) {
      continue;
    }
    std::ifstream in(info.dir / provides[key].get<std::string>(),
                     std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (!text.empty()) {
      return text;
    }
  }
  return std::string(assist::rewrite_prompt(name));
}

// ---- models -----------------------------------------------------------

std::vector<models::CapabilityStatus>
Controller::capabilities() const
{
  using models::Capability;
  if (!_engine || !_engine->available()) {
    return models::resolve_capabilities(_catalog, *_store, _hw, nullptr);
  }
  return models::resolve_capabilities(
      _catalog, *_store, _hw, [this](Capability c) { return engine_runs_(c); });
}

bool
Controller::engine_runs_(models::Capability c) const
{
  using models::Capability;
  if (!_engine || !_engine->available()) {
    return false;
  }
  // Which engine op realizes each capability.
  switch (c) {
  case Capability::TextToImage:
  case Capability::LivePreview:
  case Capability::AlphaOutput:
    return _engine->supports(engine::kOpGenerateImage);
  case Capability::ImageEdit:
    return _engine->supports(engine::kOpEditImage);
  case Capability::TextToVideo:
  case Capability::ImageToVideo:
  case Capability::ReferenceToVideo:
  case Capability::AudioOutput:
    return _engine->supports(engine::kOpGenerateVideo);
  case Capability::TextToAudio:
    return _engine->supports(engine::kOpGenerateAudio);
  case Capability::TextToSpeech:
    return _engine->supports(engine::kOpGenerateSpeech);
  case Capability::UpscaleVideo:
    return _engine->supports(engine::kOpUpscaleVideo);
  case Capability::UpscaleImage:
    return _engine->supports(engine::kOpUpscaleImage);
  case Capability::PromptEnhance:
  case Capability::Intent:
    return _engine->supports(engine::kOpChat);
  default:
    return false;
  }
}

Json
Controller::capability_tree() const
{
  const fs::path valtz_root = _store->download_root();
  auto under = [](const fs::path& p, const fs::path& root) {
    const auto rel = p.lexically_relative(root);
    return !rel.empty() && *rel.begin() != "..";
  };
  Json families = Json::array();
  for (const auto& f : _catalog.families()) {
    Json members = Json::array();
    for (const auto& mem : f.members) {
      const models::ModelEntry* e = _catalog.find(mem.model);
      if (!e) {
        continue;
      }
      const auto info = _store->info(*e);
      const bool installed =
          info.state == models::InstallState::Installed;
      std::string source;
      if (info.state != models::InstallState::Missing) {
        source = info.linked                  ? "link"
                 : under(info.dir, valtz_root) ? "valtz"
                                              : "vpipe";
      }
      members.push_back({
        {"model", e->id}, {"label", mem.label}, {"name", e->name},
        {"role", e->role}, {"hf_path", e->hf_path},
        {"url", "https://huggingface.co/" + e->hf_path},
        {"state", models::to_str(info.state)}, {"bytes", info.bytes},
        {"disk_gb", e->disk_gb}, {"fits", e->min_ram_gb <= _hw.ram_gb()},
        {"min_ram_gb", e->min_ram_gb},
        {"path", installed ? info.path().string() : std::string()},
        {"source", source}, {"license", e->license}, {"notes", e->notes},
        {"gated", e->gated},
        {"origin", e->origin},
      });
      // A quantized variant: made here from its source (no download),
      // which must be here first; no page of its own on Hugging Face.
      if (!e->quantize_from.empty()) {
        const auto* src = _catalog.find(e->quantize_from);
        Json& row = members.back();
        row["url"] = "";
        row["quantize"] = {
            {"from", e->quantize_from},
            {"from_name", src ? src->name : e->quantize_from},
            {"bits", e->quantize_bits},
            {"group_size", e->quantize_group},
            {"ready", src && _store->info(*src).state ==
                                 models::InstallState::Installed}};
      }
    }
    // A feature is ready where a member makes it here: installed, it
    // fits, and the engine has the graph.
    Json features = Json::array();
    for (const auto& ft : f.features) {
      std::string why = "engine";
      for (const auto& mem : f.members) {
        const models::ModelEntry* e = _catalog.find(mem.model);
        if (!e) {
          continue;
        }
        for (const auto c : models::feature_capabilities(ft)) {
          if (!e->has(c) || !engine_runs_(c)) {
            continue;
          }
          const bool fits = e->min_ram_gb <= _hw.ram_gb();
          const bool installed = _store->info(*e).state ==
                                 models::InstallState::Installed;
          const std::string here = !fits        ? "memory"
                                   : !installed ? "download"
                                                : "ready";
          // The best of the members: ready, else a download, else
          // memory.
          auto score = [](const std::string& w) {
            return w == "ready" ? 3 : w == "download" ? 2
                   : w == "memory" ? 1 : 0;
          };
          if (score(here) > score(why)) {
            why = here;
          }
        }
      }
      features.push_back({{"feature", ft}, {"available", why == "ready"},
                          {"why", why}});
    }
    families.push_back({{"id", f.id}, {"name", f.name},
                        {"features", std::move(features)},
                        {"members", std::move(members)}});
  }
  return {{"families", std::move(families)},
          {"download_root", valtz_root.string()}};
}

namespace {

// What the files under `dir` hold, in bytes; 0 when it is not there.
std::uint64_t
folder_bytes(const fs::path& dir)
{
  std::error_code ec;
  if (fs::is_regular_file(dir, ec)) {
    return fs::file_size(dir, ec);
  }
  std::uint64_t n = 0;
  for (auto it = fs::recursive_directory_iterator(
           dir, fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    if (it->is_regular_file(ec) && !it->is_symlink(ec)) {
      n += it->file_size(ec);
    }
  }
  return n;
}

}

Json
Controller::storage_report() const
{
  std::error_code ec;
  Json out = Json::object();
  const auto space = fs::space(_paths.support, ec);
  out["volume"] = {{"path", _paths.support.string()},
                   {"capacity", ec ? 0 : space.capacity},
                   {"free", ec ? 0 : space.available}};

  // Models: each repo folder in the managed root, named by the catalog
  // entries it holds.
  const fs::path mroot = _store->download_root();
  Json models = Json::array();
  std::uint64_t mtotal = 0;
  for (const auto& org : fs::directory_iterator(mroot, ec)) {
    if (!org.is_directory(ec)) {
      continue;
    }
    for (const auto& repo : fs::directory_iterator(org.path(), ec)) {
      if (!repo.is_directory(ec)) {
        continue;
      }
      const std::string hf = org.path().filename().string() + "/" +
                             repo.path().filename().string();
      Json names = Json::array();
      for (const auto& e : _catalog.models()) {
        if (e.hf_path == hf &&
            _store->info(e).dir.string().starts_with(repo.path().string())) {
          names.push_back(e.name);
        }
      }
      const std::uint64_t n = folder_bytes(repo.path());
      mtotal += n;
      models.push_back({{"repo", hf}, {"names", std::move(names)},
                        {"bytes", n}});
    }
  }
  out["models"] = {{"root", mroot.string()}, {"bytes", mtotal},
                   {"items", std::move(models)}};

  // Projects: the packages in the projects folder, and the open ones
  // wherever they are -- an open one with its assets, each.
  std::vector<std::pair<fs::path, const OpenProject*>> pkgs;
  {
    std::lock_guard lk(_projects_mu);
    for (const auto& [id, op] : _projects) {
      // Its package -- an untitled one's working copy.
      pkgs.push_back({op.ws->package() ? *op.ws->package() : op.ws->dir(),
                      &op});
    }
  }
  for (const auto& e : fs::directory_iterator(_paths.projects, ec)) {
    if (e.path().extension() == ".valtz" &&
        std::none_of(pkgs.begin(), pkgs.end(), [&](const auto& p) {
          return fs::equivalent(p.first, e.path(), ec);
        })) {
      pkgs.push_back({e.path(), nullptr});
    }
  }
  Json projects = Json::array();
  std::uint64_t ptotal = 0;
  for (const auto& [pkg, op] : pkgs) {
    const std::uint64_t n = folder_bytes(pkg);
    ptotal += n;
    Json item = {{"name", op ? op->ws->project().name()
                             : pkg.stem().string()},
                 {"path", pkg.string()}, {"bytes", n},
                 {"open", op != nullptr},
                 {"ephemeral", op && op->ephemeral}};
    if (op) {
      project::Project& p = op->ws->project();
      Json assets = Json::array();
      std::uint64_t held = 0;
      if (auto list = p.assets(); list.ok()) {
        for (const auto& a : *list) {
          std::uint64_t b = 0;
          if (auto vs = p.versions(a.id); vs.ok()) {
            for (const auto& v : *vs) {
              b += v.blob.size;
            }
          }
          for (const auto& l : a.layers) {
            if (l.markup && !l.markup->raster.empty()) {
              b += l.markup->raster.size;
            }
          }
          held += b;
          assets.push_back({{"id", a.id}, {"name", a.name},
                            {"kind", project::to_str(a.kind)},
                            {"bytes", b}, {"linked", a.linked}});
        }
      }
      item["assets"] = std::move(assets);
      // The records, superseded markup, temporary files.
      item["other"] = n > held ? n - held : 0;
    }
    projects.push_back(std::move(item));
  }
  out["projects"] = {{"root", _paths.projects.string()}, {"bytes", ptotal},
                     {"items", std::move(projects)}};

  out["cache"] = {{"root", _cache->root().string()},
                  {"bytes", _cache->size_bytes()},
                  {"budget", _cache->budget_bytes()}};
  return out;
}

Status
Controller::link_model(const std::string& id, const fs::path& target)
{
  const models::ModelEntry* e = _catalog.find(id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel, {{"model", id}});
  }
  VALTZ_TRY(_store->link(*e, target));
  post_("models.changed", JobId{}, {{"model", id}, {"reason", "linked"}});
  return ok_status();
}

Status
Controller::unlink_model(const std::string& id)
{
  if (!_catalog.find(id)) {
    return make_error(Code::NotFound, msg::kUnknownModel, {{"model", id}});
  }
  _store->unlink(id);
  post_("models.changed", JobId{}, {{"model", id}, {"reason", "unlinked"}});
  return ok_status();
}

// ---- the asset list ---------------------------------------------------

Result<Json>
Controller::folders(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  return p->folders();
}

Result<Json>
Controller::view_state(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  return p->view_state();
}

Status
Controller::set_view_state(ProjectId pid, const Json& view)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(Json was, p->view_state());
  if (was == view) {
    return ok_status();
  }
  VALTZ_TRY(p->set_view_state(view));
  post_state_(pid);
  return ok_status();
}

Result<std::string>
Controller::create_folder(ProjectId pid, std::string name)
{
  auto undo = command_(pid, "folder.add", {}, "", {{"name", name}});
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(Json fs, p->folders());
  const std::string id = AssetId::make().str();
  fs.push_back({{"id", id},
                {"name", std::string(utf8_prefix(one_line(name), 64))}});
  VALTZ_TRY(p->set_folders(fs));
  post_("assets.changed", JobId{}, {{"project", pid}, {"reason", "folders"}});
  return id;
}

Status
Controller::rename_folder(ProjectId pid, const std::string& folder,
                          std::string name)
{
  auto undo = command_(pid, "folder.rename", {}, "", {{"name", name}});
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(Json fs, p->folders());
  for (auto& f : fs) {
    if (jget<std::string>(f, "id", "") == folder) {
      f["name"] = std::string(utf8_prefix(one_line(name), 64));
      VALTZ_TRY(p->set_folders(fs));
      post_("assets.changed", JobId{},
            {{"project", pid}, {"reason", "folders"}});
      return ok_status();
    }
  }
  return make_error(Code::NotFound, msg::kFolderUnknown);
}

Status
Controller::delete_folder(ProjectId pid, const std::string& folder)
{
  auto undo = command_(pid, "folder.remove");
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(Json fs, p->folders());
  Json kept = Json::array();
  for (const auto& f : fs) {
    if (jget<std::string>(f, "id", "") != folder) {
      kept.push_back(f);
    }
  }
  if (kept.size() == fs.size()) {
    return make_error(Code::NotFound, msg::kFolderUnknown);
  }
  // Its assets, to the top of the list.
  VALTZ_ASSIGN(auto all, p->assets());
  for (const auto& a : all) {
    if (a.folder == folder) {
      VALTZ_TRY(p->set_asset_folder(a.id, ""));
    }
  }
  VALTZ_TRY(p->set_folders(kept));
  post_("assets.changed", JobId{}, {{"project", pid}, {"reason", "folders"}});
  return ok_status();
}

Status
Controller::move_asset(ProjectId pid, AssetId aid, const std::string& folder)
{
  auto undo = command_(pid, "asset.move", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (!folder.empty()) {
    VALTZ_ASSIGN(Json fs, p->folders());
    if (!std::ranges::any_of(fs, [&](const Json& f) {
          return jget<std::string>(f, "id", "") == folder;
        })) {
      return make_error(Code::NotFound, msg::kFolderUnknown);
    }
  }
  VALTZ_TRY(p->set_asset_folder(aid, folder));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "folder"}});
  return ok_status();
}

Status
Controller::rename_asset(ProjectId pid, AssetId aid, std::string name)
{
  auto undo = command_(pid, "asset.rename", aid, "", {{"name", name}});
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  name = one_line(name);
  if (name.empty()) {
    return make_error(Code::InvalidArgument, msg::kAssetNameEmpty);
  }
  name = std::string(utf8_prefix(name, 200));
  if (name == a.name) {
    return ok_status();
  }
  VALTZ_TRY(p->rename_asset(aid, std::move(name)));
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "renamed"}});
  return ok_status();
}

Status
Controller::remove_asset(ProjectId pid, AssetId aid)
{
  auto undo = command_(pid, "asset.remove", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (project_composition(pid) == aid) {
    return make_error(Code::Busy, msg::kProjectCompositionStays);
  }
  VALTZ_ASSIGN(auto all, p->assets());
  for (const auto& o : all) {
    if (o.id == aid) {
      continue;
    }
    for (const auto& l : o.layers) {
      if (l.source == aid) {
        return make_error(Code::Busy, msg::kAssetInUse, {{"name", a.name}});
      }
    }
  }
  if (auto st = p->remove_asset(aid); !st.ok()) {
    return st.error().code == Code::Busy
               ? make_error(Code::Busy, msg::kAssetInUse, {{"name", a.name}})
               : st;
  }
  post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                    {"reason", "removed"}});
  return ok_status();
}

bool
Controller::runs(const models::ModelEntry& m, std::string_view modality,
                 std::string_view op) const
{
  if (!_engine || !_engine->available() ||
      _store->info(m).state != models::InstallState::Installed ||
      _hw.ram_gb() < m.min_ram_gb) {
    return false;
  }
  if (modality == "image" && op == "generate") {
    return m.has(models::Capability::TextToImage) &&
           _engine->supports(engine::kOpGenerateImage);
  }
  if (modality == "image" && op == "edit") {
    return can_edit_(m);
  }
  if (modality == "video" && op == "generate") {
    return m.has(models::Capability::TextToVideo) &&
           _engine->supports(engine::kOpGenerateVideo);
  }
  if (modality == "audio" && (op == "generate" || op == "edit")) {
    // Its latents (or codes) are another checkpoint's to decode: YuE2's
    // VAE, MOSS-TTS's audio tokenizer.
    const auto* d = _catalog.find(jget<std::string>(
        jget(m.engine, "vpipe", Json::object()), "decoder", ""));
    const bool decodes =
        !d || _store->info(*d).state == models::InstallState::Installed;
    // A song from words; speech from words -- or, with a voice to clone
    // (`edit`), speech alone.
    const bool song = op == "generate" &&
                      m.has(models::Capability::TextToAudio) &&
                      _engine->supports(engine::kOpGenerateAudio);
    const bool speech = m.has(models::Capability::TextToSpeech) &&
                        _engine->supports(engine::kOpGenerateSpeech);
    return (song || speech) && decodes;
  }
  // A clip from references (MiniMax H3 Ref2VA): the catalog says how
  // its graph reads them.
  if (modality == "video" && op == "edit") {
    return m.has(models::Capability::ReferenceToVideo) &&
           jget(jget(m.engine, "vpipe", Json::object()), "references",
                Json()).is_object() &&
           _engine->supports(engine::kOpGenerateVideo);
  }
  return false;
}

const models::ModelEntry*
Controller::turbo_adapter(const models::ModelEntry& m) const
{
  const Json turbo = jget(jget(m.engine, "vpipe", Json::object()), "turbo",
                          Json::object());
  const auto* a = _catalog.find(jget<std::string>(turbo, "lora", ""));
  return a && _store->info(*a).state == models::InstallState::Installed
             ? a
             : nullptr;
}

int
Controller::steps_for(const models::ModelEntry& m,
                      std::string_view preference, bool edit,
                      bool turbo) const
{
  return models::preset_steps(m, preference, edit,
                              turbo && turbo_adapter(m) != nullptr);
}

models::TuningContext
Controller::tuning_context(const models::ModelEntry& m) const
{
  const Json vp = jget(m.engine, "vpipe", Json::object());
  auto installed = [&](const char* block, const char* key) {
    const auto* e = _catalog.find(jget<std::string>(
        jget(vp, block, Json::object()), key, ""));
    return e && _store->info(*e).state == models::InstallState::Installed;
  };
  models::TuningContext c;
  c.turbo_installed = installed("turbo", "lora");
  if (const auto* t = turbo_adapter(m)) {
    c.turbo_name = t->name;
    c.turbo_path = _store->info(*t).path().string();
  }
  c.hyperflow_installed = installed("hyperflow", "lora");
  c.taomate_installed = installed("taomate", "lora");
  c.vdn_installed = installed("vdn", "branch");
  c.matrix_cores = _hw.gpu_matrix_cores;
  c.gpu_cores = _hw.gpu_cores;
  c.ane_cores = _hw.ane_cores;
  c.ram_gb = _hw.ram_gb();
  // The family's LoRAs: what a preset's table picks its adapter from.
  for (const auto& e : _catalog.models()) {
    if (e.role != "lora" || e.family != m.family) {
      continue;
    }
    const auto info = _store->info(e);
    c.loras[e.id] = {e.name, info.state == models::InstallState::Installed
                                 ? info.path().string() : std::string()};
  }
  return c;
}

Status
Controller::check_preset_loras_(const models::ModelEntry& m,
                                std::string_view preference,
                                const Json& overrides) const
{
  // Custom's own LoRAs, or the adapter turned off, say otherwise.
  if (overrides.is_object() &&
      (overrides.contains("loras") || overrides.contains("turbo"))) {
    return ok_status();
  }
  const Json miss =
      models::missing_loras(m, tuning_context(m), preference, overrides);
  if (miss.empty()) {
    return ok_status();
  }
  std::string names;
  for (const auto& l : miss) {
    if (!names.empty()) {
      names += " / ";
    }
    names += jget<std::string>(l, "name", "");
  }
  return make_error(Code::NotFound, msg::kPresetLoraMissing,
                    {{"model", m.name}, {"loras", names}});
}

void
Controller::apply_tuning_(project::Recipe& r, const models::ModelEntry& m,
                          const Json& t, int steps) const
{
  const Json vp = jget(m.engine, "vpipe", Json::object());
  r.params["steps"] = steps > 0 ? steps : jget(t, "steps", 8);
  // The two LoRA slots: HyperFlow's adapter first when it is on, then the
  // LoRAs on, in list order. The catalog's Turbo LoRA in the first is
  // recorded by its id; anything else by its file. The job is given the
  // files.
  struct Slot {
    std::string id, file;
    double      scale = 1.0;
  };
  std::vector<Slot> slots;
  if (jget(t, "hyperflow", false)) {
    slots.push_back({jget<std::string>(jget(vp, "hyperflow", Json::object()),
                                       "lora", ""), "", 1.0});
  } else if (jget(t, "taomate", false)) {
    // TaoMate's adapter, which vpipe's model-config `taomate` then runs
    // as its streaming method (the graph builder says so).
    slots.push_back({jget<std::string>(jget(vp, "taomate", Json::object()),
                                       "lora", ""), "", 1.0});
  }
  // A LoRA of the catalog (the family's, installed) is recorded by its
  // id, so a rebuild finds it wherever it is kept.
  std::map<std::string, std::string> catalog_ids;
  for (const auto& [id, l] : tuning_context(m).loras) {
    if (!l.path.empty()) {
      catalog_ids[models::lora_file(l.path)] = id;
    }
  }
  for (const auto& l : jget(t, "loras", Json::array())) {
    if (slots.size() >= 2) {
      break;
    }
    if (jget(l, "on", false)) {
      const auto path = jget<std::string>(l, "path", "");
      const auto cat = catalog_ids.find(path);
      const bool known = cat != catalog_ids.end();
      slots.push_back({known ? cat->second : "", known ? "" : path,
                       jget(l, "scale", 1.0)});
    }
  }
  if (!slots.empty()) {
    if (!slots[0].id.empty()) {
      r.params["lora"] = slots[0].id;
    } else {
      r.params["lora_file"] = slots[0].file;
    }
    r.params["lora_scale"] = slots[0].scale;
  }
  if (slots.size() > 1) {
    // The second slot takes a file (a catalog adapter by its file too).
    std::string file = slots[1].file;
    if (file.empty()) {
      for (const auto& [f, id] : catalog_ids) {
        if (id == slots[1].id) {
          file = f;
        }
      }
    }
    r.params["lora2_file"] = file;
    r.params["lora2_scale"] = slots[1].scale;
  }
  // Community checkpoints in place of the model's DiT and VAE: the one on
  // in each list.
  for (const auto& [list, key] : {std::pair{"dits", "dit_file"},
                                  std::pair{"vaes", "vae_file"}}) {
    for (const auto& c : jget(t, list, Json::array())) {
      if (jget(c, "on", false)) {
        r.params[key] = jget<std::string>(c, "path", "");
        break;
      }
    }
  }
  if (jget(t, "vdn", false)) {
    r.params["branch"] = jget<std::string>(jget(vp, "vdn", Json::object()),
                                           "branch", "");
  }
  // The rest as settled: what the graph builder lays on the stages.
  r.params["tuning"] = t;
}

Json
Controller::preset_summary(const models::ModelEntry& m,
                           std::string_view preference) const
{
  const Json t = models::tuning_options(m, tuning_context(m), preference,
                                        false);
  const Json v = jget(t, "values", Json::object());
  const Json turbo = jget(t, "turbo", Json());
  return {{"steps", jget(v, "steps", 0)},
          {"turbo", turbo.is_object() ? jget<std::string>(turbo, "name", "")
                                      : std::string()},
          {"missing", jget(t, "missing", Json::array())},
          {"sol_attn", jget(v, "sol_attn", false)},
          {"w8", jget(v, "w8_weights", false)},
          {"sage_attn", jget(v, "sage_attn", false)},
          {"i8_gemm", jget(v, "i8_gemm", false)},
          {"ane_ffn", jget(v, "ane_ffn", false)},
          {"ane_qkv", jget(v, "ane_qkv", false)},
          {"motion_cache", jget(v, "motion_cache", false)},
          {"taomate", jget(v, "taomate", false)}};
}

Result<Json>
Controller::tuning(const std::string& model_id, std::string_view preference,
                   bool edit, const Json& overrides) const
{
  const auto* m = _catalog.find(model_id);
  if (!m) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  return models::tuning_options(*m, tuning_context(*m), preference, edit,
                                overrides);
}

std::vector<Controller::AutoPick>
Controller::auto_models() const
{
  std::vector<AutoPick> out;
  for (const char* modality : {"image", "video", "audio"}) {
    for (const char* op : {"generate", "edit"}) {
      // A sound's `edit` is speech in a voice the row holds: a model
      // that speaks (DESIGN §4g).
      AutoPick a;
      a.modality = modality;
      a.op = op;
      a.order = _catalog.auto_order(modality, op);
      for (const auto& id : a.order) {
        const auto* m = _catalog.find(id);
        if (m && runs(*m, modality, op)) {
          a.chosen = id;
          break;
        }
      }
      out.push_back(std::move(a));
    }
  }
  return out;
}

const models::ModelEntry*
Controller::assistant_model() const
{
  if (!_assistant.empty()) {
    if (const auto* m = _catalog.find(_assistant);
        m && m->role == "assistant") {
      return m;
    }
  }
  if (!_assistant_choice.empty()) {
    if (const auto* m = _catalog.find(_assistant_choice);
        m && m->role == "assistant" &&
        _store->info(*m).state == models::InstallState::Installed) {
      return m;
    }
  }
  return models::pick_assistant(_catalog, *_store, _hw);
}

void
Controller::rescan_models()
{
  _store->rescan();
  post_("models.changed", JobId{});
}

Result<engine::ModelRef>
Controller::resolve_model_(const models::ModelEntry& e,
                           bool want_preview) const
{
  auto inst = _store->info(e);
  if (inst.state != models::InstallState::Installed) {
    return make_error(Code::NotFound, msg::kModelNotInstalled,
                      {{"model", e.name}});
  }
  engine::ModelRef ref;
  ref.id = e.id;
  ref.family = e.family;
  ref.dir = inst.dir;
  ref.engine = e.engine;
  if (want_preview && !e.preview_with.empty()) {
    if (const auto* tae = _catalog.find(e.preview_with)) {
      auto ti = _store->info(*tae);
      if (ti.state == models::InstallState::Installed) {
        ref.preview = ti.path();
      }
    }
  }
  return ref;
}

Result<JobId>
Controller::download_model(const std::string& model_id,
                           const std::string& hf_token)
{
  const auto* e = _catalog.find(model_id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  if (!_engine->supports(engine::kOpFetchModel)) {
    return make_error(Code::Unsupported, msg::kNoEngineForDownload);
  }
  // Its MTP drafter, shipped apart (engine.vpipe.mtp_drafter), comes with
  // it: a download of its own, queued after this one.
  const auto drafter = jget<std::string>(
      jget(e->engine, "vpipe", Json::object()), "mtp_drafter", "");
  engine::JobSpec spec;
  spec.id = JobId::make();
  spec.op = std::string(engine::kOpFetchModel);
  spec.model.id = e->id;
  spec.params = {{"hf_path", e->hf_path},
                 {"base_path", _store->download_root().string()}};
  if (!e->fetch_variant.empty()) {
    spec.params["variant"] = e->fetch_variant;
  }
  if (!hf_token.empty()) {
    spec.params["hf_token"] = hf_token;
  }

  JobRecord rec;
  rec.id = spec.id;
  rec.op = spec.op;
  rec.purpose = "download";
  rec.title = std::format("Download {}", e->name);
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  post_("job.queued", spec.id, {{"title", rec.title}, {"op", rec.op},
                                {"purpose", rec.purpose},
                                {"model", e->id}});

  const bool gated = e->gated;
  const std::string name = e->name;
  VALTZ_TRY(_engine->submit(std::move(spec),
                            [this, gated, name](const engine::JobEvent& ev) {
    switch (ev.kind) {
    case engine::JobEventKind::Started:
      _jobs.update(ev.job, [](JobRecord& r) { r.state = JobState::Running; });
      post_("job.started", ev.job, {{"engine", ev.text}});
      break;
    case engine::JobEventKind::Progress: {
      // The file being fetched, and how much of it.
      if (ev.progress >= 0) {
        _jobs.update(ev.job, [&](JobRecord& r) { r.progress = ev.progress; });
      }
      Json d = {{"progress", ev.progress}};
      if (ev.data.is_object()) {
        d.update(ev.data);
      }
      post_("job.progress", ev.job, d);
      break;
    }
    case engine::JobEventKind::Finished:
      finish_job_(ev.job, JobState::Finished, "");
      _store->rescan();
      post_("job.finished", ev.job);
      post_("models.changed", JobId{});
      break;
    case engine::JobEventKind::Failed: {
      finish_job_(ev.job, JobState::Failed, ev.text);
      _store->rescan();
      // A gated repo refused: its file list is public, its files are
      // not -- vpipe names the status of the first one refused ("HTTP
      // 401": no token or not valid; 403: the license not accepted).
      // Why, in words the person can act on.
      Json d = {{"message", ev.text}};
      if (gated && ev.text.find("HTTP 401") != std::string::npos) {
        d = message_fields(msg::kDownloadNeedsToken, {{"model", name}});
      } else if (gated && ev.text.find("HTTP 403") != std::string::npos) {
        d = message_fields(msg::kDownloadNotGranted, {{"model", name}});
      }
      d["code"] = to_str(ev.error);
      post_("job.failed", ev.job, d);
      post_("models.changed", JobId{});
      break;
    }
    case engine::JobEventKind::Cancelled:
      finish_job_(ev.job, JobState::Cancelled, "");
      post_("job.cancelled", ev.job);
      break;
    default:
      break;
    }
  }));
  if (const auto* d = _catalog.find(drafter);
      d && _store->info(*d).state != models::InstallState::Installed) {
    (void)download_model(d->id);
  }
  return rec.id;
}

Result<JobId>
Controller::quantize_model(const std::string& model_id)
{
  const auto* e = _catalog.find(model_id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  if (e->quantize_from.empty()) {
    return make_error(Code::InvalidArgument, msg::kNotQuantizable,
                      {{"model", e->name}});
  }
  const auto* src = _catalog.find(e->quantize_from);
  const auto si = src ? _store->info(*src) : models::InstallInfo{};
  if (!src || si.state != models::InstallState::Installed) {
    return make_error(Code::NotFound, msg::kQuantizeNeedsSource,
                      {{"source", src ? src->name : e->quantize_from},
                       {"model", e->name}});
  }
  if (_store->info(*e).state == models::InstallState::Installed) {
    return make_error(Code::AlreadyExists, std::format(
        "{} is already installed ({})", e->name,
        _store->info(*e).path().string()));
  }
  if (!_engine->supports(engine::kOpQuantizeModel)) {
    return make_error(Code::Unsupported, msg::kEngineCannotRun,
                      {{"operation",
                        std::string(engine::kOpQuantizeModel)}});
  }
  // Made beside where it goes, under a name the store does not read, and
  // moved in whole when it is done.
  const fs::path dest = _store->download_root() / e->hf_path;
  const fs::path staging =
      dest.parent_path() / std::format(".{}.quantizing",
                                       dest.filename().string());
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(dest.parent_path(), ec);

  engine::JobSpec spec;
  spec.id = JobId::make();
  spec.op = std::string(engine::kOpQuantizeModel);
  spec.model.id = e->id;
  spec.model.dir = si.path();
  spec.params = {{"output", staging.string()},
                 {"bits", e->quantize_bits},
                 {"group_size", e->quantize_group}};

  JobRecord rec;
  rec.id = spec.id;
  rec.op = spec.op;
  rec.purpose = "quantize";
  rec.title = std::format("Quantize {}", e->name);
  rec.destination = dest.string();
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  post_("job.queued", spec.id, {{"title", rec.title}, {"op", rec.op},
                                {"purpose", rec.purpose},
                                {"model", e->id}});
  VALTZ_LOG_INFO("models", "quantizing {} from {} ({} bits, groups of {})"
                 " into {}", e->id, si.path().string(), e->quantize_bits,
                 e->quantize_group, dest.string());

  VALTZ_TRY(_engine->submit(std::move(spec),
                            [this, staging, dest](
                                const engine::JobEvent& ev) {
    std::error_code ec;
    switch (ev.kind) {
    case engine::JobEventKind::Started:
      _jobs.update(ev.job, [](JobRecord& r) { r.state = JobState::Running; });
      post_("job.started", ev.job, {{"engine", ev.text}});
      break;
    case engine::JobEventKind::Progress: {
      if (ev.progress >= 0) {
        _jobs.update(ev.job, [&](JobRecord& r) { r.progress = ev.progress; });
      }
      Json d = {{"progress", ev.progress}};
      if (ev.data.is_object()) {
        d.update(ev.data);
      }
      post_("job.progress", ev.job, d);
      break;
    }
    case engine::JobEventKind::Finished: {
      // In place, whole: what was there (a stopped one's) goes first.
      fs::remove_all(dest, ec);
      ec.clear();
      fs::rename(staging, dest, ec);
      _store->rescan();
      if (ec || !fs::is_directory(dest)) {
        const std::string why = ec ? ec.message()
                                   : std::string("nothing was written");
        VALTZ_LOG_ERROR("models", "quantized model not kept: {}", why);
        finish_job_(ev.job, JobState::Failed, why);
        post_("job.failed", ev.job, {{"code", "io"}, {"message", why}});
      } else {
        finish_job_(ev.job, JobState::Finished, dest.string());
        post_("job.finished", ev.job, {{"path", dest.string()}});
      }
      post_("models.changed", JobId{});
      break;
    }
    case engine::JobEventKind::Failed:
      fs::remove_all(staging, ec);
      finish_job_(ev.job, JobState::Failed, ev.text);
      post_("job.failed", ev.job, {{"code", to_str(ev.error)},
                                   {"message", ev.text}});
      break;
    case engine::JobEventKind::Cancelled:
      fs::remove_all(staging, ec);
      finish_job_(ev.job, JobState::Cancelled, "");
      post_("job.cancelled", ev.job);
      break;
    default:
      break;
    }
  }));
  return rec.id;
}

// ---- projects ---------------------------------------------------------

namespace {

// Where working copies live: the app's support folder, apart from the
// managed cache (which evicts).
fs::path
working_copies(const AppPaths& paths)
{
  return paths.support / "projects";
}

// The history may hold media past this: min(4 GB, 5 % of the volume's
// free space).
std::uint64_t
history_budget(const fs::path& where)
{
  std::error_code ec;
  const auto space = fs::space(where, ec);
  const std::uint64_t four = std::uint64_t{4} << 30;
  return ec ? four
            : std::min<std::uint64_t>(four, space.available / 20);
}

}

Result<ProjectId>
Controller::adopt_(std::unique_ptr<project::Workspace> ws,
                   const project::WorkspaceReport& report)
{
  project::Workspace* w = ws.get();
  const ProjectId id = w->project().id();
  Json data = {{"project", id}, {"name", w->project().name()},
               {"path", w->package() ? w->package()->string()
                                     : w->dir().string()},
               {"untitled", w->untitled()},
               {"recovered", report.recovered},
               {"legacy", report.legacy},
               {"migrated", report.migrated}};
  // The extensions the save holds data of that are not here: opened all
  // the same, their parts passed over (DESIGN §5b).
  Json missing = Json::array();
  for (const auto& e : report.extensions) {
    const auto eid = jget<std::string>(e, "id", "");
    const bool here = std::ranges::any_of(
        _extensions,
        [&](const ext::Extension& x) { return x.id == eid && x.admitted(); });
    if (!eid.empty() && !here) {
      missing.push_back(e);
    }
  }
  data["missing_extensions"] = std::move(missing);
  w->undo().on_change([this, id] { post_state_(id); });
  {
    std::lock_guard lk(_projects_mu);
    _projects[id] = OpenProject{std::move(ws), false, data};
  }
  post_("project.opened", JobId{}, data);
  post_state_(id);
  return id;
}

Result<ProjectId>
Controller::create_project(const fs::path& package, std::string name)
{
  VALTZ_ASSIGN(auto ws, project::Workspace::create(
                            package, std::move(name),
                            working_copies(_paths)));
  return adopt_(std::move(ws), {});
}

Result<ProjectId>
Controller::create_untitled(const fs::path& dir, std::string name)
{
  VALTZ_ASSIGN(auto ws, project::Workspace::create_untitled(
                            dir, std::move(name)));
  return adopt_(std::move(ws), {});
}

Result<ProjectId>
Controller::open_project(const fs::path& package)
{
  {
    // One working copy per package in a process.
    std::lock_guard lk(_projects_mu);
    for (const auto& [id, op] : _projects) {
      std::error_code ec;
      if (op.ws->package() &&
          fs::equivalent(*op.ws->package(), package, ec)) {
        return id;
      }
    }
  }
  project::WorkspaceReport report;
  VALTZ_ASSIGN(auto ws, project::Workspace::open(
                            package, working_copies(_paths), &report));
  VALTZ_ASSIGN(const ProjectId id, adopt_(std::move(ws), report));
  // Originals on other drives may have moved, changed or gone offline
  // since the project was last open.
  auto r = refresh_links(id);
  (void)r;
  return id;
}

project::Workspace*
Controller::workspace_(ProjectId id) const
{
  std::lock_guard lk(_projects_mu);
  auto it = _projects.find(id);
  return it == _projects.end() ? nullptr : it->second.ws.get();
}

project::UndoLog::Scope
Controller::command_(ProjectId pid, std::string kind, AssetId asset,
                     std::string layer, Json args)
{
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return {};
  }
  return w->undo().command(std::move(kind), std::move(args),
                           asset.is_nil() ? "" : asset.str(),
                           std::move(layer));
}

project::UndoLog::Scope
Controller::join_(ProjectId pid, JobId job)
{
  project::Workspace* w = workspace_(pid);
  std::uint64_t seq = 0;
  {
    std::lock_guard lk(_job_commands_mu);
    if (auto it = _job_commands.find(job); it != _job_commands.end()) {
      seq = it->second;
    }
  }
  if (!w || seq == 0) {
    return command_(pid, "generate");
  }
  auto s = w->undo().join(seq);
  {
    // Joined elsewhere (a command of its own), later joins follow it.
    std::lock_guard lk(_job_commands_mu);
    _job_commands[job] = s.seq();
  }
  return s;
}

Json
Controller::extensions_used_(const project::Project& p) const
{
  Json out = Json::array();
  std::set<std::string> seen;
  auto assets = p.assets();
  if (!assets.ok()) {
    return out;
  }
  for (const auto& a : *assets) {
    if (a.recipe.is_nil()) {
      continue;
    }
    auto r = p.recipe(a.recipe);
    if (!r.ok()) {
      continue;
    }
    const auto* m = _catalog.find(r->model);
    if (!m || m->origin.empty() || !seen.insert(m->origin).second) {
      continue;
    }
    Json e = {{"id", m->origin}, {"version", m->origin_version}};
    for (const auto& x : _extensions) {
      if (x.id == m->origin) {
        e["name"] = x.name;
      }
    }
    out.push_back(std::move(e));
  }
  return out;
}

bool
Controller::project_busy_(ProjectId pid) const
{
  for (const auto& j : _jobs.list()) {
    if (j.project == pid && (j.state == JobState::Queued ||
                             j.state == JobState::Running)) {
      return true;
    }
  }
  return false;
}

Json
Controller::project_state(ProjectId pid) const
{
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return Json::object();
  }
  auto brief = [](const std::optional<project::UndoCommand>& c) -> Json {
    if (!c) {
      return nullptr;
    }
    return {{"kind", c->kind}, {"args", c->args}, {"asset", c->asset}};
  };
  return {{"project", pid},
          {"dirty", w->dirty()},
          {"untitled", w->untitled()},
          {"package", w->package() ? w->package()->string() : ""},
          {"name", w->project().name()},
          {"busy", project_busy_(pid)},
          {"undo", brief(w->undo().next_undo())},
          {"redo", brief(w->undo().next_redo())}};
}

Json
Controller::undo_history(ProjectId pid) const
{
  Json out = Json::array();
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return out;
  }
  for (const auto& c : w->undo().list()) {
    out.push_back({{"seq", c.seq},     {"kind", c.kind},
                   {"args", c.args},   {"asset", c.asset},
                   {"layer", c.layer}, {"at", c.at_ms},
                   {"done", c.done}});
  }
  return out;
}

std::vector<ProjectId>
Controller::open_projects() const
{
  std::lock_guard lk(_projects_mu);
  std::vector<ProjectId> out;
  for (const auto& [id, op] : _projects) {
    out.push_back(id);
  }
  return out;
}

Json
Controller::open_report(ProjectId pid) const
{
  std::lock_guard lk(_projects_mu);
  auto it = _projects.find(pid);
  return it == _projects.end() ? Json::object() : it->second.opened;
}

void
Controller::post_state_(ProjectId pid)
{
  Json s = project_state(pid);
  if (!s.empty()) {
    post_("project.changed", JobId{}, std::move(s));
  }
}

Status
Controller::save_project(ProjectId pid)
{
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (w->untitled()) {
    return make_error(Code::InvalidArgument, msg::kProjectUntitled);
  }
  VALTZ_TRY(w->save(extensions_used_(w->project())));
  (void)w->collect_garbage(history_budget(w->dir()));
  post_state_(pid);
  return ok_status();
}

Status
Controller::save_project_as(ProjectId pid, const fs::path& package)
{
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_TRY(w->save_as(package, extensions_used_(w->project())));
  (void)w->collect_garbage(history_budget(w->dir()));
  {
    // A named project from now on: its thumbnails go where every
    // project's go.
    std::lock_guard lk(_projects_mu);
    if (auto it = _projects.find(pid); it != _projects.end()) {
      it->second.ephemeral = false;
    }
  }
  post_("project.renamed", JobId{},
        {{"project", pid}, {"name", w->project().name()},
         {"path", w->package()->string()}});
  post_state_(pid);
  return ok_status();
}

Status
Controller::revert_project(ProjectId pid)
{
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (project_busy_(pid)) {
    return make_error(Code::Busy, msg::kProjectBusy);
  }
  VALTZ_TRY(w->revert());
  post_("assets.changed", JobId{}, {{"project", pid},
                                    {"reason", "revert"}});
  post_state_(pid);
  return ok_status();
}

Status
Controller::undo(ProjectId pid)
{
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // Taking back a task's request withdraws the task.
  if (const auto next = w->undo().next_undo()) {
    withdraw_tasks_(pid, next->seq);
  }
  VALTZ_ASSIGN(auto c, w->undo().undo());
  post_("assets.changed", JobId{}, {{"project", pid},
                                    {"asset", c.asset},
                                    {"reason", "undo"},
                                    {"kind", c.kind}});
  return ok_status();
}

Status
Controller::redo(ProjectId pid)
{
  project::Workspace* w = workspace_(pid);
  if (!w) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(auto c, w->undo().redo());
  post_("assets.changed", JobId{}, {{"project", pid},
                                    {"asset", c.asset},
                                    {"reason", "redo"},
                                    {"kind", c.kind}});
  return ok_status();
}

void
Controller::withdraw_tasks_(ProjectId pid, std::uint64_t seq)
{
  std::vector<JobId> live;
  {
    std::lock_guard lk(_job_commands_mu);
    for (const auto& [job, s] : _job_commands) {
      if (s != seq) {
        continue;
      }
      const auto rec = _jobs.get(job);
      if (rec && rec->project == pid &&
          (rec->state == JobState::Queued ||
           rec->state == JobState::Running)) {
        _withdrawn.insert(job);
        live.push_back(job);
      }
    }
  }
  for (const JobId& job : live) {
    VALTZ_LOG_INFO("build", "job {}: its request undone, withdrawn",
                   job.str());
    (void)_engine->cancel(job);
  }
}

bool
Controller::withdrawn_(JobId job) const
{
  std::lock_guard lk(_job_commands_mu);
  return _withdrawn.contains(job);
}

Json
Controller::tasks() const
{
  std::vector<JobRecord> live;
  for (auto& j : _jobs.list()) {
    if ((j.purpose == "build" || j.purpose == "export") &&
        (j.state == JobState::Queued || j.state == JobState::Running) &&
        !withdrawn_(j.id)) {
      live.push_back(std::move(j));
    }
  }
  // The order the engine takes them: the running one, then as asked
  // (an id is made in time order).
  std::ranges::sort(live, [](const JobRecord& a, const JobRecord& b) {
    const bool ra = a.state == JobState::Running;
    const bool rb = b.state == JobState::Running;
    if (ra != rb) {
      return ra;
    }
    if (a.created_ms != b.created_ms) {
      return a.created_ms < b.created_ms;
    }
    return a.id.str() < b.id.str();
  });
  Json out = Json::array();
  int position = 0;
  for (const auto& j : live) {
    const bool upscale = j.op == engine::kOpUpscaleVideo ||
                         j.op == engine::kOpUpscaleImage;
    const bool exp = j.purpose == "export";
    Json t = {{"job", j.id.str()},
              {"project", j.project.str()},
              {"asset", exp ? std::string() : j.asset.str()},
              {"kind", exp ? "export" : upscale ? "upscale" : "generate"},
              {"op", j.op},
              {"title", j.title},
              {"state", to_str(j.state)},
              {"position", position++},
              {"runner", "local"},
              {"progress", j.progress}};
    if (exp) {
      t["source"] = j.asset.str();
      t["destination"] = j.destination;
    } else if (upscale) {
      std::lock_guard lk(_upscale_mu);
      if (auto it = _upscale_targets.find(j.asset);
          it != _upscale_targets.end()) {
        t["source"] = it->second.composition.str();
        t["layer"] = it->second.layer;
      }
    }
    out.push_back(std::move(t));
  }
  return out;
}

Result<JobId>
Controller::refresh_links(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(auto assets, p->assets());
  std::vector<AssetId> linked;
  for (const auto& a : assets) {
    if (a.linked) {
      linked.push_back(a.id);
    }
  }
  JobId jid = JobId::make();
  if (linked.empty()) {
    return jid;  // nothing to do; no events
  }
  JobRecord rec;
  rec.id = jid;
  rec.op = "refresh-links";
  rec.purpose = "refresh-links";
  rec.title = "Check linked originals";
  rec.project = pid;
  rec.state = JobState::Running;
  rec.created_ms = project::now_ms();
  _jobs.add(rec);

  std::lock_guard lk(_bg_mu);
  _bg.emplace_back([this, p, pid, jid, linked] {
    int missing = 0;
    for (const auto& id : linked) {
      if (_shutting_down.load()) {
        break;
      }
      auto r = p->refresh_link(id);
      if (!r.ok()) {
        VALTZ_LOG_WARN("link", "{}: {}", id.str(), r.error().message);
        continue;
      }
      if (r->state == project::LinkState::Missing) {
        ++missing;
      }
      if (r->state != project::LinkState::Ok) {
        post_("assets.changed", jid,
              {{"project", pid}, {"asset", id},
               {"reason", std::string("link-") + to_str(r->state)}});
      }
    }
    finish_job_(jid, JobState::Finished,
                missing ? std::format("{} originals missing", missing) : "");
    post_("job.finished", jid, {{"project", pid}, {"missing", missing}});
  });
  return jid;
}

Result<fs::path>
Controller::thumbnail(ProjectId pid, AssetId aid, int max_px, bool plain)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  max_px = std::clamp(max_px, 32, 2048);
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  // A composition or a markup shows as it is drawn (a timeline: its first
  // frame), keyed by what it draws; anything else, its file. (`plain`
  // has no meaning for a drawn asset: it has no file underneath.)
  (void)plain;
  const bool drawn = a.cls == project::AssetClass::Still ||
                     a.cls == project::AssetClass::Composition ||
                     a.cls == project::AssetClass::Markup;
  ContentHash content;
  media::MediaType type = media::MediaType::Image;
  std::function<Result<fs::path>()> drawn_file;
  fs::path src;
  if (drawn) {
    VALTZ_ASSIGN(content, p->content_key(aid));
    if (a.kind == project::AssetKind::Audio) {
      type = media::MediaType::Audio;
      drawn_file = [&]() { return rendered_(pid, aid, 0, 0); };
    } else {
      drawn_file = [&]() { return rendered_frame_(pid, aid, 0, 0, 0); };
    }
  } else {
    VALTZ_ASSIGN(project::AssetVersion v, p->version(aid));
    if (v.content.is_zero()) {
      return make_error(Code::NotFound, msg::kAssetHasNoContent);
    }
    content = v.content;
    type = v.info.type;
    VALTZ_ASSIGN(src, p->media_path(aid));
    drawn_file = [&]() -> Result<fs::path> { return src; };
  }
  const std::string key = cache::CacheStore::key_for(
      content, std::format("thumb-{}.jpg", max_px));
  if (is_ephemeral_(pid)) {
    // Kept in the package: nothing of the session in the shared cache.
    const fs::path dir = p->blobs().tmp_dir() / "thumbs";
    const fs::path out = dir / (key + ".jpg");
    std::error_code ec;
    if (fs::exists(out, ec)) {
      return out;
    }
    fs::create_directories(dir, ec);
    VALTZ_ASSIGN(fs::path from, drawn_file());
    VALTZ_TRY(media::write_thumbnail(from, type, max_px, out));
    return out;
  }
  if (auto hit = _cache->find(key)) {
    return *hit;
  }
  VALTZ_ASSIGN(fs::path from, drawn_file());
  VALTZ_TRY(media::write_thumbnail(from, type, max_px,
                                   _cache->staging_path(key)));
  return _cache->commit(key);
}

Status
Controller::set_ephemeral(ProjectId id, bool on)
{
  std::lock_guard lk(_projects_mu);
  auto it = _projects.find(id);
  if (it == _projects.end()) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  it->second.ephemeral = on;
  return ok_status();
}

bool
Controller::is_ephemeral_(ProjectId id) const
{
  std::lock_guard lk(_projects_mu);
  auto it = _projects.find(id);
  return it != _projects.end() && it->second.ephemeral;
}

Status
Controller::close_project(ProjectId id, bool discard)
{
  std::unique_ptr<project::Workspace> ws;
  {
    std::lock_guard lk(_projects_mu);
    auto it = _projects.find(id);
    if (it == _projects.end()) {
      return make_error(Code::NotFound, msg::kProjectNotOpen);
    }
    ws = std::move(it->second.ws);
    _projects.erase(it);
  }
  if (ws) {
    if (!discard) {
      (void)ws->collect_garbage(history_budget(ws->dir()));
    }
    // Kept for its history (valtzctl), or when dirty -- to be resumed.
    if (_keep_working_copies && !discard) {
      ws.reset();
    } else {
      (void)ws->close(discard);
    }
  }
  post_("project.closed", JobId{}, {{"project", id}});
  return ok_status();
}

project::Project*
Controller::project(ProjectId id) const
{
  project::Workspace* w = workspace_(id);
  return w ? &w->project() : nullptr;
}

Result<JobId>
Controller::import_files(ProjectId pid, std::vector<fs::path> files,
                         project::ImportOptions opts)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  if (files.empty()) {
    return make_error(Code::InvalidArgument, msg::kNothingToImport);
  }
  JobRecord rec;
  rec.id = JobId::make();
  rec.op = "import";
  rec.purpose = "import";
  rec.title = files.size() == 1
                  ? std::format("Import {}", files[0].filename().string())
                  : std::format("Import {} files", files.size());
  rec.project = pid;
  rec.state = JobState::Running;
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  post_("job.queued", rec.id, {{"title", rec.title}, {"op", rec.op},
                               {"purpose", rec.purpose}});

  JobId jid = rec.id;
  std::lock_guard lk(_bg_mu);
  _bg.emplace_back([this, p, pid, jid, files = std::move(files), opts] {
    // One command, every file of it.
    auto undo = command_(pid, "import", {}, "",
                         {{"count", files.size()}});
    Json failed = Json::array();
    int n = 0;
    for (std::size_t i = 0; i < files.size(); ++i) {
      if (_shutting_down.load()) {
        break;
      }
      auto r = p->import_file(files[i], opts);
      if (r.ok()) {
        ++n;
        post_("assets.changed", jid, {{"project", pid}, {"asset", r->id},
                                      {"reason", "imported"}});
      } else {
        VALTZ_LOG_WARN("import", "{}: {}", files[i].string(),
                       r.error().message);
        failed.push_back({{"file", files[i].string()},
                          {"message", r.error().message}});
      }
      float prog = static_cast<float>(i + 1) / files.size();
      _jobs.update(jid, [&](JobRecord& rr) { rr.progress = prog; });
      post_("job.progress", jid, {{"progress", prog}});
    }
    finish_job_(jid, failed.empty() ? JobState::Finished : JobState::Failed,
                "");
    if (failed.empty()) {
      post_("job.finished", jid, {{"project", pid}, {"imported", n}});
    } else {
      Json d = message_fields(msg::kImportFailedSome,
                              {{"failed", std::to_string(failed.size())},
                               {"total", std::to_string(n + failed.size())}});
      d["code"] = "io";
      d["imported"] = n;
      d["failed"] = failed;
      post_("job.failed", jid, d);
    }
  });
  return jid;
}

// ---- assistant --------------------------------------------------------

bool
Controller::assistant_mtp(const models::ModelEntry& m) const
{
  return _assistant_mtp &&
         jget(jget(m.engine, "vpipe", Json::object()), "mtp", true);
}

Status
Controller::choose_assistant(const std::string& id)
{
  if (!id.empty()) {
    const auto* m = _catalog.find(id);
    if (!m) {
      return make_error(Code::NotFound, msg::kUnknownModel, {{"model", id}});
    }
    if (m->role != "assistant") {
      return make_error(Code::InvalidArgument, msg::kNotAnAssistant,
                        {{"model", m->name}});
    }
  }
  const fs::path file = _paths.support / "assistant.json";
  Json j = read_settings(file);
  if (!j.is_object()) {
    j = Json::object();
  }
  j["model"] = id;
  VALTZ_TRY(write_settings(file, j));
  _assistant_choice = id;
  post_("assistant.changed", JobId{}, {{"choice", id}});
  return ok_status();
}

Status
Controller::set_assistant_keep_loaded(double seconds)
{
  seconds = std::max(0.0, seconds);
  const fs::path file = _paths.support / "assistant.json";
  Json j = read_settings(file);
  j["keep_loaded"] = seconds;
  VALTZ_TRY(write_settings(file, j));
  _assistant_keep = seconds;
  post_("assistant.changed", JobId{}, {{"keep_loaded", seconds}});
  return ok_status();
}

Status
Controller::set_assistant_drafter(const std::string& kind, int bits)
{
  if (kind != "mtp" && kind != "dflash") {
    return make_error(Code::InvalidArgument,
                      std::format("no drafter {} (mtp or dflash)", kind));
  }
  bits = bits == 4 ? 4 : 8;
  const fs::path file = _paths.support / "assistant.json";
  Json j = read_settings(file);
  j["drafter"] = kind;
  j["drafter_bits"] = bits;
  VALTZ_TRY(write_settings(file, j));
  _assistant_drafter = kind;
  _assistant_drafter_bits = bits;
  post_("assistant.changed", JobId{}, {{"drafter", kind}, {"bits", bits}});
  return ok_status();
}

Json
Controller::assistants() const
{
  const auto* pick = models::pick_assistant(_catalog, *_store, _hw);
  const auto* in_use = assistant_model();
  Json list = Json::array();
  for (const auto* e : _catalog.serving(models::Capability::PromptEnhance)) {
    if (e->role != "assistant") {
      continue;
    }
    const Json vp = jget(e->engine, "vpipe", Json::object());
    // pick_assistant's measure: the RAM it is offered from, and every
    // weight resident in what the GPU keeps.
    const bool fits =
        _hw.ram_gb() >= e->min_ram_gb &&
        (_hw.gpu_working_set_bytes == 0 ||
         e->disk_gb * 1e9 <= static_cast<double>(_hw.gpu_working_set_bytes));
    Json row = {{"id", e->id},
                {"name", e->name},
                {"state", models::to_str(_store->info(*e).state)},
                {"fits", fits},
                {"disk_gb", e->disk_gb},
                {"min_ram_gb", e->min_ram_gb},
                {"rank", e->rank},
                {"mtp", jget(vp, "mtp", true)},
                {"sampling", jget(vp, "sampling", Json::object())}};
    if (const auto* d = _catalog.find(
            jget<std::string>(vp, "mtp_drafter", ""))) {
      row["drafter"] = {{"id", d->id},
                        {"name", d->name},
                        {"state", models::to_str(_store->info(*d).state)}};
    }
    if (const auto* d = _catalog.find(
            jget<std::string>(vp, "dflash_drafter", ""))) {
      row["dflash"] = {{"id", d->id},
                       {"name", d->name},
                       {"state", models::to_str(_store->info(*d).state)},
                       {"disk_gb", d->disk_gb}};
    }
    list.push_back(std::move(row));
  }
  return {{"choice", _assistant_choice},
          {"auto", pick ? pick->id : std::string()},
          {"using", in_use ? in_use->id : std::string()},
          {"keep_loaded", _assistant_keep},
          {"drafter", _assistant_drafter},
          {"drafter_bits", _assistant_drafter_bits},
          {"models", std::move(list)}};
}

std::string
Controller::ReplyForm::apply(std::string prompt) const
{
  if (!lead.empty() && !prompt.empty()) {
    prompt = lead + "\n\n" + prompt;
  }
  return assist::retag(prompt, names);
}

Result<JobId>
Controller::submit_chat_(JobId id, std::string purpose, std::string text,
                         std::vector<engine::JobInput> images,
                         int max_new_tokens, std::string song,
                         ReplyForm form)
{
  const auto* m = assistant_model();
  if (!m) {
    return make_error(Code::Unsupported, msg::kNoAssistantModel);
  }
  VALTZ_ASSIGN(engine::ModelRef ref, resolve_model_(*m, false));

  engine::JobSpec spec;
  spec.id = id;
  spec.op = std::string(engine::kOpChat);
  spec.model = ref;
  // Without thinking, decoded with the model's own sampler (catalog
  // engine.vpipe.sampling: Qwen's non-thinking recommendation); MTP
  // drafts under it as it does greedy.
  const Json vp = jget(m->engine, "vpipe", Json::object());
  spec.params = {{"text", std::move(text)},
                 {"disable_thinking", true},
                 {"max_new_tokens", max_new_tokens},
                 {"mtp", assistant_mtp(*m)},
                 {"sampling", jget(vp, "sampling", Json::object())},
                 {"keep_loaded", _assistant_keep}};
  // A model whose MTP head ships apart (engine.vpipe.mtp_drafter): the
  // drafter's directory, when it is installed.
  if (assistant_mtp(*m)) {
    if (const auto* d = _catalog.find(jget<std::string>(
            jget(m->engine, "vpipe", Json::object()), "mtp_drafter", ""));
        d && _store->info(*d).state == models::InstallState::Installed) {
      spec.params["mtp_model"] = _store->info(*d).path().string();
    }
  }
  // DFlash 2 when chosen and here (it takes over from MTP in text-chat);
  // else MTP, as above.
  if (_assistant_drafter == "dflash") {
    if (const auto* d = _catalog.find(
            jget<std::string>(vp, "dflash_drafter", ""));
        d && _store->info(*d).state == models::InstallState::Installed) {
      spec.params["draft_model"] = _store->info(*d).path().string();
      spec.params["draft_bits"] = _assistant_drafter_bits;
    }
  }
  spec.inputs = std::move(images);

  JobRecord rec;
  rec.id = id;
  rec.op = spec.op;
  rec.purpose = purpose;
  rec.title = purpose == "enhance" ? "Enhance prompt" : "Understand request";
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  post_("job.queued", id, {{"title", rec.title}, {"op", rec.op},
                           {"purpose", purpose}});
  // What the assistant has written so far, and the suggestion read out of
  // it each time it grows (assist.partial): the person watches it come
  // rather than waiting for the whole reply.
  struct Written {
    std::mutex mu;
    std::string text;
    std::string shown;
  };
  auto written = std::make_shared<Written>();
  VALTZ_TRY(_engine->submit(std::move(spec),
                            [this, purpose, song = std::move(song),
                             form = std::move(form),
                             written](const engine::JobEvent& ev) {
    if (ev.kind == engine::JobEventKind::Text && purpose == "enhance") {
      std::string now;
      {
        std::lock_guard lk(written->mu);
        written->text += ev.text;
        now = form.apply(assist::partial_enhance_reply(written->text));
        if (now.empty() || now == written->shown) {
          now.clear();
        } else {
          written->shown = now;
        }
      }
      if (!now.empty()) {
        post_("assist.partial", ev.job, {{"prompt", std::move(now)}});
      }
    }
    on_chat_event_(ev, purpose, song, form);
  }));
  return id;
}

void
Controller::on_chat_event_(const engine::JobEvent& ev,
                           const std::string& purpose,
                           const std::string& song, const ReplyForm& form)
{
  switch (ev.kind) {
  case engine::JobEventKind::Started:
    _jobs.update(ev.job, [](JobRecord& r) { r.state = JobState::Running; });
    post_("job.started", ev.job, {{"engine", ev.text}});
    break;
  case engine::JobEventKind::Text:
    post_("job.text", ev.job, {{"text", ev.text}});
    break;
  case engine::JobEventKind::Output:
    if (purpose == "enhance") {
      auto r = assist::parse_enhance_reply(ev.text);
      if (r.ok()) {
        if (!song.empty()) {
          r->prompt = assist::keep_song_lyrics(r->prompt, song);
        }
        r->prompt = form.apply(std::move(r->prompt));
        post_("assist.enhanced", ev.job, Json(*r));
      } else {
        // The detail (why the reply did not parse) is for the log.
        VALTZ_LOG_WARN("assist", "unreadable reply: {}", r.error().message);
        Json d = message_fields(msg::kEnhanceUnreadable);
        d["code"] = "corrupt";
        d["raw"] = ev.text;
        post_("job.failed", ev.job, d);
      }
    } else if (purpose == "intent") {
      auto r = assist::parse_intent_reply(ev.text);
      if (r.ok()) {
        post_("assist.intent", ev.job, Json(*r));
      }
    }
    break;
  case engine::JobEventKind::Finished:
    finish_job_(ev.job, JobState::Finished, "");
    post_("job.finished", ev.job);
    break;
  case engine::JobEventKind::Failed:
    finish_job_(ev.job, JobState::Failed, ev.text);
    post_("job.failed", ev.job, {{"code", to_str(ev.error)},
                                 {"message", ev.text}});
    break;
  case engine::JobEventKind::Cancelled:
    finish_job_(ev.job, JobState::Cancelled, "");
    post_("job.cancelled", ev.job);
    break;
  default:
    break;
  }
}

Result<JobId>
Controller::enhance_prompt(EnhancePromptRequest req)
{
  // Auto's pick for what Start would make: a clip from references is
  // its video `edit` list's (Ref2VA), as generate_video picks.
  const bool clip_refs = req.target == "video" &&
                         (!req.references.empty() || req.continue_from);
  // Sound with a voice in its row to clone: speech (Auto's audio
  // `edit` list), as generate_audio picks.
  bool voice = false;
  if (req.target == "audio" && req.project) {
    if (project::Project* vp = project(*req.project)) {
      for (const auto& id : req.row.empty() ? req.references : req.row) {
        voice = voice || voiced_(*vp, id);
      }
    }
  }
  if (req.model == "auto") {
    req.model.clear();
    const char* op = clip_refs || voice ? "edit" : "generate";
    for (const auto& a : auto_models()) {
      if (a.modality == req.target && a.op == op) {
        req.model = a.chosen;
      }
    }
  }
  const models::ModelEntry* m =
      req.model.empty() ? nullptr : _catalog.find(req.model);
  project::Project* p = req.project ? project(*req.project) : nullptr;
  std::vector<AssetId> row = req.row;
  if (row.empty()) {
    if (req.base) {
      row.push_back(*req.base);
    }
    row.insert(row.end(), req.references.begin(), req.references.end());
  }
  // Its tags as mentions of what they name (none: dropped).
  if (p) {
    VALTZ_ASSIGN(PromptForm pf, prompt_form_(*p, req.prompt,
                                             req.inline_pictures, row,
                                             false));
    req.prompt = pf.text;
    req.inline_pictures = pf.inline_media;
  }
  if (assist::prompt_words(req.prompt).empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  std::vector<AssetId> pictures;
  if (p) {
    pictures = edit_pictures_(*p, req.base, req.references);
  }
  const bool edit = m && m->has(models::Capability::ImageEdit) &&
                    (req.mode == "edit" ||
                     (req.mode == "auto" && !pictures.empty()));

  assist::EnhanceRequest er;
  er.target = req.target;
  er.style = req.style;
  er.size = req.size;
  er.seconds = req.seconds;
  er.speech = m && m->has(models::Capability::TextToSpeech);
  er.voice = voice;
  // Translated only for a model that reads English alone.
  er.english = m && jget<std::string>(m->prompting, "language", "") ==
                        "english";
  // The model's makers' rewriter when they publish one; a skill of
  // Valtz's own (YuE2's song writer) or an extension's.
  const std::string skill = m ? jget<std::string>(
      jget(m->prompting, "enhance", Json::object()),
      edit ? "edit" : "generate", "") : std::string();
  if (m) {
    er.system = skill_text(skill);
  }
  const int most = m ? max_pictures(*m) : 0;
  er.prompt = assist::tag_pictures(
      req.prompt, inline_numbers(req.inline_pictures, pictures, req.base,
                                 most),
      edit && m ? jget<std::string>(m->prompting, "reference_tag", "")
                : std::string());
  // A rewriter that reads pictures is shown them, in the model's order.
  std::vector<engine::JobInput> images;
  if (edit && p && !er.system.empty()) {
    for (const auto& id : pictures) {
      if (static_cast<int>(images.size()) >= most) {
        break;
      }
      auto path = p->media_path(id);
      auto v = p->version(id);
      if (path.ok() && v.ok()) {
        images.push_back({"image", *path, v->content, v->info});
      }
    }
    er.pictures = static_cast<int>(images.size());
  }
  // A clip written to MiniMax H3's own guide (an extension's H3 may
  // name it too): what the guide leaves to the writer -- the mode, the
  // length, the references by the model's names -- a reply a section
  // at a time, and those names put back as the row's tags. Any other
  // skill keeps its own reply (docs/EXTENSIONS.md).
  ReplyForm form;
  const bool clip_skill = req.target == "video" && m &&
                          !er.system.empty() &&
                          (skill == "minimax-h3-base" ||
                           skill == "minimax-h3-ref");
  if (clip_skill) {
    clip_request_(p, *m, req, row, er, images, form);
  }
  // Its instructions ask for a long description, reasoned toward first
  // (a full-reference clip's runs to 500 words and more); a song's, for
  // its lyrics.
  const int tokens = clip_skill             ? 4096
                     : !er.system.empty()   ? 2048
                     : er.target == "audio" ? 1536
                                            : 512;
  // A song's own lyrics are the person's words: kept as written,
  // whatever the reply did with them.
  return submit_chat_(JobId::make(), "enhance",
                      assist::build_enhance_prompt(er), std::move(images),
                      tokens,
                      er.target == "audio" && !er.speech ? er.prompt : "",
                      std::move(form));
}

void
Controller::clip_request_(
    project::Project* p, const models::ModelEntry& m,
    const EnhancePromptRequest& req, const std::vector<AssetId>& row,
    assist::EnhanceRequest& er, std::vector<engine::JobInput>& images,
    ReplyForm& form)
{
  auto& names = form.names;
  const Json vp = jget(m.engine, "vpipe", Json::object());
  const Json defaults = jget(vp, "defaults", Json::object());
  const double fps = req.fps > 0 ? req.fps : jget(defaults, "fps", 24.0);
  const int frames =
      req.frames > 0 ? req.frames : jget(defaults, "frames", 124);
  er.seconds = fps > 0 ? frames / fps : 0;
  // Where each of the row's media is, as the prompt box tags it.
  auto row_tag = [&](const std::string& id) -> std::string {
    if (!p) {
      return {};
    }
    std::map<project::AssetKind, int> seen;
    for (const auto& r : row) {
      auto a = p->asset(r);
      if (!a.ok()) {
        continue;
      }
      const int n = seen[a->kind]++;
      if (r.str() != id) {
        continue;
      }
      using K = project::AssetKind;
      const auto k = a->kind == K::Image   ? assist::RefKind::Image
                     : a->kind == K::Video ? assist::RefKind::Video
                     : a->kind == K::Audio ? assist::RefKind::Audio
                                           : assist::RefKind::Other;
      return k == assist::RefKind::Other ? std::string()
                                         : assist::ref_tag(k, n);
    }
    return {};
  };
  auto seconds_of = [&](AssetId id) {
    auto v = p->version(id);
    return v.ok() ? v->info.duration.seconds() : 0.0;
  };
  // What the assistant is shown: a picture as it looks, a frame of a
  // clip (its thumbnail, from the managed cache) -- it cannot watch one.
  auto show = [&](AssetId id) -> bool {
    auto path = thumbnail(*req.project, id, 512);
    if (!path.ok()) {
      return false;
    }
    images.push_back({"image", *path, {}, {}});
    return true;
  };
  // Each mention as the model names its medium; the rest dropped, as
  // generate_video drops them.
  std::map<std::string, std::string> tag_of;
  const Json fmt = jget(m.prompting, "reference_tags", Json::object());
  if (m.has(models::Capability::ReferenceToVideo)) {
    er.video_mode = "Ref2VA";
    GenerateVideoRequest v;
    v.references = req.references;
    v.continue_from = req.continue_from;
    auto media = p ? video_references_(*p, v, m, fps)
                   : Result<Json>(Json::object());
    // A row the model cannot take is Start's to say; written without.
    const Json list = media.ok() ? jget(*media, "list", Json::array())
                                 : Json::array();
    const auto sound = media.ok() ? jget<std::string>(*media, "sound", "")
                                  : std::string();
    int audio = 0;
    // The summary's task types (the guide's), as each is used here.
    bool continues = false, guided = false, reused = false, heard = false;
    for (const auto& e : list) {
      const auto id = AssetId::parse(jget<std::string>(e, "asset", ""));
      if (!id) {
        continue;
      }
      auto a = p->asset(*id);
      if (!a.ok()) {
        continue;
      }
      const auto kind = jget<std::string>(e, "kind", "");
      const std::string tag = jget<std::string>(
          jget(*media, "tags", Json::object()), id->str().c_str(), "");
      tag_of[id->str()] = tag;
      if (const auto t = row_tag(id->str()); !t.empty() && !tag.empty()) {
        names.emplace_back(tag, t);
      }
      std::string line;
      if (kind == "image") {
        guided = true;
        line = std::format("{}: a picture, \"{}\"", tag, a->name);
        if (show(*id)) {
          line += std::format(" (image {} shown above)", images.size());
        }
        line += " -- who or what to show, defined as a <Subject N> in it; "
                "not a frame of the video";
      } else if (kind == "video") {
        line = std::format("{}: a clip, \"{}\"", tag, a->name);
        if (const double s = seconds_of(*id); s > 0) {
          line += std::format(", {:.1f} s", s);
        }
        if (show(*id)) {
          line += std::format(" (a frame of it: image {} shown above)",
                              images.size());
        }
        if (jget<std::string>(e, "role", "") == "continue") {
          continues = heard = true;
          const double tail = jget(e, "tail_frames", 0) / fps;
          line += std::format(
              " -- the clip the target video continues from: it picks up "
              "where it ends (its last {:.2f} seconds are read)", tail);
        } else {
          guided = true;
          line += " -- its subjects, motion or camera to follow, each "
                  "used defined as a <Subject N> in it";
        }
        if (jget(e, "audio", false)) {
          line += std::format("; its sound is <Audio {}>", ++audio);
        }
      } else {
        ++audio;
        line = std::format("{}: a sound, \"{}\"", tag, a->name);
        if (const double s = seconds_of(*id); s > 0) {
          line += std::format(", {:.1f} s", s);
        }
        if (req.reference_sound && id->str() == sound) {
          reused = true;
          line += " -- the video's soundtrack, copied whole (audio "
                  "reuse: fully_copy)";
        } else {
          heard = true;
          line += " -- followed, not copied: a voice's timbre, a song's "
                  "style or tempo, as the request says (audio reference)";
        }
      }
      er.references.push_back(std::move(line));
    }
    std::string tasks;
    auto task = [&](bool on, const char* t) {
      if (on) {
        tasks += (tasks.empty() ? "" : " + ") + std::string(t);
      }
    };
    task(continues, "video continuation");
    task(guided || list.empty(), "reference generation");
    task(reused, "audio reuse");
    task(heard, "audio reference");
    er.video_tasks = tasks;
  } else if (p && req.base && m.has(models::Capability::ImageToVideo) &&
             jget(vp, "first_frame", false)) {
    auto a = p->asset(*req.base);
    if (a.ok() && a->kind == project::AssetKind::Image) {
      er.video_mode = "I2VA";
      std::string tag = jget<std::string>(fmt, "image", "<Picture {n}>");
      if (const auto at = tag.find("{n}"); at != std::string::npos) {
        tag.replace(at, 3, "1");
      }
      // The guide's I2VA instruction, word for word: Valtz's to write.
      form.lead = std::format("For the target video, at 0.00 seconds into "
                              "the target video, {} (from [Shot 1]) is "
                              "fully referenced.", tag);
      tag_of[req.base->str()] = tag;
      if (const auto t = row_tag(req.base->str()); !t.empty()) {
        names.emplace_back(tag, t);
      }
      std::string line =
          std::format("{}: the picture the video opens on, \"{}\"", tag,
                      a->name);
      if (show(*req.base)) {
        line += std::format(" -- image {} shown above", images.size());
      }
      er.references.push_back(std::move(line));
    } else {
      er.video_mode = "T2VA";
    }
  } else {
    er.video_mode = "T2VA";
  }
  std::vector<std::string> tags;
  for (const auto& id : req.inline_pictures) {
    auto it = id ? tag_of.find(id->str()) : tag_of.end();
    tags.push_back(it != tag_of.end() ? it->second : std::string());
  }
  er.prompt = assist::tag_inline(req.prompt, tags);
  er.pictures = static_cast<int>(images.size());
}

std::vector<AssetId>
Controller::edit_pictures_(project::Project& p,
                           const std::optional<AssetId>& base,
                           const std::vector<AssetId>& refs)
{
  auto is_picture = [&](AssetId id) {
    auto a = p.asset(id);
    return a.ok() && a->kind == project::AssetKind::Image;
  };
  std::vector<AssetId> out;
  const bool b = base && is_picture(*base);
  if (b) {
    out.push_back(*base);
  }
  for (const auto& ref : refs) {
    if (is_picture(ref) && !(b && ref == *base)) {
      out.push_back(ref);
    }
  }
  return out;
}

Result<JobId>
Controller::detect_intent(std::string text,
                          std::vector<assist::Attachment> atts,
                          bool use_model)
{
  JobId id = JobId::make();
  post_("assist.intent", id, Json(assist::heuristic_intent(text, atts)));
  const auto* m = use_model ? assistant_model() : nullptr;
  if (m && _store->info(*m).state == models::InstallState::Installed) {
    auto r = submit_chat_(id, "intent",
                          assist::build_intent_prompt(text, atts));
    if (!r.ok()) {
      VALTZ_LOG_WARN("assist", "intent model unavailable: {}",
                     r.error().message);
    }
  }
  return id;
}

// ---- generation -------------------------------------------------------

// ---- prompts ------------------------------------------------------------

bool
Controller::is_prompt(const project::Asset& a)
{
  return a.kind == project::AssetKind::Text &&
         std::ranges::find(a.tags, std::string("prompt")) != a.tags.end();
}

Result<Controller::PromptForm>
Controller::prompt_form_(project::Project& p, const std::string& prompt,
                         const std::vector<std::optional<AssetId>>&
                             inline_media,
                         const std::vector<AssetId>& row, bool bound)
{
  // The row's kinds, for the tags' places in it.
  std::vector<assist::RefKind> kinds;
  for (const auto& id : row) {
    auto a = p.asset(id);
    using K = project::AssetKind;
    kinds.push_back(!a.ok()                ? assist::RefKind::Other
                    : a->kind == K::Image ? assist::RefKind::Image
                    : a->kind == K::Video ? assist::RefKind::Video
                    : a->kind == K::Audio ? assist::RefKind::Audio
                                          : assist::RefKind::Other);
  }
  // Each mention where its medium sits now (its first place).
  std::vector<int> mentions;
  for (const auto& m : inline_media) {
    int at = -1;
    for (std::size_t i = 0; m && i < row.size(); ++i) {
      if (row[i] == *m) {
        at = static_cast<int>(i);
        break;
      }
    }
    mentions.push_back(at);
  }
  PromptForm f;
  f.positional = assist::positional_prompt(prompt, mentions, kinds);
  if (bound) {
    if (const auto un = assist::unbound_ref_tags(f.positional, kinds);
        !un.empty()) {
      return make_error(Code::InvalidArgument, msg::kPromptRefUnbound,
                        {{"tag", un.front()}});
    }
  }
  std::vector<int> at;
  f.text = assist::mention_form(f.positional, kinds, {}, &at);
  for (int i : at) {
    f.inline_media.push_back(
        i >= 0 ? std::optional<AssetId>(row[static_cast<std::size_t>(i)])
               : std::nullopt);
  }
  return f;
}

Result<project::RecipeInput>
Controller::capture_prompt_(project::Project& p, ProjectId pid,
                            const std::string& text,
                            std::optional<AssetId> from)
{
  // Pinned to the version it is now: what was made from it says which.
  auto pinned = [&](AssetId id) -> Result<project::RecipeInput> {
    VALTZ_ASSIGN(project::Asset a, p.asset(id));
    return project::RecipeInput{"prompt", id, a.head};
  };
  auto told = [&](AssetId id, const char* reason) {
    post_("assets.changed", JobId{}, {{"project", pid},
                                      {"asset", id},
                                      {"reason", reason}});
  };
  std::string name =
      one_line(assist::without_markdown(assist::prompt_words(text)));
  if (name.empty()) {
    name = "Prompt";
  }
  name = std::string(utf8_prefix(name, 64));
  if (from) {
    auto a = p.asset(*from);
    if (a.ok() && is_prompt(*a)) {
      if (auto cur = p.read_text(*from); cur.ok() && *cur == text) {
        return pinned(*from);
      }
      // Nothing made from it yet: it is still a draft, changed in place.
      if (auto deps = p.dependents(*from, false); deps.ok() &&
                                                  deps->empty()) {
        VALTZ_TRY(p.update_text(*from, text));
        (void)p.rename_asset(*from, name);
        told(*from, "edited");
        return pinned(*from);
      }
    }
  }
  // The same words captured before: that prompt, not another.
  VALTZ_ASSIGN(auto all, p.assets());
  for (const auto& a : all) {
    if (is_prompt(a)) {
      if (auto t = p.read_text(a.id); t.ok() && *t == text) {
        return pinned(a.id);
      }
    }
  }
  VALTZ_ASSIGN(project::Asset a, p.add_text(name, text, {"prompt"}));
  told(a.id, "defined");
  return pinned(a.id);
}

Result<AssetId>
Controller::capture_prompt(ProjectId pid, const std::string& prompt,
                           const std::vector<std::optional<AssetId>>&
                               inline_media,
                           const std::vector<AssetId>& row,
                           std::optional<AssetId> from)
{
  auto undo = command_(pid, "prompt.capture");
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // Tags that name nothing yet are kept: a form, filled in later.
  VALTZ_ASSIGN(PromptForm f, prompt_form_(*p, prompt, inline_media, row,
                                          /*bound=*/false));
  if (assist::prompt_words(f.positional).empty() &&
      assist::find_ref_tags(f.positional).empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  VALTZ_ASSIGN(project::RecipeInput in,
               capture_prompt_(*p, pid, f.positional, from));
  return in.asset;
}

Status
Controller::set_prompt_text(ProjectId pid, AssetId id,
                            const std::string& text)
{
  auto undo = command_(pid, "prompt.text", id);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(id));
  if (!is_prompt(a)) {
    return make_error(Code::InvalidArgument, msg::kNotAPrompt,
                      {{"name", a.name}});
  }
  VALTZ_ASSIGN(auto deps, p->dependents(id, false));
  if (!deps.empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptInUse,
                      {{"name", a.name}});
  }
  VALTZ_TRY(p->update_text(id, text));
  std::string name =
      one_line(assist::without_markdown(assist::prompt_words(text)));
  (void)p->rename_asset(id, std::string(utf8_prefix(
      name.empty() ? std::string("Prompt") : name, 64)));
  post_("assets.changed", JobId{}, {{"project", pid},
                                    {"asset", id},
                                    {"reason", "edited"}});
  return ok_status();
}

Result<JobId>
Controller::generate_image(GenerateImageRequest req)
{
  auto undo = command_(req.project,
                       req.base ? "edit.image" : "generate.image");
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // The prompt against its row: the base, then the references, unless
  // the request says. Mentions and tags in the mention form; the words
  // alone name the result.
  std::vector<AssetId> row = req.row;
  if (row.empty()) {
    if (req.base) {
      row.push_back(*req.base);
    }
    for (const auto& ref : req.references) {
      if (!(req.base && ref == *req.base)) {
        row.push_back(ref);
      }
    }
  }
  VALTZ_ASSIGN(PromptForm pf, prompt_form_(*p, req.prompt,
                                           req.inline_pictures, row, true));
  const std::string words = assist::prompt_words(pf.positional);
  if (words.empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  if (req.mode != "auto" && req.mode != "edit" && req.mode != "generate") {
    return make_error(Code::InvalidArgument, std::format(
        "mode '{}' is not auto, edit or generate", req.mode));
  }
  // The pictures an edit can use (a video reference stays a compare-only
  // input until video graphs land): the base, if it is a picture, then
  // the references.
  const std::vector<AssetId> pictures =
      edit_pictures_(*p, req.base, req.references);
  const bool base = req.base && !pictures.empty() &&
                    pictures.front() == *req.base;
  const bool want_edit = req.mode == "edit" ||
                         (req.mode == "auto" && !pictures.empty());
  const models::ModelEntry* m = nullptr;
  if (!req.model.empty() && req.model != "auto") {
    m = _catalog.find(req.model);
    if (!m) {
      return make_error(Code::NotFound, msg::kUnknownModel,
                        {{"model", req.model}});
    }
  } else {
    // Auto: its edit pick when there are pictures to edit; with none that
    // can edit here, its generate pick -- the pictures stay compare-only
    // (and an edit asked for outright fails below).
    auto pick = [&](const char* op) -> const models::ModelEntry* {
      for (const auto& a : auto_models()) {
        if (a.modality == "image" && a.op == op && !a.chosen.empty()) {
          return _catalog.find(a.chosen);
        }
      }
      return nullptr;
    };
    m = want_edit ? pick("edit") : nullptr;
    if (!m) {
      m = pick("generate");
    }
  }
  const bool edit = want_edit && m && can_edit_(*m);
  if (req.mode == "edit" && !edit) {
    return m ? make_error(Code::Unsupported, msg::kModelCannotEdit,
                          {{"model", m->name}})
             : make_error(Code::Unsupported, msg::kNoEditModel);
  }
  if (edit && pictures.empty()) {
    return make_error(Code::InvalidArgument, msg::kEditNeedsReference);
  }
  if (!edit && (!m || !m->has(models::Capability::TextToImage))) {
    return make_error(Code::NotFound, msg::kNoImageModel);
  }
  // An edit's inline pictures become what the model calls them.
  const std::string prompt = assist::tag_pictures(
      pf.text,
      inline_numbers(pf.inline_media, pictures, req.base,
                     max_pictures(*m)),
      edit ? jget<std::string>(m->prompting, "reference_tag", "")
           : std::string());

  project::Recipe r;
  r.op = std::string(edit ? engine::kOpEditImage : engine::kOpGenerateImage);
  r.model = m->id;
  r.deterministic = false;
  r.params = {{"prompt", templated(*m, prompt)},
              {"seed", req.seed >= 0 ? req.seed : random_seed()}};
  stamp_origin(r, *m);
  if (!req.negative.empty()) {
    r.params["negative"] = req.negative;
  }
  if (req.width > 0) {
    r.params["width"] = req.width;
  }
  if (req.height > 0) {
    r.params["height"] = req.height;
  }
  // Steps are always recorded explicitly, so a snapshot says exactly
  // what ran even if the catalog's defaults change later; so are the
  // Favor options (models/tuning.h).
  VALTZ_TRY(check_preset_loras_(*m, req.preference, req.tuning));
  apply_tuning_(r, *m, models::resolve_tuning(*m, tuning_context(*m),
                                              req.preference, edit,
                                              req.tuning),
                req.steps);
  // The base first: the engine reads input order.
  if (base) {
    r.inputs.push_back({"base", *req.base, 0});
    // What the request says, over the base as it looks: a look of the
    // base's own (its modifiers, a stack) is rendered into the picture
    // the model gets (submit_build_), so it is not asked for again here.
    if (!req.base_adjust.identity()) {
      r.params["base_adjust"] = media::to_json(req.base_adjust);
    }
    if (!req.base_crop.identity()) {
      r.params["base_crop"] = media::to_json(req.base_crop);
    }
  }
  for (const auto& ref : req.references) {
    if (!(base && ref == *req.base)) {
      r.inputs.push_back({"reference", ref, 0});
    }
  }
  // Made from its prompt as well as its pictures: the prompt, captured.
  VALTZ_ASSIGN(project::RecipeInput pin,
               capture_prompt_(*p, req.project, pf.positional,
                               req.prompt_asset));
  r.inputs.push_back(pin);
  // The prompt on one line, cut to 48 bytes and made unique by the
  // project ("…", " (2)").
  std::string name = req.name.empty()
                         ? one_line(assist::without_markdown(words))
                         : req.name;
  VALTZ_ASSIGN(project::Asset a,
               p->define_derived(std::move(name), project::AssetKind::Image,
                                 r, 48));
  post_("assets.changed", JobId{}, {{"project", req.project},
                                    {"asset", a.id},
                                    {"reason", "defined"}});
  return submit_build_(req.project, a.id, a.name);
}

Result<JobId>
Controller::generate_video(GenerateVideoRequest req)
{
  auto undo = command_(req.project, "generate.video");
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // A clip reads the words: inline media are dropped (or, with
  // references, named as the model names them -- below). Its row: the
  // clip continued, then the references -- or the picture it opens on.
  std::vector<AssetId> row = req.row;
  if (row.empty()) {
    if (req.continue_from) {
      row.push_back(*req.continue_from);
    }
    row.insert(row.end(), req.references.begin(), req.references.end());
    if (req.first) {
      row.push_back(*req.first);
    }
  }
  VALTZ_ASSIGN(PromptForm pf, prompt_form_(*p, req.prompt, req.inline_media,
                                           row, true));
  const std::string words = assist::prompt_words(pf.positional);
  if (words.empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  const bool refs = !req.references.empty() || req.continue_from;
  if (refs && req.first) {
    return make_error(Code::InvalidArgument, msg::kVideoFirstOrReferences);
  }
  // With references, Auto's video `edit` pick (Ref2VA); else its generate.
  const char* op = refs ? "edit" : "generate";
  const models::ModelEntry* m = nullptr;
  if (!req.model.empty() && req.model != "auto") {
    m = _catalog.find(req.model);
    if (!m) {
      return make_error(Code::NotFound, msg::kUnknownModel,
                        {{"model", req.model}});
    }
  } else {
    for (const auto& a : auto_models()) {
      if (a.modality == "video" && a.op == op && !a.chosen.empty()) {
        m = _catalog.find(a.chosen);
        break;
      }
    }
  }
  if (refs && (!m || !runs(*m, "video", "edit"))) {
    return make_error(Code::NotFound, msg::kNoReferenceModel);
  }
  if (!refs && (!m || !runs(*m, "video", "generate"))) {
    return make_error(Code::NotFound, msg::kNoVideoModel);
  }
  const Json vp = jget(m->engine, "vpipe", Json::object());
  if (req.first) {
    VALTZ_ASSIGN(project::Asset fa, p->asset(*req.first));
    if (fa.kind != project::AssetKind::Image ||
        !m->has(models::Capability::ImageToVideo) ||
        !jget(vp, "first_frame", false)) {
      return make_error(Code::Unsupported, msg::kVideoStartsFromPicture);
    }
  }

  // Recorded as made: edges up to the model's grid, the length up to one
  // it makes (17n + 5 frames for H3), so the recipe says what ran.
  const Json defaults = jget(vp, "defaults", Json::object());
  const int align = std::max(16, jget(vp, "align", 16));
  auto up = [align](int v) {
    return std::max(align, (v + align - 1) / align * align);
  };
  const int width = up(req.width > 0 ? req.width
                                     : jget(defaults, "width", 960));
  const int height = up(req.height > 0 ? req.height
                                       : jget(defaults, "height", 544));
  const Json grid = jget(vp, "frame_grid", Json::object());
  const int step = std::max(1, jget(grid, "step", 1));
  const int offset = std::max(1, jget(grid, "offset", 1));
  int frames = req.frames > 0 ? req.frames : jget(defaults, "frames", 124);
  frames = std::min(frames, jget(vp, "max_frames", 241));
  frames = frames <= offset
               ? offset
               : offset + (frames - offset + step - 1) / step * step;
  const double fps = req.fps > 0 ? req.fps : jget(defaults, "fps", 24.0);
  // Favor's options: Custom's, else the preference's -- and a request
  // that asks for no Turbo has none.
  Json overrides = req.tuning.is_object() ? req.tuning : Json::object();
  if (!req.turbo && !overrides.contains("turbo")) {
    overrides["turbo"] = false;
  }
  // TaoMate's method makes a clip from words alone: one that opens on a
  // picture takes the preset's Turbo LoRA instead.
  if (req.first) {
    overrides["taomate"] = false;
  }
  VALTZ_TRY(check_preset_loras_(*m, req.preference, overrides));

  project::Recipe r;
  r.op = std::string(engine::kOpGenerateVideo);
  r.model = m->id;
  r.deterministic = false;
  r.params = {{"prompt", templated(*m, words)},
              {"seed", req.seed >= 0 ? req.seed : random_seed()},
              {"width", width},
              {"height", height},
              {"frames", frames},
              {"fps", fps}};
  stamp_origin(r, *m);
  // The adapter or branch is part of what made the clip: a rebuild
  // applies it too.
  apply_tuning_(r, *m, models::resolve_tuning(*m, tuning_context(*m),
                                              req.preference, false,
                                              overrides),
                req.steps);
  if (req.first) {
    // The picture it opens on is <Picture 1> to the model (H3's I2VA:
    // "<Picture 1> (from [Shot 1]) is fully referenced"): its mentions
    // and tags so named, any other dropped.
    std::string first = jget<std::string>(
        jget(m->prompting, "reference_tags", Json::object()), "image", "");
    if (const auto at = first.find("{n}"); at != std::string::npos) {
      first.replace(at, 3, "1");
      std::vector<std::string> tags;
      for (const auto& id : pf.inline_media) {
        tags.push_back(id && *id == *req.first ? first : std::string());
      }
      r.params["prompt"] = templated(*m, assist::tag_inline(pf.text, tags));
    }
    r.inputs.push_back({"first", *req.first, 0});
    // As for an edit's base: what the request says, over the picture as
    // it looks (rendered for the job, submit_build_).
    if (!req.first_adjust.identity()) {
      r.params["base_adjust"] = media::to_json(req.first_adjust);
    }
    if (!req.first_crop.identity()) {
      r.params["base_crop"] = media::to_json(req.first_crop);
    }
  }
  if (refs) {
    VALTZ_ASSIGN(Json media, video_references_(*p, req, *m, fps));
    r.params["reference_media"] = media["list"];
    // The order the model reads them in: the recipe's inputs.
    for (const auto& e : media["list"]) {
      auto id = AssetId::parse(jget<std::string>(e, "asset", ""));
      r.inputs.push_back({jget<std::string>(e, "role", "reference"), *id,
                          0});
    }
    if (req.reference_sound && media["sound"].is_string()) {
      r.params["reference_sound"] = true;
    }
    // Each mention and tag named as the model names its medium.
    std::vector<std::string> tags;
    for (const auto& id : pf.inline_media) {
      const std::string key = id ? id->str() : std::string();
      tags.push_back(jget<std::string>(media["tags"], key.c_str(), ""));
    }
    r.params["prompt"] = templated(*m, assist::tag_inline(pf.text, tags));
  }
  // Made from its prompt as well as its media: the prompt, captured.
  VALTZ_ASSIGN(project::RecipeInput pin,
               capture_prompt_(*p, req.project, pf.positional,
                               req.prompt_asset));
  r.inputs.push_back(pin);
  std::string name = req.name.empty()
                         ? one_line(assist::without_markdown(words))
                         : req.name;
  VALTZ_ASSIGN(project::Asset a,
               p->define_derived(std::move(name), project::AssetKind::Video,
                                 r, 48));
  post_("assets.changed", JobId{}, {{"project", req.project},
                                    {"asset", a.id},
                                    {"reason", "defined"}});
  return submit_build_(req.project, a.id, a.name);
}

Result<std::string>
Controller::prompt_outline(const std::string& model,
                           const std::vector<assist::RefKind>& row,
                           std::string_view language) const
{
  const models::ModelEntry* m = _catalog.find(model);
  if (!m) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model}});
  }
  return assist::prompt_outline(jget(m->prompting, "outline", Json()),
                                language.empty() ? _cfg.language : language,
                                row);
}

Result<Json>
Controller::video_reference_tags(ProjectId pid, const std::string& model,
                                 const std::vector<AssetId>& refs,
                                 std::optional<AssetId> continue_from)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  const models::ModelEntry* m = reference_model_(model);
  if (!m) {
    return make_error(Code::NotFound, msg::kNoReferenceModel);
  }
  GenerateVideoRequest req;
  req.references = refs;
  req.continue_from = continue_from;
  VALTZ_ASSIGN(Json media, video_references_(*p, req, *m, 24.0));
  return media["tags"];
}

const models::ModelEntry*
Controller::reference_model_(const std::string& model)
{
  if (!model.empty() && model != "auto") {
    return _catalog.find(model);
  }
  const models::ModelEntry* m = nullptr;
  for (const auto& a : auto_models()) {
    if (a.modality == "video" && a.op == "edit" && !a.chosen.empty()) {
      m = _catalog.find(a.chosen);
    }
  }
  return m;
}

Result<Json>
Controller::video_references_(project::Project& p,
                              const GenerateVideoRequest& req,
                              const models::ModelEntry& m, double fps)
{
  const Json vp = jget(m.engine, "vpipe", Json::object());
  const Json lim = jget(vp, "references", Json::object());
  // A reference read whole, by its file (vpipe's `references` list:
  // pictures, sounds), or a SPAN -- a clip, a trimmed sound, a clip's
  // tail -- cut exactly by vpipe's own stages into a reference port.
  // The list is read first, then the ports: that is the order the model
  // numbers them in, so it is the order recorded.
  Json listed = Json::array(), ported = Json::array();
  int pictures = 0, clips = 0, sounds = 0, ports = 0;
  // A reference is WHOLE: a span of something is a composition whose
  // layer marks it, drawn as it is when the job starts (DESIGN §6a).
  auto add = [&](AssetId id, const std::string& role) -> Status {
    VALTZ_ASSIGN(project::Asset a, p.asset(id));
    Json e = {{"asset", id.str()}, {"role", role}};
    switch (a.kind) {
    case project::AssetKind::Image:
      e["kind"] = "image";
      e["route"] = "list";
      ++pictures;
      listed.push_back(std::move(e));
      return ok_status();
    case project::AssetKind::Video: {
      const bool audio = sounds_(p, a, 0);
      e["kind"] = "video";
      e["route"] = "port";
      e["audio"] = audio;
      ++clips;
      sounds += audio ? 1 : 0;
      ports += audio ? 2 : 1;
      ported.push_back(std::move(e));
      return ok_status();
    }
    case project::AssetKind::Audio:
      e["kind"] = "audio";
      e["route"] = "list";
      ++sounds;
      listed.push_back(std::move(e));
      return ok_status();
    default:
      return make_error(Code::InvalidArgument, msg::kVideoReferenceKind,
                        {{"name", a.name}});
    }
  };
  // The clip continued first among the spans: <Video 1>.
  if (req.continue_from) {
    VALTZ_ASSIGN(project::Asset c, p.asset(*req.continue_from));
    if (c.kind != project::AssetKind::Video) {
      return make_error(Code::InvalidArgument, msg::kContinueNeedsClip);
    }
    VALTZ_TRY(add(*req.continue_from, "continue"));
    // Its tail: whole grid lengths (17n + 5 frames for H3: the encoder
    // keeps a clip's FIRST such frames, so any other count would lose the
    // moment it picks up from), at most what it has. A clip's is the
    // catalog's (or `tail_seconds`); a COMPOSITION -- a guide
    // (continuation_guide) or any timeline -- is read whole, as every
    // span is (DESIGN §6a).
    std::int64_t have = 0;
    double rate = fps;
    if (auto n = clip_frames_(p, c); n.ok()) {
      have = n->first;
      rate = n->second.num > 0 ? static_cast<double>(n->second.num) /
                                     n->second.den
                               : fps;
    }
    // In the model's frames.
    have = static_cast<std::int64_t>(
        std::floor(static_cast<double>(have) * fps / rate + 1e-6));
    std::int64_t want = jget(lim, "tail_frames", 90);
    if (c.cls == project::AssetClass::Composition) {
      want = have;
    } else if (req.tail_seconds > 0) {
      want = std::lround(req.tail_seconds * fps);
    }
    const Json grid = jget(vp, "frame_grid", Json::object());
    const std::int64_t step = std::max(1, jget(grid, "step", 1));
    const std::int64_t off = std::max(1, jget(grid, "offset", 1));
    std::int64_t tail = std::min<std::int64_t>(want, have > 0 ? have : want);
    tail = tail < off + step ? tail : off + (tail - off) / step * step;
    ported.back()["tail_frames"] = tail;
  }
  for (const auto& id : req.references) {
    if (req.continue_from && id == *req.continue_from) {
      continue;  // read once, as the clip continued
    }
    VALTZ_TRY(add(id, "reference"));
  }
  const int total = pictures + clips + sounds;
  if (pictures > jget(lim, "max_pictures", 9) ||
      clips > jget(lim, "max_clips", 3) ||
      sounds > jget(lim, "max_sounds", 3) ||
      total > jget(lim, "max_total", 12) || ports > jget(lim, "ports", 6)) {
    return make_error(Code::InvalidArgument, msg::kVideoTooManyReferences,
                      {{"pictures", std::to_string(jget(lim, "max_pictures",
                                                        9))},
                       {"clips", std::to_string(jget(lim, "max_clips", 3))},
                       {"sounds", std::to_string(jget(lim, "max_sounds", 3))},
                       {"total", std::to_string(jget(lim, "max_total", 12))}});
  }
  if (pictures + clips == 0) {
    return make_error(Code::InvalidArgument,
                      msg::kVideoReferencesNeedPicture);
  }
  // Each one's tag, counted within its kind in the order read; a clip
  // with its sound is its <Video k> and an <Audio j>.
  const Json fmt = jget(m.prompting, "reference_tags", Json::object());
  auto tag = [&](const std::string& kind, int n) {
    std::string t = jget<std::string>(fmt, kind.c_str(), "");
    if (const auto at = t.find("{n}"); at != std::string::npos) {
      t.replace(at, 3, std::to_string(n));
    }
    return t;
  };
  Json out = {{"list", Json::array()}, {"tags", Json::object()},
              {"sound", Json()}};
  int n_image = 0, n_video = 0, n_audio = 0;
  for (Json* group : {&listed, &ported}) {
    for (auto& e : *group) {
      const auto kind = jget<std::string>(e, "kind", "");
      const int n = kind == "image"   ? ++n_image
                    : kind == "video" ? ++n_video
                                      : ++n_audio;
      if (kind == "video" && jget(e, "audio", false)) {
        ++n_audio;
      }
      e["number"] = n;
      const auto id = jget<std::string>(e, "asset", "");
      if (!out["tags"].contains(id)) {
        out["tags"][id] = tag(kind, n);
      }
      // The first sound reference: what `reference_sound` keeps.
      if (kind == "audio" && out["sound"].is_null()) {
        out["sound"] = jget<std::string>(e, "asset", "");
      }
      out["list"].push_back(e);
    }
  }
  return out;
}

Result<JobId>
Controller::upscale_layer(UpscaleRequest req)
{
  auto undo = command_(req.project, "upscale", req.composition, req.layer);
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset comp, p->asset(req.composition));
  if (comp.layers.empty()) {
    return make_error(Code::InvalidArgument, msg::kNotComposition,
                      {{"name", comp.name}});
  }
  const project::Layer* layer = nullptr;
  for (const auto& l : comp.layers) {
    if (l.id == req.layer) {
      layer = &l;
    }
  }
  if (!layer || !layer->source) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  VALTZ_ASSIGN(project::Asset src, p->asset(*layer->source));
  // A composition is many things drawn: flat first (flatten_layer).
  if (src.cls == project::AssetClass::Composition ||
      src.cls == project::AssetClass::Still ||
      src.cls == project::AssetClass::Markup) {
    return make_error(Code::InvalidArgument, msg::kUpscaleFlattenFirst,
                      {{"name", src.name}});
  }
  const bool clip = src.kind == project::AssetKind::Video;
  if (!clip && src.kind != project::AssetKind::Image) {
    return make_error(Code::Unsupported, msg::kUpscaleKind);
  }
  const auto need = clip ? models::Capability::UpscaleVideo
                         : models::Capability::UpscaleImage;
  VALTZ_ASSIGN(project::AssetVersion v, p->version(src.id));
  // One size: the crop's one key (or none: nothing to upscale).
  const media::KeyedCrop keys = crop_keys_of(comp, req.layer);
  if (keys.place.keys.size() > 1) {
    return make_error(Code::InvalidArgument, msg::kUpscaleKeyed,
                      {{"name", src.name}});
  }
  const media::Crop c = keys.place.at(0);
  const media::PixelSize from{v.info.frame.width, v.info.frame.height};
  const double sx = std::max(1.0, c.scale_x);
  const double sy = std::max(1.0, c.scale_y);
  if ((sx <= 1.0 && sy <= 1.0) || from.width <= 0 || from.height <= 0) {
    return make_error(Code::InvalidArgument, msg::kUpscaleNotScaled,
                      {{"name", src.name}});
  }
  // Even sides, as the writer takes them.
  auto even = [](double x) {
    return std::max(2, static_cast<int>(std::lround(x / 2)) * 2);
  };
  const media::PixelSize to{even(from.width * sx), even(from.height * sy)};
  // The upscaler: the one named, else the first installed that runs.
  const models::ModelEntry* m = nullptr;
  for (const auto& e : _catalog.models()) {
    if ((req.model.empty() || req.model == "auto" || req.model == e.id) &&
        e.has(need) &&
        _store->info(e).state == models::InstallState::Installed &&
        engine_runs_(need)) {
      m = &e;
      break;
    }
  }
  if (!m) {
    return make_error(Code::NotFound, msg::kNoUpscaler);
  }
  // Its span: the layer's marks (at their own rate), else the whole clip.
  const Rational own = v.info.frame_rate;
  const Rational marks = layer->time.rate;
  auto in_source = [&](std::int64_t f) {
    if (f < 0 || marks.num <= 0 || own.num <= 0 || marks == own) {
      return f;
    }
    return static_cast<std::int64_t>(std::llround(
        f * own.to_double() / marks.to_double()));
  };
  const std::int64_t first =
      std::max<std::int64_t>(0, in_source(layer->time.in));
  const std::int64_t out = in_source(layer->time.out);
  const std::int64_t last = out >= first ? out : v.info.frame_count - 1;
  const std::int64_t count = std::max<std::int64_t>(1, last - first + 1);
  const Json up = jget(jget(m->engine, "vpipe", Json::object()), "upscale",
                       Json::object());
  project::Recipe r;
  r.model = m->id;
  r.deterministic = false;
  if (clip) {
    r.op = std::string(engine::kOpUpscaleVideo);
    r.params = {{"width", to.width},
                {"height", to.height},
                {"first", first},
                {"count", count},
                {"seed", req.seed},
                {"group", jget(up, "group", 25)},
                {"overlap", jget(up, "overlap", 4)}};
  } else {
    r.op = std::string(engine::kOpUpscaleImage);
    r.params = {{"width", to.width},
                {"height", to.height},
                {"seed", req.seed}};
    // Its eyes (VOSR's DINOv2), by catalog id: resolved at submit.
    if (const auto enc = jget<std::string>(up, "encoder", "");
        !enc.empty()) {
      r.params["encoder"] = enc;
    }
  }
  r.inputs.push_back({"source", src.id, v.number});
  stamp_origin(r, *m);
  VALTZ_ASSIGN(project::Asset a, p->define_derived(
      std::format("{} {}×{}", src.name, to.width, to.height),
      clip ? project::AssetKind::Video : project::AssetKind::Image, r,
      96));
  {
    std::lock_guard lk(_upscale_mu);
    _upscale_targets[a.id] = {req.composition, req.layer, from, to};
  }
  post_("assets.changed", JobId{}, {{"project", req.project},
                                    {"asset", a.id},
                                    {"reason", "defined"}});
  auto job = submit_build_(req.project, a.id, a.name);
  if (!job.ok()) {
    std::lock_guard lk(_upscale_mu);
    _upscale_targets.erase(a.id);
  }
  return job;
}

Result<AssetId>
Controller::flatten_layer(ProjectId pid, AssetId comp_id,
                          const std::string& layer_id)
{
  auto undo = command_(pid, "layer.flatten", comp_id, layer_id);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset comp, p->asset(comp_id));
  const auto it = std::ranges::find(comp.layers, layer_id,
                                    &project::Layer::id);
  if (it == comp.layers.end() || !it->source) {
    return make_error(Code::InvalidArgument, "no such layer");
  }
  const project::LayerTime time = it->time;
  VALTZ_ASSIGN(project::Asset src, p->asset(*it->source));
  const bool pages = src.cls == project::AssetClass::Still && src.pages > 1;
  // A still with pages: the page the layer shows (its mark-in), alone.
  Result<AssetId> made =
      pages ? flat_page_(pid, src, std::max<std::int64_t>(0, time.in),
                         std::format("{}, page {} (flat)", src.name,
                                     std::max<std::int64_t>(0, time.in) +
                                         1))
            : flatten_asset(pid, src.id, std::format("{} (flat)", src.name));
  VALTZ_ASSIGN(AssetId flat, std::move(made));
  // The same picture or clip, drawn: the same size and length, so the
  // layer's crop and marks mean what they meant.
  VALTZ_TRY(set_layer_source(pid, comp_id, layer_id, flat));
  if (!pages && (time.in >= 0 || time.out >= 0)) {
    VALTZ_TRY(set_layer_time(pid, comp_id, layer_id, time));
  }
  return flat;
}

void
Controller::apply_upscale_(ProjectId pid, AssetId made, JobId job)
{
  UpscaleTarget t;
  {
    std::lock_guard lk(_upscale_mu);
    auto it = _upscale_targets.find(made);
    if (it == _upscale_targets.end()) {
      return;
    }
    t = it->second;
    _upscale_targets.erase(it);
  }
  // In the job's own command: undone with the result, as one.
  auto undo = join_(pid, job);
  project::Project* p = project(pid);
  if (!p) {
    return;
  }
  auto comp = p->asset(t.composition);
  if (!comp.ok()) {
    return;
  }
  // The crop taken before the source changes: the same frame, the content
  // larger, its scale smaller by as much.
  media::KeyedCrop keys = crop_keys_of(*comp, t.layer);
  for (auto& k : keys.place.keys) {
    k.value.content = t.to;
    k.value.scale_x *= static_cast<double>(t.from.width) / t.to.width;
    k.value.scale_y *= static_cast<double>(t.from.height) / t.to.height;
  }
  if (auto st = set_layer_source(pid, t.composition, t.layer, made);
      !st.ok()) {
    VALTZ_LOG_WARN("upscale", "the layer kept its clip: {}",
                   st.error().message);
    return;
  }
  if (!keys.place.keys.empty()) {
    if (auto st = set_crop_keys(pid, t.composition, keys, t.layer);
        !st.ok()) {
      VALTZ_LOG_WARN("upscale", "the layer's scale stayed: {}",
                     st.error().message);
    }
  }
}

Result<JobId>
Controller::generate_audio(GenerateAudioRequest req)
{
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  // A song reads the words: inline pictures are dropped. The prompt is
  // its style and, under section headers, its lyrics.
  VALTZ_ASSIGN(PromptForm pf, prompt_form_(*p, req.prompt, {}, req.row,
                                           true));
  const std::string words = assist::prompt_words(pf.positional);
  // The voice to speak in: given, else the row's first sound -- which
  // makes Auto's pick its audio `edit` list's (a model that speaks).
  if (!req.voice) {
    for (const auto& id : req.row) {
      if (voiced_(*p, id)) {
        req.voice = id;
        break;
      }
    }
  }
  const models::ModelEntry* m = nullptr;
  if (!req.model.empty() && req.model != "auto") {
    m = _catalog.find(req.model);
    if (!m) {
      return make_error(Code::NotFound, msg::kUnknownModel,
                        {{"model", req.model}});
    }
  } else {
    const char* op = req.voice ? "edit" : "generate";
    for (const auto& a : auto_models()) {
      if (a.modality == "audio" && a.op == op && !a.chosen.empty()) {
        m = _catalog.find(a.chosen);
        break;
      }
    }
  }
  // Its command: what it makes, known now (everything above only read).
  const bool speech = m && m->has(models::Capability::TextToSpeech);
  auto undo = command_(req.project,
                       speech ? "generate.speech" : "generate.audio");
  if (speech) {
    // The file its weights are held at: Fast and Med its 8-bit pack.
    m = &weights_variant_(*m, models::resolve_tuning(
                                  *m, tuning_context(*m), req.preference,
                                  false, req.tuning));
    if (!runs(*m, "audio", "generate")) {
      return make_error(Code::NotFound, msg::kNoSpeechModel);
    }
    VALTZ_ASSIGN(project::RecipeInput pin,
                 capture_prompt_(*p, req.project, pf.positional,
                                 req.prompt_asset));
    return generate_speech_(req, *m, *p, words, pin);
  }
  assist::SongText song = assist::split_song(words);
  if (!req.lyrics.empty()) {
    // Lyrics given apart -- all of them sung, headers or not -- replace
    // any the prompt holds.
    song.lyrics = assist::tag_pictures(req.lyrics, {}, "");
  }
  if (song.style.empty() && song.lyrics.empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  if (req.plan != "full" && req.plan != "melody" && req.plan != "off") {
    return make_error(Code::InvalidArgument, std::format(
        "plan '{}' is not full, melody or off", req.plan));
  }
  if (!req.score.empty() && req.plan == "off") {
    return make_error(Code::InvalidArgument,
                      "a score to follow needs a plan: full or melody");
  }
  if (!m || !m->has(models::Capability::TextToAudio)) {
    return make_error(Code::NotFound, msg::kNoAudioModel);
  }
  const Json vp = jget(m->engine, "vpipe", Json::object());
  const auto* decoder = _catalog.find(jget<std::string>(vp, "decoder", ""));
  if (decoder &&
      _store->info(*decoder).state != models::InstallState::Installed) {
    return make_error(Code::NotFound, msg::kModelNotInstalled,
                      {{"model", decoder->name}});
  }
  if (!runs(*m, "audio", "generate")) {
    return make_error(Code::NotFound, msg::kNoAudioModel);
  }
  // A cap the model would pass anyway is no cap.
  const double most = jget(vp, "max_seconds", 360.0);
  const double cap = req.max_seconds > 0 && req.max_seconds < most
                         ? req.max_seconds
                         : 0.0;

  project::Recipe r;
  r.op = std::string(engine::kOpGenerateAudio);
  r.model = m->id;
  r.deterministic = false;
  r.params = {{"prompt", words},
              {"style", song.style},
              {"lyrics", song.lyrics},
              {"cot", req.plan},
              {"seed", req.seed >= 0 ? req.seed : random_seed()}};
  stamp_origin(r, *m);
  if (cap > 0) {
    r.params["max_seconds"] = cap;
  }
  if (!req.score.empty()) {
    r.params["abc"] = req.score;
  }
  // The decoder is part of what made it: a rebuild decodes with it too.
  if (decoder) {
    r.params["decoder"] = decoder->id;
  }
  // The flow matching's steps and tiers (Favor; models/tuning.h).
  apply_tuning_(r, *m, models::resolve_tuning(*m, tuning_context(*m),
                                              req.preference, false,
                                              req.tuning),
                req.steps);
  // Made from its prompt: the prompt, captured.
  VALTZ_ASSIGN(project::RecipeInput pin,
               capture_prompt_(*p, req.project, pf.positional,
                               req.prompt_asset));
  r.inputs.push_back(pin);
  // Named after its style, else its first sung line.
  std::string name = req.name;
  if (name.empty()) {
    name = song.style;
    if (name.empty()) {
      for (std::size_t at = 0; at < song.lyrics.size();) {
        auto end = song.lyrics.find('\n', at);
        if (end == std::string::npos) {
          end = song.lyrics.size();
        }
        const std::string line = song.lyrics.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.front() != '[') {
          name = line;
          break;
        }
      }
    }
    name = one_line(assist::without_markdown(name));
  }
  VALTZ_ASSIGN(project::Asset a,
               p->define_derived(std::move(name), project::AssetKind::Audio,
                                 r, 48));
  post_("assets.changed", JobId{}, {{"project", req.project},
                                    {"asset", a.id},
                                    {"reason", "defined"}});
  return submit_build_(req.project, a.id, a.name);
}

bool
Controller::voiced_(project::Project& p, AssetId id) const
{
  auto a = p.asset(id);
  if (!a.ok()) {
    return false;
  }
  if (a->kind == project::AssetKind::Audio) {
    return true;
  }
  if (a->kind != project::AssetKind::Video) {
    return false;
  }
  if (a->cls == project::AssetClass::Flat ||
      a->cls == project::AssetClass::Generated) {
    auto v = p.version(id);
    return v.ok() && v->info.has_audio;
  }
  return false;  // a composition's clip: its mix is its sound layers'
}

Result<JobId>
Controller::generate_speech_(GenerateAudioRequest& req,
                             const models::ModelEntry& m,
                             project::Project& p, const std::string& words,
                             const project::RecipeInput& prompt)
{
  // Its fields, then its words (DESIGN §4g); the language named when the
  // prompt does not -- the model speaks markedly better told it.
  assist::SpeechText sp = assist::split_speech(words);
  if (assist::prompt_words(sp.text).empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  if (sp.language.empty()) {
    sp.language = assist::speech_language(sp.text);
  }
  const Json vp = jget(m.engine, "vpipe", Json::object());
  const Json spc = jget(vp, "speech", Json::object());
  const double tps = jget(spc, "tokens_per_second", 12.5);
  const double most = jget(spc, "max_seconds", 300.0);
  const double seconds = std::clamp(req.seconds, 0.0, most);

  project::Recipe r;
  r.op = std::string(engine::kOpGenerateSpeech);
  r.model = m.id;
  r.deterministic = false;
  r.params = {{"prompt", words},
              {"text", sp.text},
              {"instruction", sp.instruction},
              {"quality", sp.quality},
              {"sound_event", sp.sound_event},
              {"ambient_sound", sp.ambient_sound},
              {"language", sp.language},
              {"duration_tokens",
               seconds > 0 ? static_cast<int>(std::lround(seconds * tps))
                           : 0},
              {"seed", req.seed >= 0 ? req.seed : random_seed()}};
  stamp_origin(r, m);
  // The codec is part of what made it: a rebuild decodes with it too.
  if (const auto* codec = _catalog.find(jget<std::string>(vp, "decoder",
                                                          ""))) {
    r.params["decoder"] = codec->id;
  }
  // Favor: the weights held at 8 bits, the int8 GEMMs (models/tuning.h:
  // Fast and Med on, Fine off) -- what the graph builder lays on.
  r.params["tuning"] = models::resolve_tuning(m, tuning_context(m),
                                              req.preference, false,
                                              req.tuning);
  r.inputs.push_back(prompt);
  if (req.voice) {
    VALTZ_ASSIGN(project::Asset v, p.asset(*req.voice));
    if (!voiced_(p, *req.voice)) {
      return make_error(Code::InvalidArgument, msg::kVoiceKind,
                        {{"name", v.name}});
    }
    r.inputs.push_back({"voice", *req.voice, 0});
  }
  // Named after its words.
  std::string name = req.name;
  if (name.empty()) {
    name = one_line(assist::without_markdown(sp.text));
    name = std::string(utf8_prefix(name, 80));
  }
  VALTZ_ASSIGN(project::Asset a,
               p.define_derived(std::move(name), project::AssetKind::Audio,
                                r, 48));
  post_("assets.changed", JobId{}, {{"project", req.project},
                                    {"asset", a.id},
                                    {"reason", "defined"}});
  return submit_build_(req.project, a.id, a.name);
}

bool
Controller::can_edit_(const models::ModelEntry& m) const
{
  // The catalog says the model edits; the engine must also have an edit
  // graph, and the family must say how that graph takes references.
  const Json vp = jget(m.engine, "vpipe", Json::object());
  return m.has(models::Capability::ImageEdit) &&
         jget(vp, "edit", Json()).is_object() &&
         _engine->supports(engine::kOpEditImage);
}

const models::ModelEntry&
Controller::weights_variant_(const models::ModelEntry& m,
                             const Json& tuning) const
{
  if (!tuning.is_object() || !tuning.contains("w8_weights")) {
    return m;
  }
  const models::ModelEntry* src =
      m.quantize_from.empty() ? &m : _catalog.find(m.quantize_from);
  if (!src) {
    return m;
  }
  auto installed = [&](const models::ModelEntry& e) {
    return _store->info(e).state == models::InstallState::Installed;
  };
  const models::ModelEntry* pack = nullptr;
  for (const auto& e : _catalog.models()) {
    if (e.quantize_from == src->id && e.quantize_bits == 8 &&
        installed(e)) {
      pack = &e;
      break;
    }
  }
  if (jget(tuning, "w8_weights", true) && pack) {
    return *pack;
  }
  if (installed(*src)) {
    return *src;
  }
  return pack ? *pack : m;
}

Result<JobId>
Controller::build_asset(ProjectId pid, AssetId aid)
{
  auto undo = command_(pid, "build", aid);
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  return submit_build_(pid, aid, a.name);
}

Result<JobId>
Controller::submit_build_(ProjectId pid, AssetId aid, std::string title)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(aid));
  if (a.origin != project::Origin::Derived) {
    return make_error(Code::InvalidArgument, msg::kSourceNotBuilt);
  }
  VALTZ_ASSIGN(project::Recipe r, p->recipe(a.recipe));
  VALTZ_ASSIGN(auto inputs, p->resolve_inputs(r));
  const auto* m = _catalog.find(r.model);
  if (!m) {
    return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                      {{"model", r.model}});
  }
  if (!_engine->supports(r.op)) {
    return make_error(Code::Unsupported, msg::kEngineCannotRun,
                      {{"operation", r.op}});
  }
  VALTZ_ASSIGN(engine::ModelRef ref, resolve_model_(*m, true));
  // A run-time adapter the recipe applies (the Turbo LoRA), by its file.
  if (const auto lora = jget<std::string>(r.params, "lora", "");
      !lora.empty()) {
    const auto* le = _catalog.find(lora);
    if (!le) {
      return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                        {{"model", lora}});
    }
    const auto li = _store->info(*le);
    if (li.state != models::InstallState::Installed) {
      return make_error(Code::NotFound, msg::kModelNotInstalled,
                        {{"model", le->name}});
    }
    ref.lora = li.path();
  }
  // LoRAs by their files, in the two slots.
  for (const auto& [key, slot] :
       {std::pair{"lora_file", &ref.lora}, std::pair{"lora2_file",
                                                     &ref.lora2}}) {
    const auto f = jget<std::string>(r.params, key, "");
    if (f.empty()) {
      continue;
    }
    std::error_code ec;
    if (!fs::is_regular_file(f, ec)) {
      return make_error(Code::NotFound, msg::kLoraMissing, {{"file", f}});
    }
    *slot = f;
  }
  // A generator's own decoder (YuE2's VAE), by its folder -- unless a
  // checkpoint of one's own takes its place (below).
  if (const auto dec = jget<std::string>(r.params, "decoder", "");
      !dec.empty()) {
    const auto* de = _catalog.find(dec);
    if (!de) {
      return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                        {{"model", dec}});
    }
    const auto di = _store->info(*de);
    if (di.state != models::InstallState::Installed) {
      return make_error(Code::NotFound, msg::kModelNotInstalled,
                        {{"model", de->name}});
    }
    ref.vae = di.path();
  }
  // A vision tower its conditioner reads (VOSR's DINOv2), by its folder.
  if (const auto enc = jget<std::string>(r.params, "encoder", "");
      !enc.empty()) {
    const auto* ee = _catalog.find(enc);
    if (!ee) {
      return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                        {{"model", enc}});
    }
    const auto ei = _store->info(*ee);
    if (ei.state != models::InstallState::Installed) {
      return make_error(Code::NotFound, msg::kModelNotInstalled,
                        {{"model", ee->name}});
    }
    ref.encoder = ei.path();
  }
  // Community checkpoints for its DiT and VAE, files or folders.
  for (const auto& [key, slot] :
       {std::pair{"dit_file", &ref.dit}, std::pair{"vae_file", &ref.vae}}) {
    const auto f = jget<std::string>(r.params, key, "");
    if (f.empty()) {
      continue;
    }
    std::error_code ec;
    if (!fs::exists(f, ec)) {
      return make_error(Code::NotFound, msg::kWeightsMissing, {{"file", f}});
    }
    *slot = f;
  }
  // A second checkpoint beside the model (H3's VDN branch), by its
  // folder.
  if (const auto branch = jget<std::string>(r.params, "branch", "");
      !branch.empty()) {
    const auto* be = _catalog.find(branch);
    if (!be) {
      return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                        {{"model", branch}});
    }
    const auto bi = _store->info(*be);
    if (bi.state != models::InstallState::Installed) {
      return make_error(Code::NotFound, msg::kModelNotInstalled,
                        {{"model", be->name}});
    }
    ref.branch = bi.path();
  }

  engine::JobSpec spec;
  spec.id = JobId::make();
  spec.op = r.op;
  spec.model = ref;
  spec.params = r.params;
  spec.output_dir = p->blobs().tmp_dir();
  // What its commit joins: the command it is submitted in.
  if (project::Workspace* w = workspace_(pid)) {
    if (const auto seq = w->undo().recording()) {
      std::lock_guard lk(_job_commands_mu);
      _job_commands[spec.id] = seq;
    }
  }
  project::BlobRef rendered_base;
  for (const auto& in : inputs) {
    VALTZ_ASSIGN(project::Asset ia, p->asset(in.asset));
    const bool picture_in = in.role == "base" || in.role == "reference" ||
                            in.role == "first";
    const bool drawn = ia.cls == project::AssetClass::Still ||
                       ia.cls == project::AssetClass::Composition ||
                       ia.cls == project::AssetClass::Markup;
    if (drawn) {
      // A composition or a markup goes in AS IT IS DRAWN -- a picture, a
      // movie with its mix, a mix -- flattened only now, and kept in the
      // managed cache by what it draws: the next generation from it
      // finds it made (DESIGN §6a).
      engine::JobInput ji;
      ji.role = in.role;
      VALTZ_ASSIGN(ji.path, rendered_(pid, in.asset, 0, 0));
      VALTZ_ASSIGN(ji.info, media::probe_file(ji.path));
      VALTZ_ASSIGN(ji.hash, hash_file(ji.path));
      VALTZ_LOG_INFO("build", "{} ({}) drawn for the model", ia.name,
                     in.role);
      if (picture_in && in.role != "reference" &&
          ia.kind == project::AssetKind::Image && rendered_base.empty()) {
        if (auto b = p->blobs().ingest_file(ji.path); b.ok()) {
          rendered_base = *b;
        }
      }
      spec.inputs.push_back(std::move(ji));
      continue;
    }
    VALTZ_ASSIGN(fs::path path, p->media_path(in.asset, in.version));
    VALTZ_ASSIGN(project::AssetVersion v, p->version(in.asset, in.version));
    engine::JobInput ji{in.role, path, in.hash, v.info};
    if (picture_in && ia.kind == project::AssetKind::Image) {
      // A clip's reference picture is read by vpipe itself, which takes
      // a PNG, a JPEG, a TIFF as they are: a RAW or a HEIC is drawn
      // first, as it shows.
      std::string ext = path.extension().string();
      std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      const bool unread = r.op == engine::kOpGenerateVideo &&
                          ext != ".png" && ext != ".jpg" &&
                          ext != ".jpeg" && ext != ".tif" &&
                          ext != ".tiff" && ext != ".webp";
      if (unread) {
        const fs::path flat =
            p->blobs().tmp_dir() /
            std::format("{}-in{}.png", spec.id.str(), spec.inputs.size());
        VALTZ_TRY(flatten(pid, in.asset, flat));
        VALTZ_ASSIGN(ji.info, media::probe_file(flat));
        VALTZ_ASSIGN(ji.hash, hash_file(flat));
        ji.path = flat;
        if (in.role != "reference" && rendered_base.empty()) {
          if (auto b = p->blobs().ingest_file(flat); b.ok()) {
            rendered_base = *b;
          }
        }
      }
    }
    spec.inputs.push_back(std::move(ji));
  }

  // Its phase clock, with how the last of its kind went.
  {
    const double frames = std::max(1, jget(r.params, "frames", 1));
    const double volume = jget(r.params, "width", 0.0) *
                          jget(r.params, "height", 0.0) * frames;
    Json prior = timing_prior_(*p, r);
    std::lock_guard lk(_timing_mu);
    _timing.insert_or_assign(spec.id, JobTiming(steady_seconds(),
                                                std::move(prior), volume,
                                                frames));
  }

  JobRecord rec;
  rec.id = spec.id;
  rec.op = r.op;
  rec.purpose = "build";
  rec.title = std::move(title);
  rec.project = pid;
  rec.asset = aid;
  rec.recipe = r;
  rec.inputs = inputs;
  rec.rendered_base = rendered_base;
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  post_("job.queued", rec.id, {{"title", rec.title}, {"op", rec.op},
                               {"purpose", rec.purpose},
                               {"project", pid}, {"asset", aid}});

  VALTZ_TRY(_engine->submit(std::move(spec),
                            [this](const engine::JobEvent& ev) {
    on_build_event_(ev);
  }));
  return rec.id;
}

Json
Controller::timing_prior_(project::Project& p, const project::Recipe& r)
{
  const std::string key = r.op + "|" + r.model;
  {
    std::lock_guard lk(_timing_mu);
    if (const auto it = _priors.find(key); it != _priors.end()) {
      return it->second;
    }
  }
  // The project's newest result of the kind that kept its timing.
  auto assets = p.assets();
  if (!assets.ok()) {
    return Json();
  }
  std::vector<project::Asset> made;
  for (const auto& a : *assets) {
    if (a.origin == project::Origin::Derived && a.head > 0) {
      made.push_back(a);
    }
  }
  std::sort(made.begin(), made.end(), [](const auto& x, const auto& y) {
    return x.modified_ms > y.modified_ms;
  });
  for (const auto& a : made) {
    auto ar = p.recipe(a.recipe);
    if (!ar.ok() || ar->op != r.op || ar->model != r.model) {
      continue;
    }
    auto v = p.version(a.id);
    if (!v.ok() || jget(v->timing, "phases", Json()).is_null()) {
      continue;
    }
    const double frames = std::max(1, jget(v->executed, "frames", 1));
    Json prior = v->timing;
    prior["frames"] = frames;
    prior["volume"] = jget(v->executed, "width", 0.0) *
                      jget(v->executed, "height", 0.0) * frames;
    return prior;
  }
  return Json();
}

void
Controller::on_build_event_(const engine::JobEvent& ev)
{
  switch (ev.kind) {
  case engine::JobEventKind::Started:
    _jobs.update(ev.job, [](JobRecord& r) { r.state = JobState::Running; });
    {
      std::lock_guard lk(_timing_mu);
      if (auto it = _timing.find(ev.job); it != _timing.end()) {
        it->second.start(steady_seconds());
      }
    }
    post_("job.started", ev.job, {{"engine", ev.text}});
    break;
  case engine::JobEventKind::Progress:
  case engine::JobEventKind::Preview: {
    _jobs.update(ev.job, [&](JobRecord& r) {
      if (ev.progress >= 0) {
        r.progress = ev.progress;
      }
    });
    Json d = {{"step", ev.step}, {"steps", ev.steps},
              {"progress", ev.progress}};
    if (ev.kind == engine::JobEventKind::Preview) {
      // A clip's frame count and rate; an export's frame and phase.
      for (const char* k : {"frames", "fps", "frame", "phase"}) {
        if (ev.data.is_object() && ev.data.contains(k)) {
          d[k] = ev.data[k];
        }
      }
      post_("job.preview", ev.job, d, ev.tensor);
    } else {
      // The phase and its count (engine.h, kPhase*), and the seconds
      // left once there is an estimate (job-timing.h).
      if (ev.data.is_object()) {
        d.update(ev.data);
      }
      std::lock_guard lk(_timing_mu);
      if (auto it = _timing.find(ev.job); it != _timing.end()) {
        const double now = steady_seconds();
        it->second.note(d, now);
        if (const auto left = it->second.left(now)) {
          d["left"] = *left;
        }
      }
      post_("job.progress", ev.job, d);
    }
    break;
  }
  case engine::JobEventKind::Output: {
    auto rec = _jobs.get(ev.job);
    project::Project* p = rec ? project(rec->project) : nullptr;
    if (!p) {
      VALTZ_LOG_WARN("build", "output for a closed project dropped");
      break;
    }
    if (withdrawn_(ev.job)) {
      // Its request was undone while it ran: nothing to commit it to.
      VALTZ_LOG_INFO("build", "job {}: withdrawn, its result dropped",
                     ev.job.str());
      break;
    }
    project::Project::BuildRecord b;
    b.recipe = rec->recipe;
    b.inputs = rec->inputs;
    b.output = ev.output;
    b.info = ev.output_info;
    b.engine = engine_description();
    b.host = _host;
    b.model_digest = rec->model_digest;
    // What it made beside its file: a song's score.
    if (const auto score = jget<std::string>(ev.data, "score", "");
        !score.empty()) {
      b.outputs["score"] = score;
    }
    {
      // How long it took; the prior of the next of its kind.
      std::lock_guard lk(_timing_mu);
      if (auto it = _timing.find(ev.job); it != _timing.end()) {
        const double now = steady_seconds();
        b.timing = it->second.record(now);
        _priors[rec->recipe.op + "|" + rec->recipe.model] =
            it->second.prior(now);
      }
    }
    // Into the command it was submitted in (its place in the history).
    auto undo = join_(rec->project, ev.job);
    auto v = p->commit_build(rec->asset, std::move(b));
    if (!v.ok()) {
      VALTZ_LOG_ERROR("build", "could not store the result: {}",
                      v.error().message);
      break;
    }
    record_history_(*p, *rec, *v);
    _jobs.update(ev.job, [&](JobRecord& r) {
      r.message = std::format("version {}", v->number);
    });
    post_("assets.changed", ev.job, {{"project", rec->project},
                                     {"asset", rec->asset},
                                     {"version", v->number},
                                     {"reason", "built"}});
    // An upscale: its layer shows it now.
    apply_upscale_(rec->project, rec->asset, ev.job);
    break;
  }
  case engine::JobEventKind::Finished: {
    auto rec = _jobs.get(ev.job);
    if (withdrawn_(ev.job)) {
      // Done before the cancel reached it: as far as anyone asked, it
      // was cancelled.
      finish_job_(ev.job, JobState::Cancelled, "");
      post_("job.cancelled", ev.job);
      break;
    }
    finish_job_(ev.job, JobState::Finished, rec ? rec->message : "");
    Json d = Json::object();
    if (rec) {
      d = {{"project", rec->project}, {"asset", rec->asset}};
    }
    post_("job.finished", ev.job, d);
    break;
  }
  case engine::JobEventKind::Failed: {
    finish_job_(ev.job, JobState::Failed, ev.text);
    if (auto rec = _jobs.get(ev.job)) {
      std::lock_guard lk(_upscale_mu);
      _upscale_targets.erase(rec->asset);
    }
    Json d = {{"code", to_str(ev.error)}, {"message", ev.text}};
    // Refused for memory: what vpipe asked for and had, and what to
    // change in the request.
    if (const Json m = jget(ev.data, "memory", Json());
        ev.error == Code::OutOfMemory && m.is_object()) {
      auto rec = _jobs.get(ev.job);
      d.update(out_of_memory_report(m, rec ? &rec->recipe : nullptr));
    }
    post_("job.failed", ev.job, d);
    break;
  }
  case engine::JobEventKind::Cancelled:
    finish_job_(ev.job, JobState::Cancelled, "");
    if (auto rec = _jobs.get(ev.job)) {
      std::lock_guard lk(_upscale_mu);
      _upscale_targets.erase(rec->asset);
    }
    post_("job.cancelled", ev.job);
    break;
  case engine::JobEventKind::Text:
    break;
  }
}

namespace {

// A still the app and the history read as it is: no development, no
// decoder of Valtz's own in between.
bool
plain_still(std::string_view ext)
{
  std::string e(ext);
  std::ranges::transform(e, e.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return e == "png" || e == "jpg" || e == "jpeg" || e == "tif" ||
         e == "tiff" || e == "heic" || e == "webp";
}

}

void
Controller::record_history_(project::Project& p, const JobRecord& rec,
                            const project::AssetVersion& made)
{
  const auto op = rec.recipe.op;
  const bool clip = op == engine::kOpGenerateVideo;
  // A song or speech: a sound made, a state as a song is.
  const bool song = op == engine::kOpGenerateAudio ||
                    op == engine::kOpGenerateSpeech;
  if (op != engine::kOpGenerateImage && op != engine::kOpEditImage &&
      !clip && !song) {
    return;
  }
  auto known = p.history();
  if (!known.ok()) {
    VALTZ_LOG_WARN("history", "not read: {}", known.error().message);
    return;
  }
  auto seen = [&](const ContentHash& h) {
    return std::ranges::any_of(*known, [&](const auto& e) {
      return e.picture.hash == h;
    });
  };
  auto name_of = [&](AssetId id) {
    auto a = p.asset(id);
    return a.ok() ? a->name : std::string();
  };
  std::vector<project::HistoryEntry> add;

  // The base, as the model got it: the file it was given, adjusted and
  // cropped as the recipe says -- what Start sent, whatever has been
  // done to the asset since. A plain file of the asset is that file. A
  // clip's is the picture it opens on.
  auto base = std::ranges::find(rec.inputs,
                                std::string(clip ? "first" : "base"),
                                &project::ResolvedInput::role);
  if (op != engine::kOpGenerateImage && !song &&
      base != rec.inputs.end()) {
    auto bv = p.version(base->asset, base->version);
    auto path = p.media_path(base->asset, base->version);
    const auto adj = media::adjustments_from_json(
        jget(rec.recipe.params, "base_adjust", Json::object()));
    const auto crop = media::crop_from_json(
        jget(rec.recipe.params, "base_crop", Json::object()));
    if (bv.ok() && path.ok()) {
      project::HistoryEntry e;
      e.id = HistoryId::make();
      e.created_ms = rec.created_ms;
      e.role = "base";
      e.asset = base->asset;
      e.version = base->version;
      e.label = name_of(base->asset);
      if (!rec.rendered_base.empty() && adj.identity() &&
          crop.identity()) {
        // Rendered for the job (a look of its own): that picture.
        e.picture = rec.rendered_base;
        e.rendered = true;
        if (auto sz = media::oriented_size(
                p.blobs().path_of(rec.rendered_base));
            sz.ok()) {
          e.width = sz->width;
          e.height = sz->height;
        }
      } else if (adj.identity() && crop.identity() && !bv->blob.empty() &&
                 plain_still(bv->blob.ext)) {
        e.picture = bv->blob;
        e.width = bv->info.frame.width;
        e.height = bv->info.frame.height;
      } else {
        // Adjusted, cropped, a camera RAW, a linked original: drawn as
        // the model got it, into a picture of the project's own.
        const auto tmp = p.blobs().make_tmp_path("png");
        media::LayerPicture pic;
        pic.file = *path;
        pic.adjust = adj;
        pic.crop = crop;
        auto st = media::flatten_layers({pic}, tmp);
        auto blob = st.ok() ? p.blobs().adopt_file(tmp, "png")
                            : Result<project::BlobRef>(st.error());
        if (blob.ok()) {
          e.picture = *blob;
          e.rendered = true;
          if (auto sz = media::oriented_size(p.blobs().path_of(*blob));
              sz.ok()) {
            e.width = sz->width;
            e.height = sz->height;
          }
        } else {
          std::error_code ec;
          fs::remove(tmp, ec);
          VALTZ_LOG_WARN("history", "base not kept: {}",
                         blob.error().message);
        }
      }
      if (!e.picture.empty() && !seen(e.picture.hash)) {
        add.push_back(std::move(e));
      }
    }
  }

  // The result, as it was made.
  if (!made.blob.empty() && !seen(made.blob.hash)) {
    project::HistoryEntry e;
    e.id = HistoryId::make();
    e.created_ms = made.created_ms;
    e.role = "result";
    e.picture = made.blob;
    e.width = made.info.frame.width;
    e.height = made.info.frame.height;
    e.asset = rec.asset;
    e.version = made.number;
    e.label = name_of(rec.asset);
    if (clip) {
      // Its picture's frames (a soundtrack may run longer).
      e.kind = "video";
      e.frames = std::max<std::int64_t>(made.info.frame_count, 1);
      const auto& r = made.info.frame_rate;
      e.seconds = r.num > 0
          ? static_cast<double>(e.frames) * r.den / r.num
          : made.info.duration.seconds();
    } else if (song) {
      // A sound: no picture, its length.
      e.kind = "audio";
      e.width = e.height = 0;
      e.seconds = made.info.duration.seconds();
    }
    add.push_back(std::move(e));
  }
  for (auto& e : add) {
    e.generation = rec.asset;
    e.generation_version = made.number;
    if (auto st = p.add_history(e); !st.ok()) {
      VALTZ_LOG_WARN("history", "not kept: {}", st.error().message);
      return;
    }
  }
  if (!add.empty()) {
    post_("history.changed", rec.id, {{"project", rec.project}});
  }
}

Result<std::vector<Controller::HistoryState>>
Controller::history(ProjectId pid)
{
  project::Project* p = project(pid);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(auto entries, p->history());
  std::vector<HistoryState> out;
  out.reserve(entries.size());
  for (auto& e : entries) {
    auto file = p->blobs().path_of(e.picture);
    out.push_back({std::move(e), std::move(file)});
  }
  return out;
}

fs::path
page_path(const fs::path& destination, std::int64_t page,
          std::int64_t pages)
{
  const int width = static_cast<int>(std::to_string(pages).size());
  const std::string n = std::to_string(page + 1);
  const std::string num =
      std::string(std::max<int>(0, width - static_cast<int>(n.size())),
                  '0') + n;
  return destination.parent_path() /
         std::format("{}-{}{}", destination.stem().string(), num,
                     destination.extension().string());
}

Result<JobId>
Controller::export_asset(ExportRequest req)
{
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  const engine::ExportFormat* f = engine::export_format(req.format);
  if (!f) {
    return make_error(Code::InvalidArgument, msg::kExportFormatUnknown,
                      {{"format", req.format}});
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(req.asset));
  const bool video = a.kind == project::AssetKind::Video;
  const bool sound = a.kind == project::AssetKind::Audio;
  if (video != f->video || sound != f->sound) {
    // What the format is for, said of the format.
    return make_error(Code::InvalidArgument,
                      f->sound   ? msg::kExportNeedsSound
                      : f->video ? msg::kExportNeedsVideo
                                 : msg::kExportNeedsPicture,
                      {{"format", req.format}});
  }
  if (req.destination.empty()) {
    return make_error(Code::InvalidArgument, msg::kExportNotSaved,
                      {{"path", ""}});
  }
  if (!_engine->supports(engine::kOpExportMedia)) {
    return make_error(Code::Unsupported, msg::kEngineCannotRun,
                      {{"operation", std::string(engine::kOpExportMedia)}});
  }
  engine::JobSpec spec;
  spec.id = JobId::make();
  spec.op = std::string(engine::kOpExportMedia);
  spec.params = {{"format", req.format}};
  spec.output_dir = p->blobs().tmp_dir();
  if (req.quality > 0) {
    spec.params["quality"] = std::clamp(req.quality, 1, 100);
  }
  const bool drawn = a.cls == project::AssetClass::Still ||
                     a.cls == project::AssetClass::Composition ||
                     a.cls == project::AssetClass::Markup;
  auto source = [&](const fs::path& file) -> Status {
    VALTZ_ASSIGN(media::MediaInfo info, media::probe_file(file));
    VALTZ_ASSIGN(ContentHash h, hash_file(file));
    spec.inputs.push_back({"source", file, h, info});
    return ok_status();
  };
  // The PROJECT exported: in its output (DESIGN §6b) -- its colour, its
  // sound's channels and rate. Any other asset as it is.
  std::optional<project::OutputSettings> output;
  if (project_composition(req.project) == req.asset) {
    VALTZ_ASSIGN(project::OutputSettings o, p->output());
    output = o;
  }
  int made = 0;
  auto as_output_sound = [&](const fs::path& mix) -> Result<fs::path> {
    if (!output || (output->channels == 2 &&
                    output->sample_rate == media::kMixRate)) {
      return mix;
    }
    const fs::path out = p->blobs().tmp_dir() /
        std::format("{}-sound-{}.wav", spec.id.str(), made++);
    VALTZ_TRY(media::convert_sound(mix, out, output->channels,
                                   output->sample_rate));
    return out;
  };
  auto as_output_picture = [&](const fs::path& pic) -> Result<fs::path> {
    // OpenEXR keeps the light itself (scene-linear): no display space.
    const auto c = output ? media::output_color(output->color)
                          : std::nullopt;
    if (!c || req.format == "exr") {
      return pic;
    }
    const fs::path out = p->blobs().tmp_dir() /
        std::format("{}-picture-{}.png", spec.id.str(), made++);
    VALTZ_TRY(media::convert_picture(pic, out, *c));
    return out;
  };
  if (!drawn) {
    // A flat or generated asset: its file, as it is.
    VALTZ_ASSIGN(fs::path path, p->media_path(req.asset));
    VALTZ_ASSIGN(project::AssetVersion v, p->version(req.asset));
    spec.inputs.push_back({"source", path, v.content, v.info});
  } else if (a.cls == project::AssetClass::Still && a.pages > 1) {
    // A still's PAGES: each one's rendering, a file a page.
    for (std::int64_t i = 0; i < a.pages; ++i) {
      VALTZ_ASSIGN(fs::path drawn_page,
                   rendered_page_(req.project, req.asset, i, 0));
      VALTZ_ASSIGN(fs::path file, as_output_picture(drawn_page));
      VALTZ_TRY(source(file));
      spec.inputs.back().role = "page";
    }
    spec.params["pages"] = a.pages;
  } else if (!video) {
    // A still, a markup, a timeline of sound: its rendering (cached),
    // written as the format says.
    VALTZ_ASSIGN(fs::path drawn_file,
                 rendered_(req.project, req.asset, 0, 0));
    fs::path file = drawn_file;
    if (sound) {
      VALTZ_ASSIGN(file, as_output_sound(drawn_file));
    } else {
      VALTZ_ASSIGN(file, as_output_picture(drawn_file));
    }
    VALTZ_TRY(source(file));
  } else {
    // A timeline with a frame: drawn by Valtz frame by frame into the
    // engine's writer, its mix joined after.
    VALTZ_ASSIGN(media::MovieStack stack,
                 movie_stack(req.project, req.asset, /*for_job=*/true));
    VALTZ_ASSIGN(std::int64_t n, length_of_(req.project, req.asset, 0));
    VALTZ_ASSIGN(media::PixelSize size, shown_size_(*p, req.asset, 0, 0));
    stack.frames = n;
    spec.params["stack"] = media::to_json(stack);
    spec.params["composition"] = true;
    spec.params["frames"] = n;
    spec.params["width"] = size.width;
    spec.params["height"] = size.height;
    if (output) {
      spec.params["color"] = output->color;
    }
    VALTZ_ASSIGN(fs::path mixed, mixed_(req.project, req.asset, 0));
    fs::path mix = mixed;
    if (!mix.empty()) {
      VALTZ_ASSIGN(mix, as_output_sound(mixed));
      VALTZ_ASSIGN(media::MediaInfo info, media::probe_file(mix));
      VALTZ_ASSIGN(ContentHash h, hash_file(mix));
      spec.inputs.push_back({"sound", mix, h, info});
    }
  }

  // Its clock: a long clip's export says how long it has left (its
  // frames' pace; no prior).
  {
    std::lock_guard lk(_timing_mu);
    _timing.insert_or_assign(spec.id, JobTiming(steady_seconds(), Json(),
                                                0, 1));
  }

  JobRecord rec;
  rec.id = spec.id;
  rec.op = spec.op;
  rec.purpose = "export";
  rec.title = a.name;
  rec.project = req.project;
  rec.asset = req.asset;
  rec.destination = req.destination.string();
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  post_("job.queued", rec.id, {{"title", rec.title}, {"op", rec.op},
                               {"purpose", rec.purpose},
                               {"project", req.project},
                               {"asset", req.asset}});
  VALTZ_TRY(_engine->submit(std::move(spec),
                            [this](const engine::JobEvent& ev) {
    on_export_event_(ev);
  }));
  return rec.id;
}

void
Controller::on_export_event_(const engine::JobEvent& ev)
{
  switch (ev.kind) {
  case engine::JobEventKind::Output: {
    // The engine wrote into the project's scratch; the file moves to
    // where it was asked to go (copied, when that is another volume).
    auto rec = _jobs.get(ev.job);
    if (!rec) { break; }
    // A still's page: beside the destination, numbered.
    const auto page = jget<std::int64_t>(ev.data, "page", -1);
    const fs::path dst =
        page >= 0 ? page_path(rec->destination, page,
                              jget<std::int64_t>(ev.data, "pages", 1))
                  : fs::path(rec->destination);
    std::error_code ec;
    if (dst.has_parent_path()) {
      fs::create_directories(dst.parent_path(), ec);
    }
    fs::remove(dst, ec);
    fs::rename(ev.output, dst, ec);
    if (ec) {
      ec.clear();
      fs::copy_file(ev.output, dst, fs::copy_options::overwrite_existing,
                    ec);
      std::error_code ignore;
      fs::remove(ev.output, ignore);
    }
    _jobs.update(ev.job, [&](JobRecord& r) {
      // A page not saved is an empty one: the export failed.
      r.outputs.push_back(ec ? std::string() : dst.string());
      if (page < 0) {
        r.message = ec ? std::string() : dst.string();
      } else if (!ec && r.message.empty()) {
        r.message = dst.string();
      }
    });
    if (ec) {
      VALTZ_LOG_ERROR("export", "could not save {}: {}", dst.string(),
                      ec.message());
    }
    break;
  }
  case engine::JobEventKind::Finished: {
    auto rec = _jobs.get(ev.job);
    if (!rec || rec->message.empty() ||
        std::ranges::any_of(rec->outputs, &std::string::empty)) {
      // Finished, but the file did not reach its destination.
      const std::string where = rec ? rec->destination : std::string();
      finish_job_(ev.job, JobState::Failed, "not saved");
      Json d = message_fields(msg::kExportNotSaved, {{"path", where}});
      d["code"] = "io";
      post_("job.failed", ev.job, d);
      break;
    }
    finish_job_(ev.job, JobState::Finished, rec->message);
    Json d = {{"project", rec->project}, {"asset", rec->asset},
              {"path", rec->message}};
    if (rec->outputs.size() > 1) {
      d["paths"] = rec->outputs;  // a still's pages, in order
    }
    post_("job.finished", ev.job, d);
    break;
  }
  default:
    // Started, progress, failures: as for any build.
    on_build_event_(ev);
    break;
  }
}

Status
Controller::cancel(JobId id)
{
  return _engine->cancel(id);
}

}
