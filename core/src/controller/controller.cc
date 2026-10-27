#include "valtz/controller/controller.h"

#include "controller/agent-host.h"

#include "valtz/assist/drama3.h"
#include "valtz/assist/moss-image.h"
#include "valtz/assist/video-summary.h"
#include "valtz/base/log.h"
#include "valtz/base/text.h"
#include "valtz/base/tree-copy.h"
#include "valtz/media/layers.h"
#include "valtz/media/model-input.h"
#include "valtz/media/sound.h"
#include "valtz/media/probe.h"
#include "valtz/media/thumbnail.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>

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

// The tuning's slots and the engine's are one count: vpipe's.
static_assert(models::kLoraSlots == engine::kLoraSlots);

// A recipe's params that name a weight file of one's own -- each LoRA
// slot's, a DiT's, a VAE's: never sent to a fleet member, nor taken from
// one.
std::vector<std::string>
own_file_keys()
{
  std::vector<std::string> keys = {"lora_file", "dit_file", "vae_file",
                                   "encoder_weights", "vae_weights"};
  for (int k = 2; k <= engine::kLoraSlots; ++k) {
    keys.push_back(std::format("lora{}_file", k));
  }
  return keys;
}

// `what` among a list of strings (a fleet member's ops, models).
bool
listed(const Json& list, std::string_view what)
{
  return std::ranges::any_of(
      list.is_array() ? list : Json::array(), [&](const Json& x) {
        return x.is_string() && x.get_ref<const std::string&>() == what;
      });
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

// How `p` keeps the clips it makes (OutputSettings::intermediate): a
// clip job's "intermediate", recorded with it.
std::string
intermediate_of(project::Project& p)
{
  auto o = p.output();
  return o.ok() ? o->intermediate : project::OutputSettings{}.intermediate;
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

// A prompt NAMED by the person (rename_asset, a request's prompt_name;
// DESIGN §10c): its name is kept as its words change, where one named by
// its words follows them.
constexpr const char* kNamedTag = "named";
// A prompt MADE FROM another by the assistant (apply_suggestion): its
// recipe's op; its input "prompt" the words before it.
constexpr const char* kEnhanceOp = "enhance-prompt";

bool
is_named(const project::Asset& a)
{
  return std::ranges::find(a.tags, std::string(kNamedTag)) != a.tags.end();
}

void
set_named(project::Asset& a, bool on)
{
  std::erase(a.tags, std::string(kNamedTag));
  if (on) {
    a.tags.push_back(kNamedTag);
  }
}

// What a prompt's words name it: its first line, no Markdown -- a
// MOSS-Image layout's, its description.
std::string
words_name(const std::string& text)
{
  const std::string laid = assist::moss_layout_caption(text);
  std::string name =
      !laid.empty()
          ? one_line(laid)
          : one_line(assist::without_markdown(assist::prompt_words(text)));
  return std::string(utf8_prefix(name.empty() ? "Prompt" : name, 64));
}

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

// A tag's form with its number in ("<Picture {n}>", 2: "<Picture 2>").
std::string
numbered(std::string form, int n)
{
  if (const auto at = form.find("{n}"); at != std::string::npos) {
    form.replace(at, 3, std::to_string(n));
  }
  return form;
}

// What a clip model's prompts call the pictures it opens and ends on:
// FL2VA's own form when both are given (catalog `first_last_tag`), else
// its picture tag ("<Picture {n}>").
std::string
keyframe_tag(const models::ModelEntry& m, bool both)
{
  const std::string own =
      both ? jget<std::string>(m.prompting, "first_last_tag", "")
           : std::string();
  return !own.empty()
             ? own
             : jget<std::string>(jget(m.prompting, "reference_tags",
                                      Json::object()),
                                 "image", "");
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

// What serving needs of the controller, as the fleet asks for it.
class Controller::FleetHost final : public fleet::Host {
public:
  explicit FleetHost(Controller& c) : _c(c) {}

  Json fleet_self() override { return _c.fleet_self_(); }

  Status
  fleet_serve(const fleet::Offer& o, const std::string& from,
              const fs::path& out_dir, engine::JobSink sink) override
  {
    return _c.fleet_serve_(o, from, out_dir, std::move(sink));
  }

  void fleet_cancel(JobId id) override { (void)_c._engine->cancel(id); }

  Json
  fleet_load() override
  {
    return _c._engine ? _c._engine->throughput() : Json::object();
  }

  void
  fleet_note(std::string kind, Json data) override
  {
    JobId job;
    if (auto id = JobId::parse(jget<std::string>(data, "job", ""))) {
      job = *id;
    }
    _c.post_(std::move(kind), job, std::move(data));
  }

private:
  Controller& _c;
};

Result<std::unique_ptr<Controller>>
Controller::create(ControllerConfig cfg)
{
  AppPaths paths = AppPaths::standard(cfg.support_root);
  const AppPaths defaults = AppPaths::defaults(cfg.support_root);
  std::error_code ec;
  for (const auto& d : {paths.support, paths.engine}) {
    fs::create_directories(d, ec);
    if (ec) {
      return make_error(Code::Io, std::format("cannot create {}: {}",
                                              d.string(), ec.message()));
    }
  }
  // A folder put elsewhere (locations.json) is made again where its
  // parent is -- not where a drive not connected would be: Valtz starts
  // without it, saying so (locations()).
  std::set<std::string> missing;
  auto reachable = [&ec](const fs::path& d) {
    if (fs::is_directory(d, ec)) {
      return true;
    }
    if (!fs::is_directory(d.parent_path(), ec)) {
      return false;
    }
    fs::create_directories(d, ec);
    return !ec;
  };
  if (!reachable(paths.models)) {
    if (paths.models == defaults.models) {
      return make_error(Code::Io, std::format("cannot create {}",
                                              paths.models.string()));
    }
    VALTZ_LOG_WARN("models", "the models folder {} is not there",
                   paths.models.string());
    missing.insert("models");
  }

  std::unique_ptr<Controller> c(new Controller());
  c->_cfg = cfg;
  c->_paths = paths;
  c->_keep_working_copies = cfg.keep_working_copies;
  cache::CacheStore::Options copts;
  copts.budget_bytes = cfg.cache_budget_bytes;
  if (paths.cache != defaults.cache && !reachable(paths.cache)) {
    // Meanwhile the default one: a cache must be written.
    VALTZ_LOG_WARN("cache", "the cache folder {} is not there: {} for now",
                   paths.cache.string(), defaults.cache.string());
    missing.insert("cache");
    VALTZ_ASSIGN(c->_cache, cache::CacheStore::open(defaults.cache, copts));
  } else {
    VALTZ_ASSIGN(c->_cache, cache::CacheStore::open(paths.cache, copts));
  }
  c->_locations_missing = std::move(missing);
  c->_hw = models::probe_hardware();
  VALTZ_ASSIGN(c->_catalog, models::Catalog::builtin());
  // The models folder and those kept after it moved are Valtz's own.
  const auto roots = cfg.model_roots.empty()
      ? models::ModelStore::default_roots(paths.models, paths.models_kept)
      : cfg.model_roots;
  std::size_t owned = 1;
  if (cfg.model_roots.empty()) {
    for (std::size_t i = 1; i < roots.size(); ++i) {
      if (std::ranges::any_of(paths.models_kept, [&](const fs::path& k) {
            return k.lexically_normal() == roots[i].lexically_normal();
          })) {
        owned = i + 1;
      }
    }
  }
  c->_store = std::make_unique<models::ModelStore>(
      roots, paths.support / "model-links.json", owned);
  // A compress a crash stopped (DESIGN §8b): its passes, never marked,
  // never read -- gone. Only one untouched for six hours: another Valtz
  // (the app, valtzctl) may be writing one now.
  {
    std::error_code ec;
    const fs::path dir = c->_store->download_root() / "compressed";
    const auto stale = fs::file_time_type::clock::now() -
                       std::chrono::hours(6);
    for (const auto& d : fs::directory_iterator(dir, ec)) {
      const std::string n = d.path().filename().string();
      if (!n.starts_with(".") || n.find(".compressing") == std::string::npos) {
        continue;
      }
      auto newest = fs::last_write_time(d.path(), ec);
      for (const auto& f : fs::directory_iterator(d.path(), ec)) {
        newest = std::max(newest, fs::last_write_time(f.path(), ec));
      }
      if (newest < stale) {
        VALTZ_LOG_INFO("models", "a stopped compress's {} removed", n);
        fs::remove_all(d.path(), ec);
      }
    }
  }
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
    c->_video_every = std::max(0.0, jget(helper, "video_every", 0.0));
    if (const auto vs = jget<std::string>(helper, "video_size", "");
        vs == "high" || vs == "highest") {
      c->_video_size = vs;
    }
    c->_assistant_context = std::max<std::int64_t>(
        0, jget<std::int64_t>(helper, "context", 0));
  }
  // VALTZ_ASSISTANT: another helper for this run only, chosen nowhere --
  // valtzctl's --assistant, for the app too (a 24 GB Mac checked on the
  // 9B whatever the person chose).
  if (const char* a = std::getenv("VALTZ_ASSISTANT"); a && *a) {
    c->_assistant = a;
  }
  c->_upscale_tuning = read_settings(paths.support / "upscale.json");

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
    // The wired limit chosen (Settings › General › Memory, DESIGN §10a).
    ecfg.wired_limit_mb = jget<std::uint64_t>(
        read_settings(paths.support / "memory.json"), "wired_limit_mb", 0);
    // A generation's encodings kept between jobs (DESIGN §4l): the cache
    // outlives the engine (declared before it).
    ecfg.cache = c->_cache.get();
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
  // A member of a fleet, as its configuration says (DESIGN §11).
  if (cfg.fleet) {
    fs::path conf = cfg.fleet_config;
    if (conf.empty()) {
      const char* e = std::getenv("VALTZ_FLEET_CONFIG");
      conf = e != nullptr && *e != '\0' ? fs::path(e)
                                        : paths.support / "fleet.json";
    }
    const fs::path work =
        conf.parent_path() / (conf.stem().string() + "-files");
    c->_fleet_host = std::make_unique<FleetHost>(*c);
    c->_fleet = fleet::make_fleet(conf, work, *c->_fleet_host);
    // Members at addresses, without Bonjour ("host:port,..."): a network
    // that does not carry it, or a Mac not allowed to use it.
    if (const char* to = std::getenv("VALTZ_FLEET_CONNECT"); to && *to) {
      std::stringstream in(to);
      std::string one;
      while (std::getline(in, one, ',')) {
        if (const auto colon = one.rfind(':');
            colon != std::string::npos && colon > 0) {
          c->_fleet->connect(one.substr(0, colon),
                             std::atoi(one.c_str() + colon + 1));
        }
      }
    }
  }
  // The agent (DESIGN §9b): its sessions, and the turns it serves for
  // a member's.
  c->_agent = std::make_unique<AgentHost>(*c);
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
  // The agent's sessions first: a turn in flight is a job of the engine.
  if (_agent) {
    _agent->shutdown();
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
  // The fleet first: no job comes in, none goes out.
  if (_fleet) {
    _fleet->stop();
  }
  // Jobs waiting for a download: cancelled, as the engine's queue is.
  std::map<JobId, Waiting> waiting;
  {
    std::lock_guard lk(_downloads_mu);
    waiting.swap(_waiting);
  }
  for (auto& [id, w] : waiting) {
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Cancelled;
    w.sink(ev);
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
  // The caches left behind this run: vpipe's scratch in them, done with
  // now -- and the folder too, emptied by a move or a drop.
  for (const auto& left : _caches_left) {
    std::error_code ec;
    fs::remove_all(left / "scratch", ec);
    remove_empty_dirs(left);
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
  // The arithmetic achieved, here and -- with a member connected -- in
  // the fleet.
  if (_engine) {
    const Json t = _engine->throughput();
    for (const char* k : {"gpu_tflops", "ane_tflops", "peak_tflops"}) {
      if (t.contains(k)) {
        j[k] = t[k];
      }
    }
  }
  if (_fleet) {
    if (Json f = _fleet->throughput(); jget(f, "connected", 0) > 0) {
      j["fleet"] = std::move(f);
    }
  }
  return j;
}

namespace {

// A path as the held ones are compared: absolute, normalized, no
// trailing slash.
std::string
held_path(const fs::path& p)
{
  std::error_code ec;
  fs::path c = fs::weakly_canonical(p, ec);
  std::string s = (ec ? p : c).lexically_normal().string();
  while (s.size() > 1 && s.back() == '/') {
    s.pop_back();
  }
  return s;
}

// What a model's weight files take, for one loaded unwired (vpipe counts
// what a language model reads into its own members nowhere): its
// .safetensors, two folders deep at most.
std::uint64_t
weight_files_bytes(const fs::path& dir)
{
  std::error_code ec;
  std::uint64_t n = 0;
  if (fs::is_regular_file(dir, ec)) {
    return fs::file_size(dir, ec);
  }
  fs::recursive_directory_iterator it(
      dir, fs::directory_options::skip_permission_denied, ec), end;
  for (; !ec && it != end; it.increment(ec)) {
    if (it.depth() > 1) {
      it.disable_recursion_pending();
    }
    if (it->is_regular_file(ec) &&
        it->path().extension() == ".safetensors") {
      n += it->file_size(ec);
    }
  }
  return n;
}

}  // namespace

Json
Controller::memory_held()
{
  Json j = _engine ? _engine->memory_held() : Json::object();
  if (!j.is_object()) {
    return Json::object();
  }
  // Every installed model's folders -- its own, a kept original or
  // copy, its parts linked apart -- for the held directories (a model's
  // folder, a component in it, a part's file) to be named by.
  std::vector<std::pair<std::string, const models::ModelEntry*>> dirs;
  for (const auto& e : _catalog.models()) {
    const auto info = _store->info(e);
    if (info.state != models::InstallState::Installed) {
      continue;
    }
    for (const fs::path& p : {info.dir, info.other_dir}) {
      if (!p.empty()) {
        dirs.emplace_back(held_path(p), &e);
      }
    }
    for (const auto& [part, link] : info.parts) {
      if (!link.file.empty()) {
        dirs.emplace_back(held_path(link.file), &e);
      }
    }
  }
  // The longest folder holding it: a component's model, not its repo's.
  auto owner = [&](const std::string& dir) -> const models::ModelEntry* {
    const std::string d = held_path(dir);
    const models::ModelEntry* best = nullptr;
    std::size_t len = 0;
    for (const auto& [p, e] : dirs) {
      const bool in = d == p || (d.size() > p.size() &&
                                 d.compare(0, p.size(), p) == 0 &&
                                 d[p.size()] == '/');
      if (in && p.size() > len) {
        best = e;
        len = p.size();
      }
    }
    return best;
  };
  for (const char* list : {"warm", "weights"}) {
    if (!j.contains(list) || !j[list].is_array()) {
      j[list] = Json::array();
    }
    for (auto& w : j[list]) {
      if (const auto* e = owner(jget<std::string>(w, "dir", ""))) {
        w["model"] = e->id;
        w["name"] = e->name;
        w["role"] = e->role;
      }
    }
  }
  // ITEMS, what the popover draws, each byte once: a hold kept warm,
  // the checkpoints of its own model and -- a language model's -- its
  // drafters folded in (inside its wired bytes, or, loaded unwired,
  // added to its files' size); then every other checkpoint resident.
  // Its part, by vpipe's kind: "krea2-dit", "qwen-text-encoder",
  // "flux2-vae-decoder", "language-model".
  auto part_of = [](const std::string& kind) -> std::string {
    auto has = [&](std::string_view t) {
      return kind.find(t) != std::string::npos;
    };
    if (kind == "language-model") return "helper";
    if (has("vae")) return "vae";
    if (has("dit")) return "dit";
    if (has("encoder")) return "text-encoder";
    return kind;
  };
  std::vector<bool> folded(j["weights"].size(), false);
  Json items = Json::array();
  for (const auto& w : j["warm"]) {
    const std::string dir = jget<std::string>(w, "dir", "");
    const std::string model = jget<std::string>(w, "model", "");
    const bool lm = jget<std::string>(w, "kind", "") == "language-model";
    // Its own and, a language model's, its drafters (catalog
    // engine.vpipe's mtp_drafter / dflash_drafter, `requires`).
    std::vector<std::string> own{model};
    if (const auto* e = lm ? _catalog.find(model) : nullptr) {
      const Json vp = jget(e->engine, "vpipe", Json::object());
      for (const char* k : {"mtp_drafter", "dflash_drafter"}) {
        own.push_back(jget<std::string>(vp, k, ""));
      }
      own.insert(own.end(), e->requires_models.begin(),
                 e->requires_models.end());
    }
    std::uint64_t bytes = jget<std::uint64_t>(w, "bytes", 0);
    bool estimated = false;
    // The drafters' weight sets, and all it folds in.
    std::uint64_t folded_bytes = 0, folded_all = 0;
    for (std::size_t i = 0; i < j["weights"].size(); ++i) {
      const auto& c = j["weights"][i];
      const std::string m = jget<std::string>(c, "model", "");
      const bool same_dir = jget<std::string>(c, "dir", "") == dir;
      // A stage's model folds its own folder's set alone: a DiT and a
      // VAE of one model are each their own.
      if (folded[i] || !(same_dir || (lm && !m.empty() &&
          std::find(own.begin(), own.end(), m) != own.end()))) {
        continue;
      }
      folded[i] = true;
      const auto n = jget<std::uint64_t>(c, "bytes", 0);
      folded_all += n;
      if (!same_dir) folded_bytes += n;
    }
    if (lm && bytes == 0) {
      // Loaded unwired: vpipe counts none of what it read -- its files'
      // size stands for it, its drafters' weight sets beside them.
      bytes = weight_files_bytes(dir) + folded_bytes;
      estimated = bytes > 0;
    } else if (bytes < folded_all) {
      // Kept with fewer bytes than its weight sets hold (a VAE kept with
      // none): those, at least.
      bytes = folded_all;
      estimated = true;
    }
    Json it{{"part", part_of(jget<std::string>(w, "kind", ""))},
            {"kind", jget<std::string>(w, "kind", "")},
            {"name", jget<std::string>(w, "name",
                                       fs::path(dir).filename().string())},
            {"model", model},
            {"bytes", bytes},
            {"kv_bytes", jget<std::uint64_t>(w, "kv_bytes", 0)},
            {"seconds", jget<double>(w, "seconds", 0.0)},
            {"warm", true}};
    if (estimated) it["estimated"] = true;
    items.push_back(std::move(it));
  }
  for (std::size_t i = 0; i < j["weights"].size(); ++i) {
    if (folded[i]) {
      continue;
    }
    const auto& c = j["weights"][i];
    const std::string dir = jget<std::string>(c, "dir", "");
    items.push_back(
        {{"part", "weights"},
         {"name", jget<std::string>(c, "name",
                                    fs::path(dir).filename().string())},
         {"model", jget<std::string>(c, "model", "")},
         {"bytes", jget<std::uint64_t>(c, "bytes", 0)},
         {"holders", jget<std::int64_t>(c, "holders", 0)},
         {"parked", jget<bool>(c, "parked", false)},
         {"warm", false}});
  }
  j["items"] = std::move(items);
  return j;
}

namespace {

constexpr std::uint64_t kMB = std::uint64_t{1} << 20;
constexpr std::uint64_t kGB = std::uint64_t{1} << 30;
// The least a limit set may be: below it a model's scratch alone goes
// unprotected, and nothing it runs from stays wired.
constexpr std::uint64_t kWiredLimitMin = kGB;

// A size in GB as a person reads it in a message: "17.8 GB", "8 GB".
std::string
gb_words(std::uint64_t bytes)
{
  const double gb = static_cast<double>(bytes) / static_cast<double>(kGB);
  return std::abs(gb - std::round(gb)) < 0.05
             ? std::format("{:.0f} GB", gb)
             : std::format("{:.1f} GB", gb);
}

}  // namespace

std::uint64_t
Controller::wired_limit_max_() const
{
  // What the GPU keeps resident: vpipe caps its pool there -- wiring past
  // it buys a working set the driver pages against.
  return _hw.gpu_working_set_bytes > 0 ? _hw.gpu_working_set_bytes
                                       : _hw.ram_bytes;
}

Json
Controller::wired_limit() const
{
  const std::uint64_t max = wired_limit_max_();
  const std::uint64_t chosen = jget<std::uint64_t>(
      read_settings(_paths.support / "memory.json"), "wired_limit_mb", 0) *
      kMB;
  const Json e = _engine->wired_limit();
  // Automatic is vpipe's own (its wired_pool_pct, 75% of the memory),
  // capped where vpipe caps it; without an engine, its rule.
  std::uint64_t automatic = jget<std::uint64_t>(e, "auto_mb", 0) * kMB;
  if (automatic == 0) {
    automatic = _hw.ram_bytes / 4 * 3;
  }
  automatic = std::min(automatic, max);
  const Json held = _engine->memory_held();
  return {{"chosen", chosen},
          {"automatic", automatic},
          {"min", std::min(kWiredLimitMin, max)},
          {"max", max},
          {"in_force", std::min(jget<std::uint64_t>(e, "limit_mb", 0) * kMB,
                                max)},
          {"used", jget<std::uint64_t>(held, "wired_used", 0)},
          {"pending", jget(e, "pending", false)}};
}

Status
Controller::set_wired_limit(std::uint64_t bytes)
{
  const std::uint64_t max = wired_limit_max_();
  const std::uint64_t min = std::min(kWiredLimitMin, max);
  if (bytes != 0 && (bytes < min || bytes > max)) {
    return make_error(Code::InvalidArgument, msg::kWiredLimitRange,
                      {{"min", gb_words(min)}, {"max", gb_words(max)}});
  }
  const std::uint64_t mb = bytes / kMB;
  const fs::path file = _paths.support / "memory.json";
  Json all = read_settings(file);
  if (mb == 0) {
    all.erase("wired_limit_mb");
  } else {
    all["wired_limit_mb"] = mb;
  }
  VALTZ_TRY(write_settings(file, all));
  _engine->set_wired_limit_mb(mb);
  return ok_status();
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
  // What the fleet's members read of this one changed: busy or not,
  // the models here.
  const bool tell = _fleet && (kind == "job.queued" ||
                               kind == "job.started" ||
                               kind == "job.finished" ||
                               kind == "job.failed" ||
                               kind == "job.cancelled" ||
                               kind == "models.changed");
  // An agent's picture landing (on the agent's own thread); the prompt
  // enhancer's reply, for the agent's own enhance_prompt.
  if (_agent && (kind == "job.finished" || kind == "job.failed" ||
                 kind == "job.cancelled" || kind == "assist.enhanced")) {
    _agent->on_event(kind, job, data);
  }
  Event ev;
  ev.kind = std::move(kind);
  ev.job = job;
  ev.data = std::move(data);
  ev.tensor = std::move(tensor);
  _bus.post(std::move(ev));
  if (tell) {
    _fleet->self_changed();
  }
}

void
Controller::finish_job_(JobId id, JobState st, std::string message)
{
  // A failure in the Log too: the app says it for a moment (an
  // upscale's soundtrack refused flashed by, and the Log had nothing).
  if (st == JobState::Failed) {
    const auto r = _jobs.get(id);
    VALTZ_LOG_WARN("jobs", "{} {} failed: {}",
                   r ? (r->title.empty() ? r->op : r->title)
                     : std::string("job"),
                   id.str(), message);
  }
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
  {
    std::lock_guard lk(_timing_mu);
    _timing.erase(id);
  }
  // What was held for it goes on, or fails.
  release_held_(id, st);
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
  auto caps = !_engine || !_engine->available()
                  ? models::resolve_capabilities(_catalog, *_store, _hw,
                                                 nullptr)
                  : models::resolve_capabilities(
                        _catalog, *_store, _hw,
                        [this](Capability c) { return engine_runs_(c); });
  // What a member taking jobs offers is ready here too: it runs there.
  if (_fleet) {
    std::set<std::string> theirs;
    for (const auto& member : _fleet->members()) {
      const Json self = jget(member, "self", Json::object());
      if (!jget(self, "accepting", false)) {
        continue;
      }
      for (const auto& f : jget(self, "features", Json::array())) {
        if (f.is_string()) {
          theirs.insert(f.get<std::string>());
        }
      }
    }
    for (auto& c : caps) {
      if (c.availability != models::Availability::Ready &&
          theirs.contains(models::to_str(c.capability))) {
        c.availability = models::Availability::Ready;
      }
    }
  }
  // What is on its way: a job asked of it waits for its download.
  for (auto& c : caps) {
    for (auto& o : c.options) {
      o.downloading = o.entry && downloading(o.entry->id);
    }
  }
  return caps;
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
  case Capability::Transcribe:
    return _engine->supports(engine::kOpTranscribeAudio);
  case Capability::VideoSummary:
    return _engine->supports(engine::kOpSummarizeVideo);
  case Capability::AudioSeparate:
    return _engine->supports(engine::kOpSeparateAudio);
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
  // In a folder of Valtz's own: the models folder, or one kept.
  auto ours = [this](const fs::path& p) {
    return !_store->own_root_of(p).empty();
  };
  Json families = Json::array();
  const Json sources = _engine->download_sources();
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
                 : ours(info.dir)              ? "valtz"
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
        // The parts that can run from Draw Things checkpoints (catalog
        // `check.drawthings`), and the ones linked.
        {"drawthings", [&] {
           Json parts = Json::array();
           const Json dt = jget(e->check, "drawthings", Json::object());
           for (auto it = dt.begin(); dt.is_object() && it != dt.end();
                ++it) {
             parts.push_back(it.key());
           }
           return parts;
         }()},
      });
      for (const auto& [part, l] : info.parts) {
        members.back()["parts"][part] = {{"path", l.file.string()},
                                         {"present", l.present}};
      }
      // Where it can be downloaded from: each source that carries its
      // repo, under its path there (a mirror may name it otherwise).
      if (e->quantize_from.empty() && !e->hf_path.empty()) {
        Json from = Json::array();
        for (const auto& s : sources) {
          const auto name = jget<std::string>(s, "name", "");
          if (auto there = _engine->source_path(name, e->hf_path);
              !there.empty()) {
            from.push_back({{"name", name}, {"path", there}});
          }
        }
        members.back()["sources"] = std::move(from);
      }
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
      // A compressed copy in its download's place (DESIGN §8b): its
      // width, and what the download took -- or kept beside it, which
      // of the two is in use.
      Json& row = members.back();
      if (const int bits = std::max(info.compressed_bits, info.other_bits);
          bits > 0) {
        const bool in_use = info.compressed_bits > 0;
        const fs::path copy = in_use ? info.dir : info.other_dir;
        std::ifstream in(copy / models::ModelStore::kCompressedMarker);
        const Json mark = in ? Json::parse(in, nullptr, false) : Json();
        row["compressed"] = {
            {"bits", bits},
            {"bytes", in_use ? info.bytes : info.other_bytes},
            {"original_bytes", jget<std::uint64_t>(mark, "original_bytes",
                                                   0)},
            {"in_use", in_use},
            {"original_kept", !info.other_dir.empty()}};
      }
      // Compressible: what each width would take -- or why not here (not
      // installed, compressed already, a link's or vpipe's files).
      if (!e->compress.empty()) {
        const std::string why =
            !installed                ? "missing"
            : info.compressed_bits > 0 || info.other_bits > 0
                ? "compressed"
            : info.linked              ? "link"
            : !ours(info.dir)          ? "vpipe"
            : !_engine->supports(engine::kOpQuantizeModel) ? "engine"
                                                           : "";
        Json widths = Json::array();
        if (why.empty()) {
          for (const int bits : {8, 4}) {
            const auto est = compress_estimate_(*e, info.dir, bits);
            widths.push_back(
                {{"bits", bits}, {"bytes", est.total},
                 {"saves", est.original > est.total
                               ? est.original - est.total : 0}});
          }
        }
        row["compress"] = {{"ready", why.empty()}, {"why", why},
                           {"widths", std::move(widths)}};
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
          {"download_root", valtz_root.string()},
          {"download_sources", sources},
          {"download_source", download_source_()}};
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

// When `path` was made (its birth time, as the Finder's "Created"), ms
// since 1970; 0 when it cannot be read.
std::int64_t
created_ms(const fs::path& path)
{
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) {
    return 0;
  }
  return static_cast<std::int64_t>(st.st_birthtimespec.tv_sec) * 1000 +
         st.st_birthtimespec.tv_nsec / 1'000'000;
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

  // Models: each repo folder in the managed roots -- the models folder,
  // those kept after it moved -- named by the catalog entries it holds.
  const fs::path mroot = _store->download_root();
  Json models = Json::array();
  std::uint64_t mtotal = 0;
  std::vector<fs::directory_entry> orgs;
  for (const auto& root : _store->owned_roots()) {
    for (const auto& org : fs::directory_iterator(root, ec)) {
      orgs.push_back(org);
    }
  }
  for (const auto& org : orgs) {
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
      // A compressed copy's folder (compressed/<id>-<bits>bit) is named
      // by where its model is, not by its repo.
      // The original kept beside one too, in use or not.
      for (const auto& e : _catalog.models()) {
        const auto info = _store->info(e);
        const auto in = [&](const fs::path& d) {
          return !d.empty() &&
                 d.string().starts_with(repo.path().string());
        };
        if ((e.hf_path == hf || info.compressed_bits > 0 ||
             info.other_bits > 0) &&
            (in(info.dir) || in(info.other_dir))) {
          names.push_back(e.name);
        }
      }
      const std::uint64_t n = folder_bytes(repo.path());
      mtotal += n;
      models.push_back({{"repo", hf}, {"names", std::move(names)},
                        {"bytes", n}, {"created", created_ms(repo.path())}});
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
  const fs::path proot = paths().projects;
  for (const auto& e : fs::directory_iterator(proot, ec)) {
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
                 {"created", created_ms(pkg)},
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
                            {"bytes", b}, {"linked", a.linked},
                            {"created", a.created_ms}});
        }
      }
      item["assets"] = std::move(assets);
      // The records, superseded markup, temporary files.
      item["other"] = n > held ? n - held : 0;
    }
    projects.push_back(std::move(item));
  }
  out["projects"] = {{"root", proot.string()}, {"bytes", ptotal},
                     {"items", std::move(projects)}};

  out["cache"] = {{"root", _cache->root().string()},
                  {"bytes", _cache->size_bytes()},
                  {"budget", _cache->budget_bytes()}};
  return out;
}

Result<Json>
Controller::link_model(const std::string& id, const fs::path& target)
{
  const models::ModelEntry* e = _catalog.find(id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel, {{"model", id}});
  }
  std::error_code ec;
  if (!fs::exists(target, ec)) {
    return make_error(Code::NotFound,
                      std::format("{} is not there", target.string()));
  }
  // Checked before it is linked: a wrong folder is said now, not at the
  // first generation.
  const models::ModelCheck check = models::check_model(*e, target,
                                                       &_catalog);
  Json out = check.to_json();
  out["linked"] = false;
  if (!check.usable) {
    VALTZ_LOG_INFO("models", "{} not linked to {}: {}", id,
                   target.string(),
                   jget<std::string>(check.error(), "message", ""));
    return out;
  }
  // Its parts (Draw Things checkpoints): the model brings the rest, and
  // says so while it is not here.
  if (!check.parts.empty()) {
    for (const auto& [part, file] : check.parts) {
      VALTZ_TRY(_store->link_part(*e, part, file));
    }
    out["linked"] = true;
    if (_store->info(*e).state != models::InstallState::Installed) {
      Json n = message_fields(msg::kCheckDrawThingsNeedsModel,
                              {{"model", e->name}});
      n["level"] = "warning";
      out["notes"].insert(out["notes"].begin(), std::move(n));
    }
    for (const auto& [part, file] : check.parts) {
      VALTZ_LOG_INFO("models", "{}'s {} linked to {}", id, part,
                     file.string());
    }
    post_("models.changed", JobId{},
          {{"model", id}, {"reason", "linked"}});
    return out;
  }
  VALTZ_TRY(_store->link(*e, check.path));
  out["linked"] = true;
  VALTZ_LOG_INFO("models", "{} linked to {} ({}{})", id,
                 check.path.string(),
                 check.confirmed ? "confirmed" : "unconfirmed",
                 check.moved.empty() ? "" : ", moved " + check.moved);
  post_("models.changed", JobId{}, {{"model", id}, {"reason", "linked"}});
  return out;
}

Result<Json>
Controller::check_model(const std::string& id, const fs::path& target) const
{
  const models::ModelEntry* e = _catalog.find(id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel, {{"model", id}});
  }
  fs::path at = target;
  if (at.empty()) {
    const auto info = _store->info(*e);
    if (info.state != models::InstallState::Installed) {
      return make_error(Code::NotFound, msg::kModelNotInstalled,
                        {{"model", e->name}});
    }
    at = info.path();
  }
  std::error_code ec;
  if (!fs::exists(at, ec)) {
    return make_error(Code::NotFound,
                      std::format("{} is not there", at.string()));
  }
  Json out = models::check_model(*e, at, &_catalog).to_json();
  out["installed"] = target.empty();
  return out;
}

Status
Controller::unlink_model(const std::string& id, bool parts)
{
  if (!_catalog.find(id)) {
    return make_error(Code::NotFound, msg::kUnknownModel, {{"model", id}});
  }
  if (parts) {
    _store->unlink_parts(id);
  } else {
    _store->unlink(id);
  }
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
  if (is_prompt(a)) {
    // Named by the person; "" -- by its words again.
    const bool given = !name.empty();
    if (!given) {
      VALTZ_ASSIGN(std::string text, p->read_text(aid));
      name = words_name(text);
    }
    name = std::string(utf8_prefix(name, 200));
    if (name == a.name && is_named(a) == given) {
      return ok_status();
    }
    VALTZ_TRY(p->update_asset(aid, [&](project::Asset& r) {
      r.name = name;
      set_named(r, given);
      return ok_status();
    }));
    post_("assets.changed", JobId{}, {{"project", pid}, {"asset", aid},
                                      {"reason", "renamed"}});
    return ok_status();
  }
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

namespace {

// The TASKS of a fleet (DESIGN §11): the engine ops a member runs for
// another -- what this Mac offers, what its offload policies are set for.
constexpr std::string_view kFleetTasks[] = {
    engine::kOpGenerateImage,   engine::kOpEditImage,
    engine::kOpGenerateVideo,   engine::kOpGenerateAudio,
    engine::kOpGenerateSpeech,  engine::kOpTranscribeAudio,
    engine::kOpSummarizeVideo,  engine::kOpUpscaleVideo,
    engine::kOpUpscaleImage,    engine::kOpChat,
    engine::kOpAgentTurn};

// A job of `op` can name `m` as its model. (A chat names none: a member
// writes with its own helper.)
bool
does_task(const models::ModelEntry& m, std::string_view op)
{
  using models::Capability;
  if (op == engine::kOpGenerateImage) return m.has(Capability::TextToImage);
  if (op == engine::kOpEditImage) return m.has(Capability::ImageEdit);
  if (op == engine::kOpGenerateVideo) {
    return m.has(Capability::TextToVideo) ||
           m.has(Capability::ImageToVideo) ||
           m.has(Capability::ReferenceToVideo);
  }
  if (op == engine::kOpGenerateAudio) return m.has(Capability::TextToAudio);
  if (op == engine::kOpGenerateSpeech) {
    return m.has(Capability::TextToSpeech);
  }
  if (op == engine::kOpTranscribeAudio) return m.has(Capability::Transcribe);
  if (op == engine::kOpSummarizeVideo) {
    return m.has(Capability::VideoSummary);
  }
  if (op == engine::kOpUpscaleVideo) return m.has(Capability::UpscaleVideo);
  if (op == engine::kOpUpscaleImage) return m.has(Capability::UpscaleImage);
  return false;
}

// The engine op that runs `m` for "<modality>/<op>" (runs()' words).
std::string_view
task_of(const models::ModelEntry& m, std::string_view modality,
        std::string_view op)
{
  if (modality == "image") {
    return op == "edit" ? engine::kOpEditImage : engine::kOpGenerateImage;
  }
  if (modality == "video") {
    return engine::kOpGenerateVideo;
  }
  return m.has(models::Capability::TextToSpeech) ? engine::kOpGenerateSpeech
                                                 : engine::kOpGenerateAudio;
}

// A model of clips from references that also makes one from the prompt
// alone, given an empty reference list (catalog `references.prompt_only`:
// MiniMax H3 Ref2VA, vpipe's prompt-only request). It cannot open on a
// picture: that sequence has no keyframe.
bool
prompt_only_references(const models::ModelEntry& m)
{
  const Json rv = jget(jget(m.engine, "vpipe", Json::object()), "references",
                       Json());
  return m.has(models::Capability::ReferenceToVideo) && rv.is_object() &&
         jget(rv, "prompt_only", false);
}

}  // namespace

bool
Controller::runs(const models::ModelEntry& m, std::string_view modality,
                 std::string_view op) const
{
  const std::string key = std::format("{}/{}/{}", modality, op, m.id);
  // Set to run on the fleet only: where a member runs it.
  if (offload_policy_(task_of(m, modality, op), m.id) ==
      fleet::kOffloadAlways) {
    return fleet_runs_(key);
  }
  if (runs_here(m, modality, op)) {
    return true;
  }
  // A member taking jobs runs it (what it says it runs).
  return fleet_runs_(key);
}

bool
Controller::runs_here(const models::ModelEntry& m, std::string_view modality,
                      std::string_view op) const
{
  return runs_here_(m, modality, op, false);
}

bool
Controller::runs_soon(const models::ModelEntry& m, std::string_view modality,
                      std::string_view op) const
{
  if (runs(m, modality, op)) {
    return true;
  }
  return offload_policy_(task_of(m, modality, op), m.id) !=
             fleet::kOffloadAlways &&
         runs_here_(m, modality, op, true);
}

bool
Controller::runs_here_(const models::ModelEntry& m, std::string_view modality,
                       std::string_view op, bool soon) const
{
  // Installed -- or, `soon`, on its way.
  const auto here = [&](const models::ModelEntry& e) {
    return soon ? coming_(e)
                : _store->info(e).state == models::InstallState::Installed;
  };
  if (!_engine || !_engine->available() || !here(m) ||
      _hw.ram_gb() < m.min_ram_gb) {
    return false;
  }
  if (modality == "image" && op == "generate") {
    return m.has(models::Capability::TextToImage) &&
           _engine->supports(engine::kOpGenerateImage);
  }
  if (modality == "image" && op == "edit") {
    return can_edit_(m, false);
  }
  // A clip from words: a text-to-video model's, or a reference model
  // that takes an empty reference list (catalog `references.prompt_only`:
  // vpipe runs Ref2VA's weights from the prompt alone) -- Auto's last
  // resort, where FL2VA is not here.
  if (modality == "video" && op == "generate") {
    return (m.has(models::Capability::TextToVideo) ||
            prompt_only_references(m)) &&
           _engine->supports(engine::kOpGenerateVideo);
  }
  if (modality == "audio" && (op == "generate" || op == "edit")) {
    // Its latents (or codes) are another checkpoint's to decode: YuE2's
    // VAE, MOSS-TTS's audio tokenizer.
    const auto* d = _catalog.find(jget<std::string>(
        jget(m.engine, "vpipe", Json::object()), "decoder", ""));
    const bool decodes = !d || here(*d);
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
  // Installed -- or downloading: a job asked now waits for it (DESIGN
  // §3a), so a preset is settled as it will run.
  auto adapter = [&](const char* block, const char* key) {
    return _catalog.find(jget<std::string>(
        jget(vp, block, Json::object()), key, ""));
  };
  auto installed = [&](const char* block, const char* key) {
    const auto* e = adapter(block, key);
    return e && coming_(*e);
  };
  models::TuningContext c;
  c.turbo_installed = installed("turbo", "lora");
  if (const auto* t = adapter("turbo", "lora"); t && coming_(*t)) {
    c.turbo_name = t->name;
    c.turbo_path = model_path_(*t).string();
  }
  c.hyperflow_installed = installed("hyperflow", "lora");
  c.taomate_installed = installed("taomate", "lora");
  c.vdn_installed = installed("vdn", "branch");
  c.matrix_cores = _hw.gpu_matrix_cores;
  c.gpu_cores = _hw.gpu_cores;
  c.ane_cores = _hw.ane_cores;
  c.ram_gb = _hw.ram_gb();
  // The family's LoRAs: what a preset's table picks its adapter from --
  // each labelled as Settings > Capabilities lists it.
  for (const auto& e : _catalog.models()) {
    if (e.role != "lora" || e.family != m.family) {
      continue;
    }
    std::string label;
    for (const auto& f : _catalog.families()) {
      for (const auto& mem : f.members) {
        if (mem.model == e.id) {
          label = mem.label;
        }
      }
    }
    c.loras[e.id] = {e.name,
                     coming_(e) ? model_path_(e).string() : std::string(),
                     label};
  }
  return c;
}

Status
Controller::check_preset_loras_(const models::ModelEntry& m,
                                std::string_view preference,
                                const Json& overrides,
                                bool continuation) const
{
  // Not on this Mac, nor on its way: a fleet member runs it, with its
  // own (DESIGN §11).
  if (!coming_(m)) {
    return ok_status();
  }
  // Custom's own LoRAs, or the adapter turned off, say otherwise.
  if (overrides.is_object() &&
      (overrides.contains("loras") || overrides.contains("turbo"))) {
    return ok_status();
  }
  auto ctx = tuning_context(m);
  ctx.continuation = continuation;
  const Json miss = models::missing_loras(m, ctx, preference, overrides);
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
  // A checkpoint with a schedule of its own runs its own count: that is
  // what is recorded, whatever was asked.
  if (jget(vp, "steps_fixed", false)) {
    r.params["steps"] = jget(jget(vp, "defaults", Json::object()), "steps",
                             8);
  }
  // The LoRA slots (vpipe's eight): HyperFlow's adapter first when it is
  // on, then the LoRAs on, in list order. The catalog's Turbo LoRA in the
  // first is recorded by its id; anything else by its file. The job is
  // given the files.
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
    if (slots.size() >= static_cast<std::size_t>(engine::kLoraSlots)) {
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
  // The slots after it take files (a catalog adapter by its file too):
  // "lora2_file" .. "lora8_file".
  for (std::size_t i = 1; i < slots.size(); ++i) {
    std::string file = slots[i].file;
    if (file.empty()) {
      for (const auto& [f, id] : catalog_ids) {
        if (id == slots[i].id) {
          file = f;
        }
      }
    }
    r.params[std::format("lora{}_file", i + 1)] = file;
    r.params[std::format("lora{}_scale", i + 1)] = slots[i].scale;
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
  // The parts the model is linked to apart (Draw Things checkpoints,
  // Settings > Capabilities), recorded: its DiT and VAE where Custom
  // names none of its own (its VAE is read as a whole model's, by
  // model-select), its text encoder always.
  const auto info = _store->info(m);
  if (const auto d = info.part("dit");
      !d.empty() && !r.params.contains("dit_file")) {
    r.params["dit_file"] = d.string();
  }
  if (const auto v = info.part("vae");
      !v.empty() && !r.params.contains("vae_file")) {
    r.params["vae_weights"] = v.string();
  }
  if (const auto c = info.part("encoder"); !c.empty()) {
    r.params["encoder_weights"] = c.string();
  }
  if (jget(t, "vdn", false)) {
    r.params["branch"] = jget<std::string>(jget(vp, "vdn", Json::object()),
                                           "branch", "");
  }
  // The rest as settled: what the graph builder lays on the stages.
  r.params["tuning"] = t;
  // Made by a compressed copy of the model (DESIGN §8b): its width, for
  // whoever compares it with the original's.
  if (const int bits = _store->info(m).compressed_bits; bits > 0) {
    r.params["weights_bits"] = bits;
  }
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
          {"taomate", jget(v, "taomate", false)},
          {"taomate_lora", jget(v, "taomate_lora", false)}};
}

Result<Json>
Controller::tuning(const std::string& model_id, std::string_view preference,
                   bool edit, const Json& overrides,
                   bool continuation) const
{
  const auto* m = _catalog.find(model_id);
  if (!m) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  auto ctx = tuning_context(*m);
  ctx.continuation = continuation;
  return models::tuning_options(*m, ctx, preference, edit, overrides);
}

// ---- the upscalers' options (Settings › Upscaling) ---------------------

namespace {

// An upscale runs as Med would: int8 GEMMs where the GPU has matrix
// cores, the rest off -- unless Settings › Upscaling says otherwise.
constexpr std::string_view kUpscalePreference = "balanced";

// The kinds an upscaler restores, as Settings › Upscaling names them.
constexpr std::pair<std::string_view, models::Capability> kUpscaleKinds[] = {
    {"video", models::Capability::UpscaleVideo},
    {"image", models::Capability::UpscaleImage}};

// upscale.json's key for the pick of each kind: {"video": id, ...} --
// beside the upscalers' own values, which are keyed by model id.
constexpr char kUpscaleUse[] = "use";

}

Json
Controller::upscale_settings() const
{
  // What an upscale of each kind runs: the pick where it is installed,
  // else Auto's -- the best installed by rank (operation_model_'s order,
  // by what is on disk alone: valtzctl lists this without an engine).
  const auto installed = [&](const models::ModelEntry* e) {
    return e && _store->info(*e).state == models::InstallState::Installed;
  };
  Json kinds = Json::array();
  std::vector<std::string> running;
  for (const auto& [kind, cap] : kUpscaleKinds) {
    const models::ModelEntry* chosen = chosen_upscaler_(cap);
    const models::ModelEntry* any = nullptr;
    for (const auto& e : _catalog.models()) {
      if (e.role == "upscale" && e.has(cap) && installed(&e) &&
          (!any || e.rank > any->rank)) {
        any = &e;
      }
    }
    const models::ModelEntry* runs = installed(chosen) ? chosen : any;
    if (runs) {
      running.push_back(runs->id);
    }
    kinds.push_back({{"kind", std::string(kind)},
                     {"chosen", chosen ? chosen->id : std::string()},
                     {"auto", any ? any->id : std::string()},
                     {"runs", runs ? runs->id : std::string()}});
  }
  // Listed by kind, then as Auto ranks them (FlashVSR+ Turbo, Base, 1.1).
  std::vector<const models::ModelEntry*> ups;
  for (const auto& e : _catalog.models()) {
    if (e.role == "upscale") {
      ups.push_back(&e);
    }
  }
  std::ranges::stable_sort(ups, [](const auto* a, const auto* b) {
    const bool va = a->has(models::Capability::UpscaleVideo);
    const bool vb = b->has(models::Capability::UpscaleVideo);
    return va != vb ? va : a->rank > b->rank;
  });
  Json list = Json::array();
  for (const auto* ep : ups) {
    const auto& e = *ep;
    Json kept;
    {
      std::lock_guard lk(_upscale_tuning_mu);
      kept = jget(_upscale_tuning, e.id.c_str(), Json::object());
    }
    list.push_back(
        {{"id", e.id},
         {"name", e.name},
         {"kind", e.has(models::Capability::UpscaleVideo) ? "video"
                                                          : "image"},
         {"state", models::to_str(_store->info(e).state)},
         {"runs", std::ranges::find(running, e.id) != running.end()},
         {"custom", kept.is_object() && !kept.empty()},
         {"tuning", models::tuning_options(e, tuning_context(e),
                                           kUpscalePreference, false,
                                           kept)}});
  }
  return {{"upscalers", std::move(list)}, {"kinds", std::move(kinds)}};
}

const models::ModelEntry*
Controller::chosen_upscaler_(models::Capability cap) const
{
  const auto kind = std::ranges::find(
      kUpscaleKinds, cap,
      &std::pair<std::string_view, models::Capability>::second);
  if (kind == std::ranges::end(kUpscaleKinds)) {
    return nullptr;
  }
  std::string id;
  {
    std::lock_guard lk(_upscale_tuning_mu);
    id = jget<std::string>(jget(_upscale_tuning, kUpscaleUse, Json()),
                           std::string(kind->first).c_str(), "");
  }
  const auto* e = id.empty() ? nullptr : _catalog.find(id);
  return e && e->role == "upscale" && e->has(cap) ? e : nullptr;
}

Status
Controller::use_upscaler(std::string_view kind, const std::string& model)
{
  const auto k = std::ranges::find(
      kUpscaleKinds, kind,
      &std::pair<std::string_view, models::Capability>::first);
  if (k == std::ranges::end(kUpscaleKinds)) {
    return make_error(Code::InvalidArgument,
                      std::format("an upscaler restores video or image, "
                                  "not {}", kind));
  }
  const bool any = model.empty() || model == "auto";
  if (!any) {
    const auto* m = _catalog.find(model);
    if (!m) {
      return make_error(Code::NotFound, msg::kUnknownModel,
                        {{"model", model}});
    }
    if (m->role != "upscale" || !m->has(k->second)) {
      return make_error(Code::InvalidArgument,
                        std::format("{} does not upscale {}", m->name,
                                    kind));
    }
  }
  const fs::path file = _paths.support / "upscale.json";
  {
    std::lock_guard lk(_upscale_tuning_mu);
    Json all = read_settings(file);
    Json use = jget(all, kUpscaleUse, Json::object());
    if (!use.is_object()) {
      use = Json::object();
    }
    if (any) {
      use.erase(std::string(kind));
    } else {
      use[std::string(kind)] = model;
    }
    if (use.empty()) {
      all.erase(kUpscaleUse);
    } else {
      all[kUpscaleUse] = std::move(use);
    }
    VALTZ_TRY(write_settings(file, all));
    _upscale_tuning = std::move(all);
  }
  post_("upscale.changed", JobId{},
        {{"kind", std::string(kind)}, {"use", any ? "" : model}});
  return ok_status();
}

Status
Controller::set_upscale_tuning(const std::string& model, const Json& values,
                               bool reset)
{
  const auto* m = _catalog.find(model);
  if (!m) {
    return make_error(Code::NotFound, msg::kUnknownModel, {{"model", model}});
  }
  if (m->role != "upscale") {
    return make_error(Code::InvalidArgument,
                      std::format("{} is not an upscaler", m->name));
  }
  if (!values.is_object()) {
    return make_error(Code::InvalidArgument,
                      "an upscaler's options are {key: value}");
  }
  const fs::path file = _paths.support / "upscale.json";
  const auto ctx = tuning_context(*m);
  const Json defaults = models::resolve_tuning(*m, ctx, kUpscalePreference,
                                               false);
  Json kept = Json::object();
  {
    std::lock_guard lk(_upscale_tuning_mu);
    Json all = read_settings(file);
    // The values kept, these laid on them -- as it would run: a key it
    // does not take, a value out of range, one this Mac cannot have, is
    // not kept -- and only where they differ from the defaults: none
    // kept, none changed.
    Json asked = reset ? Json::object()
                       : jget(all, model.c_str(), Json::object());
    if (!asked.is_object()) {
      asked = Json::object();
    }
    for (const auto& [k, v] : values.items()) {
      asked[k] = v;
    }
    const Json settled = models::resolve_tuning(*m, ctx, kUpscalePreference,
                                                false, asked);
    for (const auto& [k, v] : asked.items()) {
      if (settled.contains(k) &&
          settled[k] != jget(defaults, k.c_str(), Json())) {
        kept[k] = settled[k];
      }
    }
    if (kept.empty()) {
      all.erase(model);
    } else {
      all[model] = kept;
    }
    VALTZ_TRY(write_settings(file, all));
    _upscale_tuning = std::move(all);
  }
  post_("upscale.changed", JobId{}, {{"model", model}, {"values", kept}});
  return ok_status();
}

Json
Controller::upscale_tuning(const models::ModelEntry& m) const
{
  Json kept;
  {
    std::lock_guard lk(_upscale_tuning_mu);
    kept = jget(_upscale_tuning, m.id.c_str(), Json::object());
  }
  return models::resolve_tuning(m, tuning_context(m), kUpscalePreference,
                                false, kept);
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
      // None runs now: the first that will once downloaded -- the
      // feature itself is on its way, and its job waits for it.
      for (const auto& id : a.order) {
        const auto* m = _catalog.find(id);
        if (!a.chosen.empty()) {
          break;
        }
        if (m && runs_soon(*m, modality, op)) {
          a.chosen = id;
          a.waits = true;
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

const models::ModelEntry*
Controller::agent_model() const
{
  // A helper for this run (VALTZ_ASSISTANT, valtzctl --assistant) is the
  // agent's too: the 9B on a 24 GB Mac whatever is chosen.
  if (_agent && _assistant.empty()) {
    if (const auto* m = _catalog.find(_agent->model_choice());
        m && m->role == "assistant" &&
        _store->info(*m).state == models::InstallState::Installed) {
      return m;
    }
  }
  return assistant_model();
}

std::uint64_t
Controller::agent_memory_limit_(const models::ModelEntry& m) const
{
  if (!_agent) {
    return 0;
  }
  const int gb = _agent->memory_gb();
  const auto* a = gb > 0 ? agent_model() : nullptr;
  return a != nullptr && a->id == m.id
             ? static_cast<std::uint64_t>(gb) << 30
             : 0;
}

std::uint64_t
Controller::agent_expert_cache_(const models::ModelEntry& m) const
{
  if (!_agent || m.resident_gb <= 0) {
    return 0;
  }
  // GB as Settings shows them, and as the catalog sizes the model:
  // decimal (32 GB, not 34.4).
  const int gb = _agent->expert_cache_gb();
  const auto* a = gb > 0 ? agent_model() : nullptr;
  return a != nullptr && a->id == m.id
             ? static_cast<std::uint64_t>(gb) * 1'000'000'000
             : 0;
}

namespace {

// The working memory vpipe books beside a streamed model's context --
// a long prompt's piece, its picture encoder's scratch: DeepSeek's, ~7
// GB, read off its loads on the M5 Ultra (its run less its context at
// 131,072 and 1,048,576 tokens). A figure to size by, vpipe deciding at
// the load.
constexpr std::uint64_t kStreamedWorking = 7'000'000'000;

}  // namespace

Json
Controller::agent_memory_plan_() const
{
  const auto* m = agent_model();
  if (m == nullptr) {
    return nullptr;
  }
  const auto info = _store->info(*m);
  if (info.state != models::InstallState::Installed) {
    return nullptr;
  }
  auto lc = models::language_context(info.path());
  if (m->context_bytes_per_token > 0) {
    lc.kv_bytes = static_cast<std::uint64_t>(m->context_bytes_per_token);
  }
  const double disk = std::max(0.0, m->disk_gb);
  const bool streams = m->resident_gb > 0 && m->resident_gb < disk;
  const int here = helper_context_(*m);
  const double context_gb =
      static_cast<double>(here) * static_cast<double>(lc.kv_bytes) / 1e9;
  Json p = {{"model", m->id},
            {"name", m->name},
            {"streams", streams},
            {"disk_gb", disk},
            {"resident_gb", m->resident_gb},
            {"context", {{"model", lc.tokens},
                         {"auto", helper_context_(*m, false)},
                         {"fits", helper_context_fits_(*m)},
                         {"here", here},
                         {"kv_bytes", lc.kv_bytes}}},
            {"context_gb", context_gb}};
  if (!streams) {
    return p;
  }
  // Auto's cache as vpipe sizes it with nothing planned beside it: the
  // memory it sizes in (streamed_budget_) less its run -- its context and
  // the working memory of a long prompt's piece -- and its resident part.
  // A figure to choose by: vpipe decides at the load, beside what else its
  // plan holds then, and never under its floor (two layers' experts).
  const double mem = static_cast<double>(streamed_budget_(*m)) / 1e9;
  const double run =
      context_gb + static_cast<double>(kStreamedWorking) / 1e9;
  // What it reads from the SSD (its experts, and DeepSeek's n-gram rows,
  // which are never cached) and its experts alone, where the catalog says;
  // what it holds beside them.
  const double ssd = disk - m->resident_gb;
  const double experts = m->experts_gb > 0 ? m->experts_gb : ssd;
  const double floor = m->expert_cache_floor_gb;
  const double rest = m->held_beside_cache_gb();
  const double auto_cache =
      std::min(experts, std::max(floor, mem - run - rest));
  const double asked =
      static_cast<double>(agent_expert_cache_(*m)) / 1e9;
  const double cache =
      asked > 0 ? std::max(floor, std::min(asked, auto_cache)) : auto_cache;
  p["ssd_gb"] = ssd;
  p["rest_gb"] = rest;
  p["working_gb"] = static_cast<double>(kStreamedWorking) / 1e9;
  p["experts_gb"] = experts;
  p["floor_gb"] = floor;
  p["memory_gb"] = std::max(0.0, mem);
  p["cache_auto_gb"] = auto_cache;
  p["cache_gb"] = cache;
  p["all_resident"] = cache >= experts;
  p["holds_gb"] = rest + cache + run;
  return p;
}

void
Controller::rescan_models()
{
  _store->rescan();
  {
    std::lock_guard lk(_downloads_mu);
    _compress_est.clear();
  }
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

Json
Controller::download_sources() const
{
  return {{"sources", _engine->download_sources()},
          {"chosen", download_source_()}};
}

std::string
Controller::download_source_() const
{
  const Json offered = _engine->download_sources();
  const auto chosen = jget<std::string>(
      read_settings(_paths.support / "downloads.json"), "source", "");
  std::string fallback = "huggingface";
  for (const auto& s : offered) {
    const auto name = jget<std::string>(s, "name", "");
    if (name == chosen) {
      return name;
    }
    if (jget(s, "default", false)) {
      fallback = name;
    }
  }
  return fallback;
}

Status
Controller::set_download_source(const std::string& name)
{
  bool offered = false;
  for (const auto& s : _engine->download_sources()) {
    offered = offered || jget<std::string>(s, "name", "") == name;
  }
  if (!offered) {
    return make_error(Code::InvalidArgument, msg::kUnknownDownloadSource,
                      {{"source", name}});
  }
  const fs::path file = _paths.support / "downloads.json";
  Json all = read_settings(file);
  all["source"] = name;
  VALTZ_TRY(write_settings(file, all));
  post_("models.changed", JobId{});
  return ok_status();
}

Result<JobId>
Controller::download_model(const std::string& model_id,
                           const std::string& hf_token,
                           const std::string& source_in)
{
  const auto* e = _catalog.find(model_id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  if (!_engine->supports(engine::kOpFetchModel)) {
    return make_error(Code::Unsupported, msg::kNoEngineForDownload);
  }
  // Where from: the one named, else the one chosen -- passed over for
  // Hugging Face where vpipe knows it not to carry the repo.
  std::string source = source_in.empty() ? download_source_() : source_in;
  std::string label = source;
  bool offered = false;
  for (const auto& s : _engine->download_sources()) {
    if (jget<std::string>(s, "name", "") == source) {
      offered = true;
      label = jget<std::string>(s, "label", source);
    }
  }
  if (!offered && !source_in.empty()) {
    return make_error(Code::InvalidArgument, msg::kUnknownDownloadSource,
                      {{"source", source_in}});
  }
  if (offered && _engine->source_path(source, e->hf_path).empty()) {
    if (!source_in.empty()) {
      return make_error(Code::NotFound, msg::kNotOnDownloadSource,
                        {{"model", e->name}, {"source", label}});
    }
    VALTZ_LOG_INFO("models", "{} is not on {}: downloaded from Hugging "
                   "Face", e->name, label);
    source = "huggingface";
  }
  if (models_moving_()) {
    return make_error(Code::Busy, msg::kModelsMoving);
  }
  // Asked again while it downloads: that download.
  {
    std::lock_guard lk(_downloads_mu);
    if (auto it = _downloads.find(e->id); it != _downloads.end()) {
      return it->second;
    }
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
                 {"base_path", download_base_(*e).string()}};
  if (!e->fetch_variant.empty()) {
    spec.params["variant"] = e->fetch_variant;
  }
  if (!hf_token.empty()) {
    spec.params["hf_token"] = hf_token;
  }
  // Said always: Valtz's choice, not vpipe's $VPIPE_MODEL_SOURCE.
  if (offered || source == "huggingface") {
    spec.params["source"] = source;
  }

  JobRecord rec;
  rec.id = spec.id;
  rec.op = spec.op;
  rec.purpose = "download";
  rec.title = std::format("Download {}", e->name);
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  // On its way from now: what needs it waits for it -- before the
  // engine has it, which may end it at once.
  {
    std::lock_guard lk(_downloads_mu);
    _downloads[e->id] = spec.id;
  }
  post_("job.queued", spec.id, {{"title", rec.title}, {"op", rec.op},
                                {"purpose", rec.purpose},
                                {"model", e->id}});

  // Hugging Face's gate: another source answers in its own terms.
  const bool gated = e->gated && source == "huggingface";
  const std::string name = e->name;
  const std::string id = e->id;
  // Cancelled to be deleted (stop_download): its files go once the job
  // has let go of them -- unless it finished first, whole.
  const auto discarded = [this, id](bool finished) {
    bool drop = false;
    {
      std::lock_guard lk(_downloads_mu);
      drop = _discards.erase(id) > 0;
    }
    if (drop && !finished) {
      if (const auto* d = _catalog.find(id)) {
        (void)discard_download_(*d);
      }
    }
  };
  Status submitted = _engine->submit(
      std::move(spec),
      [this, gated, name, id, discarded](const engine::JobEvent& ev) {
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
      discarded(true);
      // Fetched again over a compressed copy (DESIGN §8b): the original
      // is back, the copy goes.
      for (const auto& root : _store->owned_roots()) {
        for (const int bits : {8, 4}) {
          std::error_code ec;
          const fs::path d = root / "compressed" /
                             std::format("{}-{}bit", id, bits);
          if (fs::exists(d / models::ModelStore::kCompressedMarker, ec)) {
            VALTZ_LOG_INFO("models", "{} downloaded: its {}-bit copy at "
                           "{} deleted", id, bits, d.string());
            fs::remove_all(d, ec);
          }
        }
      }
      _store->rescan();
      download_ended_(id, true, false);
      post_("job.finished", ev.job);
      post_("models.changed", JobId{});
      break;
    case engine::JobEventKind::Failed: {
      finish_job_(ev.job, JobState::Failed, ev.text);
      discarded(false);
      _store->rescan();
      download_ended_(id, false, false);
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
      discarded(false);
      _store->rescan();
      download_ended_(id, false, true);
      post_("job.cancelled", ev.job);
      post_("models.changed", JobId{});
      break;
    default:
      break;
    }
  });
  if (!submitted.ok()) {
    finish_job_(rec.id, JobState::Failed, submitted.error().message);
    download_ended_(id, false, false);
    return submitted.error();
  }
  if (const auto* d = _catalog.find(drafter);
      d && _store->info(*d).state != models::InstallState::Installed) {
    // From where the model came, where that carries it.
    const bool there = !_engine->source_path(source, d->hf_path).empty();
    (void)download_model(d->id, "", there ? source : "");
  }
  return rec.id;
}

bool
Controller::downloading(const std::string& id) const
{
  std::lock_guard lk(_downloads_mu);
  return _downloads.contains(id);
}

Status
Controller::stop_download(const std::string& model_id, bool discard)
{
  const auto* e = _catalog.find(model_id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  std::optional<JobId> job;
  {
    std::lock_guard lk(_downloads_mu);
    if (auto it = _downloads.find(e->id); it != _downloads.end()) {
      job = it->second;
      if (discard) {
        _discards.insert(e->id);
      }
    }
  }
  if (job) {
    VALTZ_LOG_INFO("models", "{}'s download {}", e->id,
                   discard ? "cancelled: its files go as it stops"
                           : "paused: its files kept");
    // A queued one ends at once, a running one at the fetch's next look
    // (a transfer's progress callback): the job's end deletes it.
    if (Status st = cancel(*job); !st.ok()) {
      std::lock_guard lk(_downloads_mu);
      _discards.erase(e->id);
      return st;
    }
    return ok_status();
  }
  if (!discard) {
    return make_error(Code::NotFound, msg::kNoDownloadRunning,
                      {{"model", e->name}});
  }
  // Paused before: nothing holds its files.
  (void)discard_download_(*e);
  _store->rescan();
  post_("models.changed", JobId{});
  return ok_status();
}

bool
Controller::coming_(const models::ModelEntry& e) const
{
  return _store->info(e).state == models::InstallState::Installed ||
         downloading(e.id);
}

fs::path
Controller::model_path_(const models::ModelEntry& e) const
{
  const auto info = _store->info(e);
  if (info.state == models::InstallState::Installed) {
    return info.path();
  }
  // Where its download puts it: <download root>/<hf_path>[/<subdir>],
  // its file in it.
  fs::path dir = download_base_(e) / e.hf_path;
  if (!e.subdir.empty()) {
    dir /= e.subdir;
  }
  return e.file.empty() ? dir : dir / e.file;
}

std::vector<std::string>
Controller::awaited_(const std::vector<std::string>& needs) const
{
  std::vector<std::string> out;
  for (const auto& id : needs) {
    const auto* e = _catalog.find(id);
    if (!e ||
        _store->info(*e).state == models::InstallState::Installed) {
      continue;
    }
    if (!downloading(id)) {
      return {};  // lacking, and not coming: nothing to wait for
    }
    out.push_back(id);
  }
  return out;
}

bool
Controller::waiting_(JobId job) const
{
  std::lock_guard lk(_downloads_mu);
  return _waiting.contains(job);
}

void
Controller::download_ended_(const std::string& model, bool ok, bool stopped)
{
  std::vector<Waiting> go;
  std::vector<Waiting> fail;
  {
    std::lock_guard lk(_downloads_mu);
    _downloads.erase(model);
    for (auto it = _waiting.begin(); it != _waiting.end();) {
      Waiting& w = it->second;
      if (!w.awaits.contains(model)) {
        ++it;
        continue;
      }
      w.awaits.erase(model);
      if (!ok) {
        fail.push_back(std::move(w));
      } else if (w.awaits.empty()) {
        go.push_back(std::move(w));
      } else {
        ++it;
        continue;
      }
      it = _waiting.erase(it);
    }
  }
  const auto* e = _catalog.find(model);
  const std::string name = e ? e->name : model;
  const auto failed = [](Waiting& w, Code code, Json fields) {
    engine::JobEvent ev;
    ev.job = w.spec.id;
    ev.kind = engine::JobEventKind::Failed;
    ev.error = code;
    ev.text = jget<std::string>(fields, "message", "");
    ev.data = std::move(fields);
    w.sink(ev);
  };
  for (auto& w : fail) {
    VALTZ_LOG_INFO("build", "job {}: {} did not download", w.spec.id.str(),
                   name);
    failed(w, stopped ? Code::Cancelled : Code::NotFound,
           message_fields(stopped ? msg::kAwaitedDownloadStopped
                                  : msg::kAwaitedDownloadFailed,
                          {{"model", name}}));
  }
  // Here now: its models resolved as they are on disk, and into the
  // engine's queue.
  for (auto& w : go) {
    Json helpers = Json::object();
    Status st = resolve_build_(w.model, w.params, w.spec.model, helpers);
    if (!st.ok()) {
      failed(w, st.code(), message_fields(st.error()));
      continue;
    }
    w.spec.params.update(helpers);
    VALTZ_LOG_INFO("build", "job {}: {} downloaded, queued",
                   w.spec.id.str(), name);
    const JobId id = w.spec.id;
    engine::JobSink sink = w.sink;
    if (Status sub = _engine->submit(std::move(w.spec), sink); !sub.ok()) {
      engine::JobEvent ev;
      ev.job = id;
      ev.kind = engine::JobEventKind::Failed;
      ev.error = sub.code();
      ev.text = sub.error().message;
      sink(ev);
    }
  }
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
  if (models_moving_()) {
    return make_error(Code::Busy, msg::kModelsMoving);
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

namespace {

// `p` inside `root` (or it).
bool
inside(const fs::path& p, const fs::path& root)
{
  const auto rel = p.lexically_normal().lexically_relative(
      root.lexically_normal());
  return !rel.empty() && *rel.begin() != "..";
}

// The passes a compressed copy is made in, as the engine takes them: a
// part's options, and at 4 bits its 4-bit ones over them.
Json
compress_parts(const models::ModelEntry& e, int bits)
{
  Json parts = Json::array();
  for (const auto& p : e.compress) {
    Json options = p.options;
    if (bits == 4) {
      options.update(p.options_4bit);
    }
    parts.push_back({{"target", p.target}, {"options", std::move(options)}});
  }
  return parts;
}

}

models::CompressEstimate
Controller::compress_estimate_(const models::ModelEntry& e,
                               const fs::path& dir, int bits) const
{
  const std::string key = std::format("{}|{}", dir.string(), bits);
  {
    std::lock_guard lk(_downloads_mu);
    if (auto it = _compress_est.find(key); it != _compress_est.end()) {
      return it->second;
    }
  }
  const auto est = models::estimate_compressed(dir, e.compress, bits);
  std::lock_guard lk(_downloads_mu);
  _compress_est[key] = est;
  return est;
}

Result<JobId>
Controller::compress_model(const std::string& model_id, int bits,
                           bool keep_original)
{
  const auto* e = _catalog.find(model_id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  if (e->compress.empty()) {
    return make_error(Code::InvalidArgument, msg::kNotCompressible,
                      {{"model", e->name}});
  }
  if (bits != 8 && bits != 4) {
    return make_error(Code::InvalidArgument, msg::kCompressBits);
  }
  if (models_moving_()) {
    return make_error(Code::Busy, msg::kModelsMoving);
  }
  {
    std::lock_guard lk(_downloads_mu);
    if (auto it = _compressing.find(e->id); it != _compressing.end()) {
      return it->second;
    }
  }
  const auto info = _store->info(*e);
  if (info.state != models::InstallState::Installed) {
    return make_error(Code::NotFound, msg::kModelNotInstalled,
                      {{"model", e->name}});
  }
  // A copy kept beside its original: one of the two kept first.
  if (const int b = std::max(info.compressed_bits, info.other_bits);
      b > 0 && !info.other_dir.empty()) {
    return make_error(Code::AlreadyExists, msg::kCompressedBesideOriginal,
                      {{"model", e->name}, {"bits", std::to_string(b)}});
  }
  if (info.compressed_bits > 0) {
    return make_error(Code::AlreadyExists, msg::kAlreadyCompressed,
                      {{"model", e->name},
                       {"bits", std::to_string(info.compressed_bits)}});
  }
  // Valtz's own download only: the copy replaces it, and a link's files
  // or vpipe's are not Valtz's to delete.
  if (info.linked || _store->own_root_of(info.dir).empty()) {
    return make_error(Code::InvalidArgument, msg::kCompressNotOurs,
                      {{"model", e->name}, {"path", info.dir.string()}});
  }
  if (!_engine->supports(engine::kOpQuantizeModel)) {
    return make_error(Code::Unsupported, msg::kEngineCannotRun,
                      {{"operation",
                        std::string(engine::kOpQuantizeModel)}});
  }
  // Both on disk while it runs: what the passes write (vpipe hard-links
  // the rest), and room to spare.
  const auto est = compress_estimate_(*e, info.dir, bits);
  constexpr std::uint64_t kSpare = 2'000'000'000ULL;
  std::error_code ec;
  const auto space = fs::space(_store->download_root(), ec);
  if (!ec && space.available < est.written + kSpare) {
    return make_error(Code::Io, msg::kCompressNoRoom,
                      {{"model", e->name},
                       {"need", models::size_text(est.written + kSpare)},
                       {"free", models::size_text(space.available)}});
  }
  // Written beside where it goes, under a name the store does not read,
  // and moved in whole once it passed its check.
  const fs::path dest = _store->compressed_dir(e->id, bits);
  const fs::path staging =
      dest.parent_path() / std::format(".{}.compressing",
                                       dest.filename().string());
  const std::size_t passes = e->compress.size();
  const auto clear = [staging, passes] {
    std::error_code ec;
    fs::remove_all(staging, ec);
    for (std::size_t i = 1; i <= passes; ++i) {
      fs::remove_all(std::format("{}.pass{}", staging.string(), i), ec);
    }
  };
  clear();
  fs::create_directories(dest.parent_path(), ec);

  engine::JobSpec spec;
  spec.id = JobId::make();
  spec.op = std::string(engine::kOpQuantizeModel);
  spec.model.id = e->id;
  spec.model.dir = info.dir;
  spec.params = {{"output", staging.string()},
                 {"bits", bits},
                 {"group_size", 64},
                 {"parts", compress_parts(*e, bits)}};

  JobRecord rec;
  rec.id = spec.id;
  rec.op = spec.op;
  rec.purpose = "compress";
  rec.title = std::format("Compress {} to {}-bit", e->name, bits);
  rec.destination = dest.string();
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  {
    std::lock_guard lk(_downloads_mu);
    _compressing[e->id] = spec.id;
  }
  post_("job.queued", spec.id, {{"title", rec.title}, {"op", rec.op},
                                {"purpose", rec.purpose},
                                {"model", e->id}, {"bits", bits},
                                {"keep_original", keep_original}});
  VALTZ_LOG_INFO("models", "compressing {} at {} to {} bits ({} parts) "
                 "into {}: about {} of {}{}", e->id, info.dir.string(),
                 bits, passes, dest.string(), models::size_text(est.total),
                 models::size_text(est.original),
                 keep_original ? ", the original kept" : "");

  const models::ModelEntry entry = *e;
  const fs::path original = info.dir;
  const auto ended = [this, id = e->id] {
    std::lock_guard lk(_downloads_mu);
    _compressing.erase(id);
  };
  Status submitted = _engine->submit(
      std::move(spec), [this, entry, bits, staging, original, clear,
                        ended, keep_original](const engine::JobEvent& ev) {
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
      std::uint64_t freed = 0;
      const std::string why = compress_finished_(
          entry, bits, staging, original, keep_original, &freed);
      clear();
      ended();
      if (!why.empty()) {
        finish_job_(ev.job, JobState::Failed, why);
        Json d = message_fields(msg::kCompressBadCopy,
                                {{"model", entry.name}, {"why", why}});
        d["code"] = "io";
        post_("job.failed", ev.job, d);
      } else {
        const fs::path dest = _store->compressed_dir(entry.id, bits);
        finish_job_(ev.job, JobState::Finished, dest.string());
        post_("job.finished", ev.job,
              {{"path", dest.string()}, {"model", entry.id},
               {"bits", bits}, {"freed", freed},
               {"kept_original", keep_original}});
      }
      post_("models.changed", JobId{}, {{"model", entry.id}});
      break;
    }
    case engine::JobEventKind::Failed:
      clear();
      ended();
      finish_job_(ev.job, JobState::Failed, ev.text);
      post_("job.failed", ev.job, {{"code", to_str(ev.error)},
                                   {"message", ev.text}});
      break;
    case engine::JobEventKind::Cancelled:
      clear();
      ended();
      finish_job_(ev.job, JobState::Cancelled, "");
      post_("job.cancelled", ev.job);
      break;
    default:
      break;
    }
  });
  if (!submitted.ok()) {
    clear();
    ended();
    finish_job_(rec.id, JobState::Failed, submitted.error().message);
    return submitted.error();
  }
  return rec.id;
}

std::string
Controller::compress_finished_(const models::ModelEntry& e, int bits,
                               const fs::path& staging,
                               const fs::path& original,
                               bool keep_original, std::uint64_t* freed)
{
  std::error_code ec;
  // The copy checked as a link is: the same model, whole. Until it is,
  // the download stays.
  const models::ModelCheck check = models::check_model(e, staging,
                                                       &_catalog);
  if (!check.usable || check.path != staging.lexically_normal()) {
    const std::string why = check.usable
        ? std::string("the copy was not where it was written")
        : jget<std::string>(check.error(), "message", "");
    VALTZ_LOG_ERROR("models", "compressed {} not kept: {}", e.id, why);
    return why;
  }
  const auto est = compress_estimate_(e, original, bits);
  {
    std::ofstream out(staging / models::ModelStore::kCompressedMarker);
    out << to_text({{"model", e.id},
                    {"bits", bits},
                    {"group_size", 64},
                    {"parts", compress_parts(e, bits)},
                    {"from", original.string()},
                    {"original_bytes", est.original},
                    {"original_kept", keep_original},
                    {"in_use", true},
                    {"made", project::now_ms()}}, 2) << "\n";
    if (!out) {
      return "its mark could not be written";
    }
  }
  const fs::path dest = _store->compressed_dir(e.id, bits);
  fs::remove_all(dest, ec);
  ec.clear();
  fs::rename(staging, dest, ec);
  if (ec) {
    return ec.message();
  }
  _store->rescan();
  const auto now = _store->info(e);
  if (now.compressed_bits != bits) {
    fs::remove_all(dest, ec);
    _store->rescan();
    return "the copy is not where the model is looked for";
  }
  // Kept, to be tried first: both on disk until one is kept
  // (keep_compressed).
  if (keep_original) {
    VALTZ_LOG_INFO("models", "{} compressed to {} bits: {} at {}, its "
                   "download kept at {}", e.id, bits,
                   models::size_text(now.bytes), dest.string(),
                   original.string());
    return "";
  }
  std::string kept_for;
  if (delete_original_(e, original, &kept_for)) {
    const std::uint64_t before = est.original;
    *freed = before > now.bytes ? before - now.bytes : 0;
  }
  _store->rescan();
  {
    std::lock_guard lk(_downloads_mu);
    _compress_est.clear();
  }
  VALTZ_LOG_INFO("models", "{} compressed to {} bits: {} at {}, {} freed",
                 e.id, bits, models::size_text(now.bytes), dest.string(),
                 models::size_text(*freed));
  return "";
}

bool
Controller::delete_original_(const models::ModelEntry& e,
                             const fs::path& original,
                             std::string* kept_for)
{
  for (const auto& o : _catalog.models()) {
    if (o.id == e.id) {
      continue;
    }
    const auto oi = _store->info(o);
    if (oi.state != models::InstallState::Missing &&
        (inside(oi.dir, original) || inside(original, oi.dir))) {
      VALTZ_LOG_INFO("models", "{}'s download kept: {} is in it", e.id,
                     o.id);
      *kept_for = o.name;
      return false;
    }
  }
  std::error_code ec;
  fs::remove_all(original, ec);
  if (ec) {
    VALTZ_LOG_ERROR("models", "{}'s download not deleted: {}", e.id,
                    ec.message());
    return false;
  }
  // Its folders left empty, up to the models folder it is in.
  const fs::path root = _store->own_root_of(original);
  for (fs::path up = original.parent_path();
       inside(up, root) && up != root && fs::is_empty(up, ec);
       up = up.parent_path()) {
    fs::remove(up, ec);
  }
  return true;
}

namespace {

// A compressed copy's mark with `fields` laid over it.
bool
remark_copy(const fs::path& dir, const Json& fields)
{
  const fs::path f = dir / models::ModelStore::kCompressedMarker;
  Json m;
  {
    std::ifstream in(f);
    m = in ? Json::parse(in, nullptr, false) : Json();
  }
  if (!m.is_object()) {
    return false;
  }
  m.update(fields);
  const fs::path tmp = f.string() + ".new";
  {
    std::ofstream out(tmp);
    out << to_text(m, 2) << "\n";
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  fs::rename(tmp, f, ec);
  return !ec;
}

}

Status
Controller::use_compressed(const std::string& model_id, bool compressed)
{
  const auto* e = _catalog.find(model_id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  const auto info = _store->info(*e);
  if (info.other_dir.empty()) {
    return make_error(Code::InvalidArgument, msg::kNotBothKept,
                      {{"model", e->name}});
  }
  // In use already as asked.
  if ((info.compressed_bits > 0) == compressed) {
    return {};
  }
  const fs::path copy = compressed ? info.other_dir : info.dir;
  if (!remark_copy(copy, {{"in_use", compressed}})) {
    return make_error(Code::Io, msg::kCompressBadCopy,
                      {{"model", e->name},
                       {"why", "its mark could not be written"}});
  }
  _store->rescan();
  VALTZ_LOG_INFO("models", "{}: its {} in use", e->id,
                 compressed ? "compressed copy" : "original");
  post_("models.changed", JobId{}, {{"model", e->id}});
  return {};
}

Result<std::uint64_t>
Controller::keep_compressed(const std::string& model_id, bool compressed)
{
  const auto* e = _catalog.find(model_id);
  if (!e) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model_id}});
  }
  const auto info = _store->info(*e);
  if (info.other_dir.empty()) {
    return make_error(Code::InvalidArgument, msg::kNotBothKept,
                      {{"model", e->name}});
  }
  // A job reading its files -- or about to -- keeps them.
  if (busy_here_()) {
    return make_error(Code::Busy, msg::kWeightsBusy, {{"model", e->name}});
  }
  const bool copy_in_use = info.compressed_bits > 0;
  const fs::path copy = copy_in_use ? info.dir : info.other_dir;
  const fs::path original = copy_in_use ? info.other_dir : info.dir;
  const std::uint64_t copy_bytes =
      copy_in_use ? info.bytes : info.other_bytes;
  const std::uint64_t original_bytes =
      copy_in_use ? info.other_bytes : info.bytes;
  std::uint64_t freed = 0;
  if (compressed) {
    // In use, and its original's place taken, as a compress that does
    // not keep it leaves them.
    if (!remark_copy(copy, {{"in_use", true}, {"original_kept", false}})) {
      return make_error(Code::Io, msg::kCompressBadCopy,
                        {{"model", e->name},
                         {"why", "its mark could not be written"}});
    }
    std::string kept_for;
    if (!delete_original_(*e, original, &kept_for)) {
      _store->rescan();
      post_("models.changed", JobId{}, {{"model", e->id}});
      return make_error(Code::Busy, msg::kOriginalShared,
                        {{"model", e->name}, {"other", kept_for}});
    }
    freed = original_bytes;
  } else {
    std::error_code ec;
    fs::remove_all(copy, ec);
    if (ec) {
      return make_error(Code::Io, msg::kCompressBadCopy,
                        {{"model", e->name}, {"why", ec.message()}});
    }
    // The compressed folder too, once nothing else is in it.
    fs::remove(copy.parent_path(), ec);
    freed = copy_bytes;
  }
  _store->rescan();
  {
    std::lock_guard lk(_downloads_mu);
    _compress_est.clear();
  }
  VALTZ_LOG_INFO("models", "{}: its {} kept, {} freed", e->id,
                 compressed ? "compressed copy" : "original",
                 models::size_text(freed));
  post_("models.changed", JobId{}, {{"model", e->id}});
  return freed;
}

std::uint64_t
Controller::discard_download_(const models::ModelEntry& e)
{
  const fs::path root = download_base_(e).lexically_normal();
  const fs::path repo = (root / e.hf_path).lexically_normal();
  // Never the models folder itself, nor anything outside it: a link's
  // files and vpipe's folders are not a download of Valtz's.
  if (e.hf_path.empty() || repo == root || !inside(repo, root)) {
    return 0;
  }
  fs::path own = repo;
  if (!e.subdir.empty()) {
    own = (own / e.subdir).lexically_normal();
  }
  const fs::path file = e.file.empty() ? fs::path() : own / e.file;
  const fs::path mine = file.empty() ? own : file;
  std::error_code ec;
  // Another model in `dir` (or `dir` in its place): its download going
  // there, or -- `files` -- its files there. One of the very same place
  // (an extension's model framed from the same weights) is this one.
  const auto taken = [&](const fs::path& dir, bool files) {
    for (const auto& o : _catalog.models()) {
      if (o.id == e.id || o.hf_path.empty()) {
        continue;
      }
      fs::path place = root / o.hf_path;
      if (!o.subdir.empty()) {
        place /= o.subdir;
      }
      if (!o.file.empty()) {
        place /= o.file;
      }
      place = place.lexically_normal();
      if (!inside(place, dir) && !inside(dir, place)) {
        continue;
      }
      if (downloading(o.id)) {
        return true;
      }
      fs::path part = place;
      part += ".part";
      if (files && place != mine &&
          (fs::exists(place, ec) || fs::exists(part, ec))) {
        return true;
      }
    }
    return false;
  };
  // Installed there already, and fetched over itself: only what the
  // fetch had not finished goes.
  const auto info = _store->info(e);
  const bool whole = info.state == models::InstallState::Installed &&
                     (inside(info.dir, repo) || inside(repo, info.dir));
  std::uint64_t freed = 0;
  const auto drop = [&](const fs::path& p) {
    if (!fs::exists(fs::symlink_status(p, ec))) {
      return;
    }
    const std::uint64_t n = folder_bytes(p);
    fs::remove_all(p, ec);
    if (ec) {
      VALTZ_LOG_ERROR("models", "{} not deleted: {}", p.string(),
                      ec.message());
    } else {
      freed += n;
    }
  };
  std::string what;
  if (!whole && !taken(repo, true)) {
    what = repo.string();
    drop(repo);
    // Its folders left empty, up to the models folder.
    for (fs::path up = repo.parent_path();
         inside(up, root) && up != root && fs::is_empty(up, ec);
         up = up.parent_path()) {
      fs::remove(up, ec);
    }
  } else if (!file.empty()) {
    what = file.string();
    fs::path part = file;
    part += ".part";
    drop(part);
    if (!whole) {
      drop(file);
    }
  } else if (!whole && own != repo && !taken(own, true)) {
    what = own.string();
    drop(own);
  } else if (!taken(own, false)) {
    // Only its partial files: the whole ones may be another's too.
    what = "the partial files in " + own.string();
    std::vector<fs::path> parts;
    for (auto it = fs::recursive_directory_iterator(
             own, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      if (it->path().extension() == ".part") {
        parts.push_back(it->path());
      }
    }
    for (const auto& p : parts) {
      drop(p);
    }
  }
  VALTZ_LOG_INFO("models", "{}'s download deleted: {} ({} freed)", e.id,
                 what.empty() ? std::string("nothing") : what,
                 models::size_text(freed));
  return freed;
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
  // An agent at work on it: the person's writes refused (agent_hold_).
  w->project().env().set_guard(
      [this, id, w] { return write_guard_(id, *w); });
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

namespace {

// The core's own writes on this thread (Controller::CoreWrites).
thread_local int t_core_writes = 0;

}  // namespace

Controller::CoreWrites::CoreWrites() { ++t_core_writes; }
Controller::CoreWrites::~CoreWrites() { --t_core_writes; }

void
Controller::agent_hold_(ProjectId pid, const std::string& session,
                        bool held)
{
  bool changed = false;
  {
    std::lock_guard lk(_agent_holds_mu);
    auto& sessions = _agent_holds[pid];
    const bool was = !sessions.empty();
    if (held) {
      sessions.insert(session);
    } else {
      sessions.erase(session);
    }
    changed = was != !sessions.empty();
    if (sessions.empty()) {
      _agent_holds.erase(pid);
    }
  }
  // The app's read-only state follows project.changed's "agent".
  if (changed) {
    post_state_(pid);
  }
}

bool
Controller::agent_holds_(ProjectId pid) const
{
  std::lock_guard lk(_agent_holds_mu);
  auto it = _agent_holds.find(pid);
  return it != _agent_holds.end() && !it->second.empty();
}

Status
Controller::write_guard_(ProjectId pid, project::Workspace& w) const
{
  // The core's own, or in no command (a view, the playhead, the guides).
  if (t_core_writes > 0 || w.undo().recording() == 0 ||
      !agent_holds_(pid)) {
    return ok_status();
  }
  return make_error(Code::Busy, msg::kAgentAtWork);
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
          // An agent at work on it: read-only to the person.
          {"agent", agent_holds_(pid)},
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
  if (agent_holds_(pid)) {
    return make_error(Code::Busy, msg::kAgentAtWork);
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
  // An agent at work: its steps are not taken back under it (undo
  // writes unjournaled, so the write guard does not see it).
  if (agent_holds_(pid)) {
    return make_error(Code::Busy, msg::kAgentAtWork);
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
  if (agent_holds_(pid)) {
    return make_error(Code::Busy, msg::kAgentAtWork);
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
    (void)cancel(job);
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
  // Those waiting for downloads, and the names of what they wait for;
  // those held for an input being made, and its name.
  std::map<JobId, Json> waits;
  std::map<JobId, Json> held;
  {
    std::lock_guard lk(_downloads_mu);
    for (const auto& [id, w] : _waiting) {
      Json names = Json::array();
      for (const auto& m : w.awaits) {
        const auto* e = _catalog.find(m);
        names.push_back(e ? e->name : m);
      }
      waits[id] = std::move(names);
    }
    for (const auto& [id, h] : _held) {
      held[id] = h.names;
    }
  }
  // The order the engine takes them: the running ones, then as asked
  // (an id is made in time order) -- and last those waiting for a
  // download, which the engine has not been given.
  std::ranges::sort(live, [&](const JobRecord& a, const JobRecord& b) {
    const bool ra = a.state == JobState::Running;
    const bool rb = b.state == JobState::Running;
    if (ra != rb) {
      return ra;
    }
    const bool wa = waits.contains(a.id) || held.contains(a.id);
    const bool wb = waits.contains(b.id) || held.contains(b.id);
    if (wa != wb) {
      return wb;
    }
    if (a.created_ms != b.created_ms) {
      return a.created_ms < b.created_ms;
    }
    return a.id.str() < b.id.str();
  });
  Json out = Json::array();
  // Positions count per LINE: each Mac runs one task at a time, and its
  // exports one at a time beside it (the engine's export lane). One
  // waiting for a download is in no line yet.
  std::map<std::string, int> positions;
  for (const auto& j : live) {
    const std::string runner = j.runner.empty() ? "local" : j.runner_name;
    const auto wait = waits.find(j.id);
    const auto hold = held.find(j.id);
    const bool upscale = j.op == engine::kOpUpscaleVideo ||
                         j.op == engine::kOpUpscaleImage;
    const bool exp = j.purpose == "export";
    const std::string line = exp && j.runner.empty() ? runner + " exports"
                                                      : runner;
    // An asset operation's (asset-ops.h): its name.
    const AssetOperation* aop = asset_operation_of(j.op);
    Json t = {{"job", j.id.str()},
              {"project", j.project.str()},
              {"asset", exp ? std::string() : j.asset.str()},
              {"kind", exp ? "export" : upscale ? "upscale" : "generate"},
              {"operation", aop ? std::string(aop->name) : std::string()},
              {"op", j.op},
              {"title", j.title},
              {"state", to_str(j.state)},
              {"position", wait != waits.end() || hold != held.end()
                               ? positions[line]
                               : positions[line]++},
              {"runner", runner},
              {"progress", j.progress}};
    if (wait != waits.end()) {
      t["waiting"] = wait->second;
    }
    if (hold != held.end()) {
      t["waiting_for"] = hold->second;
    }
    // Every asset it makes (a separation's sources), its own first.
    if (!exp) {
      Json all = Json::array({j.asset.str()});
      for (const auto& c : j.companions) {
        all.push_back(c.second.str());
      }
      t["assets"] = std::move(all);
    }
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
  // Clips, their turn read where a record from before does not say it
  // (2026-10-24): a phone's portrait clip was recorded landscape.
  std::vector<AssetId> clips;
  for (const auto& a : assets) {
    if (a.linked) {
      linked.push_back(a.id);
    }
    if (a.kind == project::AssetKind::Video &&
        a.cls == project::AssetClass::Flat) {
      clips.push_back(a.id);
    }
  }
  JobId jid = JobId::make();
  if (linked.empty() && clips.empty()) {
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
  _bg.emplace_back([this, p, pid, jid, linked, clips] {
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
    for (const auto& id : clips) {
      if (_shutting_down.load()) {
        break;
      }
      if (auto r = p->read_turns(id); !r.ok()) {
        VALTZ_LOG_WARN("link", "{}: {}", id.str(), r.error().message);
      } else if (*r) {
        post_("assets.changed", jid,
              {{"project", pid}, {"asset", id}, {"reason", "turn"}});
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
  // A timeline's, of its frame on black and its compositions' movies in
  // Core Video's 709 (2026-10-12): one cached of its frame drawn clear --
  // white -- or lighter is not found.
  const bool timeline = a.cls == project::AssetClass::Composition &&
                        a.kind == project::AssetKind::Video;
  const std::string key = cache::CacheStore::key_for(
      content, std::format("thumb-{}{}.jpg", max_px,
                           timeline ? "-black-709" : ""));
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
  // Its agent sessions stopped: they work in it.
  if (_agent) {
    _agent->project_closing(id);
  }
  // Its jobs stopped -- a queued one dropped, a running one asked to
  // stop: nothing works on a project that is gone. They ran on before,
  // holding the machine, their results dropped (2026-10-15). The app
  // asks first and waits for them; this is for any close.
  for (const auto& j : _jobs.list()) {
    if (j.project == id &&
        (j.state == JobState::Queued || j.state == JobState::Running)) {
      VALTZ_LOG_INFO("jobs", "job {}: its project closed, stopped",
                     j.id.str());
      (void)cancel(j.id);
    }
  }
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
Controller::set_assistant_context(std::int64_t tokens)
{
  tokens = tokens > 0 ? std::max<std::int64_t>(tokens, 16384) : 0;
  const fs::path file = _paths.support / "assistant.json";
  Json j = read_settings(file);
  if (!j.is_object()) {
    j = Json::object();
  }
  j["context"] = tokens;
  VALTZ_TRY(write_settings(file, j));
  _assistant_context = tokens;
  post_("assistant.changed", JobId{}, {{"context", tokens}});
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

Status
Controller::set_video_every(double seconds)
{
  seconds = seconds > 0 ? std::clamp(seconds, 0.125, 60.0) : 0.0;
  const fs::path file = _paths.support / "assistant.json";
  Json j = read_settings(file);
  if (!j.is_object()) {
    j = Json::object();
  }
  j["video_every"] = seconds;
  VALTZ_TRY(write_settings(file, j));
  _video_every = seconds;
  post_("assistant.changed", JobId{}, {{"video_every", seconds}});
  return ok_status();
}

double
Controller::video_every(double clip_seconds) const
{
  if (_video_every > 0) {
    return _video_every;
  }
  const bool slow = _hw.memory_bandwidth_gbs > 0 &&
                    _hw.memory_bandwidth_gbs < kFastMemoryGbs;
  const double floor = slow ? 2.0 : 1.0;
  // The densest of the offered rates that reads the clip in about its
  // frames (a tenth over: a "10 s" clip runs 10.04).
  const double frames = (slow ? 0.5 : 1.0) * kSummaryFrames * 1.1;
  if (clip_seconds > 0) {
    for (const double every : {0.125, 0.25, 0.5, 1.0}) {
      if (every < floor && clip_seconds / every <= frames) {
        return every;
      }
    }
  }
  return floor;
}

media::PixelSize
Controller::video_frame_most(std::string_view level)
{
  if (level == "highest") {
    return {1152, 640};
  }
  if (level == "high") {
    return {864, 480};
  }
  return {576, 320};
}

Status
Controller::set_video_size(const std::string& level)
{
  if (level != "standard" && level != "high" && level != "highest") {
    return make_error(Code::InvalidArgument, std::format(
        "a video summary's frame size is standard, high or highest, not "
        "'{}'", level));
  }
  const fs::path file = _paths.support / "assistant.json";
  Json j = read_settings(file);
  if (!j.is_object()) {
    j = Json::object();
  }
  j["video_size"] = level;
  VALTZ_TRY(write_settings(file, j));
  _video_size = level;
  post_("assistant.changed", JobId{}, {{"video_size", level}});
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
    // pick_assistant's measure.
    const bool fits = models::fits_as_assistant(*e, _hw);
    Json row = {{"id", e->id},
                {"name", e->name},
                {"state", models::to_str(_store->info(*e).state)},
                {"fits", fits},
                {"disk_gb", e->disk_gb},
                {"min_ram_gb", e->min_ram_gb},
                {"rank", e->rank},
                {"by_name", e->by_name},
                {"mtp", jget(vp, "mtp", true)},
                {"sampling", jget(vp, "sampling", Json::object())}};
    // Its context: its own, what this Mac's memory holds (Auto), and
    // what it is given (no more than Settings' Context); installed only.
    if (const auto lc = models::language_context(_store->info(*e).path());
        lc.tokens > 0) {
      row["context"] = {{"model", lc.tokens},
                        {"auto", helper_context_(*e, false)},
                        {"fits", helper_context_fits_(*e)},
                        {"here", helper_context_(*e)},
                        {"kv_bytes", e->context_bytes_per_token > 0
                                         ? static_cast<std::uint64_t>(
                                               e->context_bytes_per_token)
                                         : lc.kv_bytes}};
    }
    if (const auto* d = _catalog.find(
            jget<std::string>(vp, "mtp_drafter", ""))) {
      row["drafter"] = {{"id", d->id},
                        {"name", d->name},
                        {"state", models::to_str(_store->info(*d).state)}};
    }
    if (const auto* d = _catalog.find(
            jget<std::string>(vp, "dflash_drafter", ""))) {
      // What it holds beside the model at 8 and 4 bits (catalog
      // engine.vpipe.held_gb): the Drafter pop-up's sizes.
      row["dflash"] = {{"id", d->id},
                       {"name", d->name},
                       {"state", models::to_str(_store->info(*d).state)},
                       {"disk_gb", d->disk_gb},
                       {"held_gb", jget(jget(d->engine, "vpipe",
                                             Json::object()),
                                        "held_gb", Json::object())}};
    }
    list.push_back(std::move(row));
  }
  return {{"choice", _assistant_choice},
          {"auto", pick ? pick->id : std::string()},
          {"using", in_use ? in_use->id : std::string()},
          {"keep_loaded", _assistant_keep},
          {"drafter", _assistant_drafter},
          {"drafter_bits", _assistant_drafter_bits},
          {"context", _assistant_context},
          {"video_every", _video_every},
          {"video_every_now", video_every()},
          {"video_size", _video_size},
          {"memory_bandwidth_gbs", _hw.memory_bandwidth_gbs},
          {"models", std::move(list)}};
}

std::string
Controller::ReplyForm::apply(std::string prompt) const
{
  if (!lead.empty() && !prompt.empty()) {
    // "{last_shot}": the number of the reply's final [Shot N].
    std::string l = lead;
    if (const auto at = l.find("{last_shot}"); at != std::string::npos) {
      int last = 1;
      for (auto s = prompt.find("[Shot "); s != std::string::npos;
           s = prompt.find("[Shot ", s + 1)) {
        last = std::max(last, std::atoi(prompt.c_str() + s + 6));
      }
      l.replace(at, 11, std::to_string(last));
    }
    prompt = l + "\n\n" + prompt;
  }
  return assist::retag(prompt, names);
}

Result<engine::JobSpec>
Controller::chat_spec_(JobId id, std::string text,
                       std::vector<engine::JobInput> images,
                       int max_new_tokens) const
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
  // Without thinking, decoded as the helper is set to.
  spec.params = helper_params_(*m);
  spec.params["text"] = std::move(text);
  spec.params["disable_thinking"] = true;
  spec.params["max_new_tokens"] = max_new_tokens;
  spec.inputs = std::move(images);
  return spec;
}

Json
Controller::helper_params_(const models::ModelEntry& m) const
{
  // Decoded with the model's own sampler (catalog engine.vpipe.sampling:
  // Qwen's non-thinking recommendation); MTP drafts under it as it does
  // greedy. Kept loaded as Settings say.
  const Json vp = jget(m.engine, "vpipe", Json::object());
  Json p = {{"mtp", assistant_mtp(m)},
            {"sampling", jget(vp, "sampling", Json::object())},
            {"keep_loaded", _assistant_keep}};
  // A long prompt's GEMMs in int8 where the catalog asks (engine.vpipe.
  // i8_prefill: Flash-Next's): a prefill of 1,024 rows or more, lossy as
  // a prefill may be; a decode step and a verify untouched.
  if (jget(vp, "i8_prefill", false)) { p["i8_prefill"] = true; }
  // The agent's model held within the agent's Memory (vpipe's
  // memory_limit_mb, DESIGN §15 0zzd): its expert cache what the limit
  // leaves, every use of it alike -- a model kept warm is handed over at
  // the size it loaded at.
  if (const auto lim = agent_memory_limit_(m); lim > 0) {
    p["memory_limit_mb"] = lim >> 20;
  }
  // And its experts' cache as the Agent's Expert Cache asks (vpipe's
  // expert_cache_mb): at most that, within the Memory and the Mac.
  if (const auto c = agent_expert_cache_(m); c > 0) {
    p["expert_cache_mb"] = c >> 20;
  }
  // Its whole context where this Mac holds it (Qwen3.5's and 3.8's
  // 262,144): an agent's long session runs to the model's limit, not
  // ours. Its K/V grows as it fills.
  if (const int n = helper_context_(m); n > 0) {
    p["context_tokens"] = n;
  }
  // A model whose MTP head ships apart (engine.vpipe.mtp_drafter): the
  // drafter's directory, when it is installed.
  if (assistant_mtp(m)) {
    if (const auto* d = _catalog.find(
            jget<std::string>(vp, "mtp_drafter", ""));
        d && _store->info(*d).state == models::InstallState::Installed) {
      p["mtp_model"] = _store->info(*d).path().string();
    }
  }
  // DFlash 2 when chosen and here (it takes over from MTP in text-chat);
  // else MTP, as above.
  if (_assistant_drafter == "dflash") {
    if (const auto* d = _catalog.find(
            jget<std::string>(vp, "dflash_drafter", ""));
        d && _store->info(*d).state == models::InstallState::Installed) {
      p["draft_model"] = _store->info(*d).path().string();
      p["draft_bits"] = _assistant_drafter_bits;
    }
  }
  return p;
}

std::uint64_t
Controller::streamed_budget_(const models::ModelEntry& m) const
{
  constexpr std::uint64_t kHeldBack = std::uint64_t{8} << 30;
  std::uint64_t b = _hw.ram_bytes > kHeldBack ? _hw.ram_bytes - kHeldBack
                                              : 0;
  if (_hw.gpu_working_set_bytes > 0) {
    b = std::min(b, _hw.gpu_working_set_bytes / 100 * 85);
  }
  if (const auto lim = agent_memory_limit_(m); lim > 0) {
    b = std::min(b, lim);
  }
  return b;
}

int
Controller::helper_context_fits_(const models::ModelEntry& m) const
{
  // The memory it is counted in: the Mac's, or the agent's Memory.
  std::uint64_t ram = _hw.ram_bytes;
  if (const auto lim = agent_memory_limit_(m); lim > 0) {
    ram = std::min(ram, lim);
  }
  auto lc = models::language_context(_store->info(m).path());
  // A token as vpipe books it where its config cannot say (DeepSeek's
  // ~7.9 KB -- its compressed latents and its indexer's scores -- not the
  // 80 KB its heads read as).
  if (m.context_bytes_per_token > 0) {
    lc.kv_bytes = static_cast<std::uint64_t>(m.context_bytes_per_token);
  }
  const std::uint64_t asked = agent_expert_cache_(m);
  const auto rest =
      static_cast<std::uint64_t>(m.held_beside_cache_gb() * 1e9);
  const auto floor =
      static_cast<std::uint64_t>(m.expert_cache_floor_gb * 1e9);
  // A context booked as the catalog says (DeepSeek's): vpipe books it
  // whole at the load, before its cache, and takes it from the cache --
  // never under the floor. So the most that fits is what the memory it
  // sizes in leaves past what it holds beside its cache, the cache (the
  // one asked for, kept whole; Auto's, down to its floor) and the working
  // memory: no share held back, the trade being the cache's.
  if (m.resident_gb > 0 && m.context_bytes_per_token > 0) {
    constexpr std::uint64_t kPage = 16;
    constexpr std::uint64_t kLeast = 32768;
    if (lc.tokens <= 0) {
      return 0;
    }
    const std::uint64_t budget = streamed_budget_(m);
    const std::uint64_t used =
        rest + std::max(floor, asked) + kStreamedWorking;
    const std::uint64_t left = budget > used ? budget - used : 0;
    const std::uint64_t n = left / lc.kv_bytes / kPage * kPage;
    return static_cast<int>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(lc.tokens), std::max(n, kLeast)));
  }
  // Else what it holds beside its context: its size on disk -- a Mixture
  // of Experts streaming its experts keeps its cache first, sized after
  // the context (Flash-Next, whose K/V is a dense model's) -- or, with a
  // cache asked for, what it holds beside the cache and the cache, in the
  // memory vpipe sizes it in (as much of it as the budget leaves). The
  // context gets its share of what they leave.
  std::uint64_t held =
      static_cast<std::uint64_t>(std::max(0.0, m.disk_gb) * 1e9);
  if (m.resident_gb > 0 && asked > 0) {
    ram = streamed_budget_(m);
    const std::uint64_t used = rest + kStreamedWorking;
    const std::uint64_t room = ram > used ? ram - used : 0;
    held = rest + std::max(floor, std::min(asked, room));
  }
  return models::helper_context(lc, ram, held);
}

int
Controller::helper_context_(const models::ModelEntry& m, bool chosen) const
{
  const int fits = helper_context_fits_(m);
  int n = fits;
  // Auto's, where the model's own is far past what a session needs and
  // the room it takes is the cache's (DeepSeek V4.1 Flash's 131,072 of
  // 1,048,576): no more than the catalog's. Chosen, up to what fits.
  if (m.context_tokens > 0) {
    n = n > 0 ? std::min(m.context_tokens, n) : m.context_tokens;
  }
  // Settings' Context: what fits at most -- more than Auto's where Auto
  // is held below it.
  if (chosen && _assistant_context > 0 && fits > 0) {
    n = static_cast<int>(std::min<std::int64_t>(fits, _assistant_context));
  }
  // VALTZ_HELPER_CONTEXT=<tokens>: less for this run -- an agent's
  // compaction reached by a short session.
  if (const char* v = std::getenv("VALTZ_HELPER_CONTEXT"); v && *v) {
    if (const long long t = std::atoll(v); t > 0 && n > 0) {
      n = static_cast<int>(std::min<long long>(n, t));
    }
  }
  return n;
}

Result<JobId>
Controller::submit_chat_(JobId id, std::string purpose, std::string text,
                         std::vector<engine::JobInput> images,
                         int max_new_tokens, std::string song,
                         ReplyForm form)
{
  // Written by this Mac's assistant -- or, in a fleet, by a member's
  // when this Mac has none or is busy (DESIGN §11).
  auto spec = chat_spec_(id, text, images, max_new_tokens);
  const bool here = spec.ok() && _engine->supports(engine::kOpChat);
  auto members = fleet_candidates_(std::string(engine::kOpChat), {}, here);
  const bool fleet_only =
      offload_policy_(engine::kOpChat, "") == fleet::kOffloadAlways;
  if (fleet_only && members.empty()) {
    return make_error(Code::Unsupported, msg::kFleetOnlyNoMember);
  }
  if (members.empty() && !spec.ok()) {
    return spec.error();
  }
  if (members.empty() && !here) {
    return make_error(Code::Unsupported, msg::kEngineCannotRun,
                      {{"operation", std::string(engine::kOpChat)}});
  }

  JobRecord rec;
  rec.id = id;
  rec.op = std::string(engine::kOpChat);
  rec.purpose = purpose;
  rec.title = purpose == "enhance" ? "Enhance prompt" : "Understand request";
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  post_("job.queued", id, {{"title", rec.title}, {"op", rec.op},
                           {"purpose", purpose}});
  // What the assistant has written so far, and the suggestion read out of
  // it each time it grows (assist.partial): the person watches it come
  // rather than waiting for the whole reply. And its speed, as an agent's
  // turn's (agent::turn_stats): from the request, its first words.
  struct Written {
    std::mutex mu;
    std::string text;
    std::string shown;
    std::chrono::steady_clock::time_point asked =
        std::chrono::steady_clock::now();
    double first_ms = -1;
  };
  auto written = std::make_shared<Written>();
  engine::JobSink sink = [this, purpose, song = std::move(song),
                          form = std::move(form),
                          written](const engine::JobEvent& ev) {
    const auto ms = [&] {
      return std::chrono::duration<double, std::milli>(
                 std::chrono::steady_clock::now() - written->asked)
          .count();
    };
    if (ev.kind == engine::JobEventKind::Text) {
      std::lock_guard lk(written->mu);
      if (written->first_ms < 0) {
        written->first_ms = ms();
      }
    }
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
    Json stats = Json::object();
    if (ev.kind == engine::JobEventKind::Output) {
      std::lock_guard lk(written->mu);
      stats = agent::turn_stats(ev.data, written->first_ms, ms());
    }
    on_chat_event_(ev, purpose, song, form, stats);
  };
  if (members.empty()) {
    VALTZ_TRY(_engine->submit(std::move(*spec), std::move(sink)));
    return id;
  }
  fleet::Offer offer{id, std::string(engine::kOpChat), "", rec.title,
                     {{"text", text}, {"max_new_tokens", max_new_tokens}},
                     images};
  std::optional<engine::JobSpec> local;
  if (here && !fleet_only) {
    local = std::move(*spec);
  }
  VALTZ_TRY(run_remote_(std::move(members), std::move(offer),
                        fs::temp_directory_path(), std::move(local),
                        std::move(sink)));
  return id;
}

void
Controller::on_chat_event_(const engine::JobEvent& ev,
                           const std::string& purpose,
                           const std::string& song, const ReplyForm& form,
                           const Json& stats)
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
      if (form.canvas.first > 0) {
        // A MOSS-Image draft -- a caption, or a layout -- compiled for
        // the picture's canvas, as its skill's script compiles it; a
        // reply in another shape (a helper ignoring the skill) read as
        // any other.
        if (auto j = assist::extract_json_object(ev.text); j.ok()) {
          auto p = assist::moss_image_prompt(*j, form.canvas.first,
                                             form.canvas.second);
          if (p.ok()) {
            assist::EnhanceResult e;
            e.prompt = std::move(*p);
            e.title = jget<std::string>(*j, "title", "");
            r = std::move(e);
          } else if (!r.ok() || r->prompt.empty()) {
            r = p.error();
          }
        }
      }
      if (r.ok()) {
        if (!song.empty()) {
          r->prompt = assist::keep_song_lyrics(r->prompt, song);
        }
        r->prompt = form.apply(std::move(r->prompt));
        Json d = Json(*r);
        if (!stats.empty()) {
          d["stats"] = stats;
        }
        post_("assist.enhanced", ev.job, std::move(d));
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
  case engine::JobEventKind::Failed: {
    finish_job_(ev.job, JobState::Failed, ev.text);
    Json d = {{"code", to_str(ev.error)}, {"message", ev.text}};
    if (const auto key = jget<std::string>(ev.data, "key", "");
        !key.empty()) {
      d["key"] = key;
      d["args"] = jget(ev.data, "args", Json::object());
    }
    post_("job.failed", ev.job, d);
    break;
  }
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
  int voices = 0;
  if (req.target == "audio" && req.project) {
    if (project::Project* vp = project(*req.project)) {
      for (const auto& id : req.row.empty() ? req.references : req.row) {
        const bool v = voiced_(*vp, id);
        voice = voice || v;
        voices += v;
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
    if (req.last) {
      row.push_back(*req.last);
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
  // A script of several speakers (Drama 3): how many voices it is given.
  if (er.speech && jget(jget(jget(m->engine, "vpipe", Json::object()),
                             "speech", Json::object()),
                        "speakers", 1) > 1) {
    er.speakers = true;
    er.voices = voices;
  }
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
  // MOSS-Image's skill has an agent write a JSON draft and run a script
  // on it: here the helper replies with the draft, and Valtz compiles it
  // (on_chat_event_) for the picture's own canvas -- the size asked for,
  // else the model's -- since a layout's boxes are that picture's pixels.
  ReplyForm form;
  if (skill == "moss-image" && !er.system.empty()) {
    int w = 0, h = 0;
    if (std::sscanf(req.size.c_str(), "%dx%d", &w, &h) != 2 || w <= 0 ||
        h <= 0) {
      const Json d = jget(jget(m->engine, "vpipe", Json::object()),
                          "defaults", Json::object());
      w = jget(d, "width", 1024);
      h = jget(d, "height", 1024);
    }
    auto grid = [](int v) { return std::max(16, (v + 8) / 16 * 16); };
    form.canvas = {grid(w), grid(h)};
    er.skill_note = std::format(
        "Valtz runs this skill for you, and there are no files, no shell "
        "and no script here: reply with the draft ITSELF and nothing else "
        "-- one JSON object, no prose, no code fence. A caption: "
        "{{\"caption\": \"...\"}}. A Layout: the draft as "
        "references/layout.md describes it, with \"width\": {0}, "
        "\"height\": {1} -- the picture is made at exactly {0} x {1} "
        "pixels, so every box is drawn on that canvas. Valtz checks and "
        "compiles it as scripts/compile_prompt.py does; what it makes is "
        "MOSS-Image's prompt.",
        form.canvas.first, form.canvas.second);
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
  const int tokens = clip_skill || form.canvas.first > 0 ? 4096
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
  auto is_picture = [&](AssetId id) {
    auto a = p->asset(id);
    return a.ok() && a->kind == project::AssetKind::Image;
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
  } else if (p && req.base && req.last &&
             m.has(models::Capability::ImageToVideo) &&
             jget(vp, "first_frame", false) &&
             jget(vp, "last_frame", false) && is_picture(*req.base) &&
             is_picture(*req.last)) {
    er.video_mode = "FL2VA";
    // The guide's FL2VA instruction, word for word -- its pictures bare,
    // as that case names them; the last shot's number is the reply's
    // (ReplyForm::apply).
    const std::string named = keyframe_tag(m, true);
    const std::string tags[] = {numbered(named, 1), numbered(named, 2)};
    form.lead = std::format(
        "How the reference pictures align with the target video — {} "
        "(from Shot 1) aligns with the 0.00-second mark of the target "
        "video; {} (from Shot {{last_shot}}) aligns with the {:.2f}-second "
        "mark of the target video.",
        tags[0], tags[1], er.seconds);
    const AssetId ends[] = {*req.base, *req.last};
    for (int i = 0; i < 2; ++i) {
      tag_of[ends[i].str()] = tags[i];
      if (const auto t = row_tag(ends[i].str()); !t.empty()) {
        names.emplace_back(tags[i], t);
      }
      std::string line = std::format(
          "{}: the picture the video {}, \"{}\"", tags[i],
          i == 0 ? "opens on (its first frame)"
                 : "ends on (its last frame)",
          p->asset(ends[i])->name);
      if (show(ends[i])) {
        line += std::format(" -- image {} shown above", images.size());
      }
      er.references.push_back(std::move(line));
    }
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
                            std::optional<AssetId> from,
                            const std::string& person)
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
  const std::string name = words_name(text);
  // The person's name for it, if they gave one: the prompt it is is
  // named so, whichever it is.
  const std::string given(utf8_prefix(one_line(person), 200));
  auto give = [&](const project::Asset& a) -> Status {
    if (given.empty() || (a.name == given && is_named(a))) {
      return ok_status();
    }
    VALTZ_TRY(p.update_asset(a.id, [&](project::Asset& r) {
      r.name = given;
      set_named(r, true);
      return ok_status();
    }));
    told(a.id, "renamed");
    return ok_status();
  };
  if (from) {
    auto a = p.asset(*from);
    if (a.ok() && is_prompt(*a)) {
      if (auto cur = p.read_text(*from); cur.ok() && *cur == text) {
        VALTZ_TRY(give(*a));
        return pinned(*from);
      }
      // Nothing made from it yet: it is still a draft, changed in place
      // -- named by its words, unless the person named it.
      if (auto deps = p.dependents(*from, false); deps.ok() &&
                                                  deps->empty()) {
        VALTZ_TRY(retext_prompt_(p, *from, text));
        if (given.empty() && !is_named(*a)) {
          (void)p.rename_asset(*from, name);
        }
        VALTZ_TRY(give(*a));
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
        VALTZ_TRY(give(a));
        return pinned(a.id);
      }
    }
  }
  if (!given.empty()) {
    VALTZ_ASSIGN(project::Asset a,
                 p.add_text(given, text, {"prompt", kNamedTag}));
    told(a.id, "defined");
    return pinned(a.id);
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
                           std::optional<AssetId> from,
                           const std::string& name)
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
               capture_prompt_(*p, pid, f.positional, from, name));
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
  VALTZ_TRY(retext_prompt_(*p, id, text));
  // Named by its words -- unless the person named it.
  if (!is_named(a)) {
    (void)p->rename_asset(id, words_name(text));
  }
  post_("assets.changed", JobId{}, {{"project", pid},
                                    {"asset", id},
                                    {"reason", "edited"}});
  return ok_status();
}

Status
Controller::retext_prompt_(project::Project& p, AssetId id,
                           const std::string& text)
{
  VALTZ_ASSIGN(project::Asset a, p.asset(id));
  if (a.origin != project::Origin::Derived) {
    VALTZ_TRY(p.update_text(id, text));
    return ok_status();
  }
  // A derived prompt's words are a build of its recipe as it is: its
  // inputs as they resolve now, so it stays fresh, its upstream kept.
  VALTZ_ASSIGN(project::Recipe r, p.recipe(a.recipe));
  VALTZ_ASSIGN(auto inputs, p.resolve_inputs(r));
  const fs::path out =
      p.blobs().tmp_dir() / (AssetId::make().str() + ".txt");
  {
    std::ofstream f(out, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
      return make_error(Code::Io,
                        std::format("cannot write {}", out.string()));
    }
  }
  project::Project::BuildRecord b;
  b.recipe = std::move(r);
  b.inputs = std::move(inputs);
  b.output = out;
  b.info.type = media::MediaType::Text;
  b.info.uti = "public.utf8-plain-text";
  VALTZ_TRY(p.commit_build(id, std::move(b)));
  return ok_status();
}

Result<Controller::AppliedPrompts>
Controller::apply_suggestion(const SuggestionApplied& req)
{
  auto undo = command_(req.project, "prompt.enhance");
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(PromptForm before,
               prompt_form_(*p, req.before, req.before_inline, req.row,
                            /*bound=*/false));
  VALTZ_ASSIGN(PromptForm after,
               prompt_form_(*p, req.enhanced, req.enhanced_inline, req.row,
                            /*bound=*/false));
  for (const PromptForm* f : {&before, &after}) {
    if (assist::prompt_words(f->positional).empty() &&
        assist::find_ref_tags(f->positional).empty()) {
      return make_error(Code::InvalidArgument, msg::kPromptEmpty);
    }
  }
  // The words before it, kept as the box's would be at Start -- named as
  // they were: the person's name for the box goes with the box's words,
  // the suggestion's now.
  VALTZ_ASSIGN(project::RecipeInput up,
               capture_prompt_(*p, req.project, before.positional,
                               req.from, {}));
  const std::string given(utf8_prefix(one_line(req.name), 200));
  auto name_it = [&](AssetId id) -> Status {
    return p->update_asset(id, [&](project::Asset& a) {
      if (std::ranges::find(a.tags, std::string("prompt")) ==
          a.tags.end()) {
        a.tags.push_back("prompt");
      }
      if (!given.empty()) {
        a.name = given;
        set_named(a, true);
      }
      return ok_status();
    });
  };
  auto told = [&](AssetId id, const char* reason) {
    post_("assets.changed", JobId{}, {{"project", req.project},
                                      {"asset", id},
                                      {"reason", reason}});
  };
  // The same suggestion applied to it before: that prompt.
  VALTZ_ASSIGN(auto made, p->dependents(up.asset, false));
  for (const AssetId& d : made) {
    auto a = p->asset(d);
    if (!a.ok() || !is_prompt(*a) ||
        a->origin != project::Origin::Derived) {
      continue;
    }
    auto r = p->recipe(a->recipe);
    auto t = p->read_text(d);
    if (r.ok() && r->op == kEnhanceOp && t.ok() && *t == after.positional) {
      VALTZ_TRY(name_it(d));
      told(d, "renamed");
      return AppliedPrompts{up.asset, d};
    }
  }
  project::Recipe r;
  r.op = kEnhanceOp;
  r.inputs = {up};
  if (!req.written_for.empty()) {
    r.params["written_for"] = req.written_for;
  }
  VALTZ_ASSIGN(project::Asset e,
               p->define_derived(words_name(after.positional),
                                 project::AssetKind::Text, std::move(r),
                                 64));
  VALTZ_TRY(name_it(e.id));
  VALTZ_TRY(retext_prompt_(*p, e.id, after.positional));
  told(e.id, "defined");
  return AppliedPrompts{up.asset, e.id};
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
  // Past the longest side the model makes (FLUX.3: 4096), said here --
  // not left to a graph that would fail at its first stage.
  if (const int edge = models::max_edge(*m);
      edge > 0 && std::max(req.width, req.height) > edge) {
    return make_error(Code::InvalidArgument, msg::kSizeTooLarge,
                      {{"model", m->name},
                       {"size", std::format("{} x {}", req.width,
                                            req.height)},
                       {"edge", std::to_string(edge)}});
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
                               req.prompt_asset, req.prompt_name));
  r.inputs.push_back(pin);
  // The prompt on one line, cut to 48 bytes and made unique by the
  // project ("…", " (2)") -- a MOSS-Image layout by its description,
  // not its JSON.
  const std::string laid = assist::moss_layout_caption(words);
  std::string name = !req.name.empty() ? req.name
                     : !laid.empty()   ? one_line(laid)
                                       : one_line(
                                             assist::without_markdown(words));
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
    if (req.last) {
      row.push_back(*req.last);
    }
  }
  VALTZ_ASSIGN(PromptForm pf, prompt_form_(*p, req.prompt, req.inline_media,
                                           row, true));
  const std::string words = assist::prompt_words(pf.positional);
  if (words.empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  const bool refs = !req.references.empty() || req.continue_from;
  if (refs && (req.first || req.last)) {
    return make_error(Code::InvalidArgument, msg::kVideoFirstOrReferences);
  }
  if (req.last && !req.first) {
    return make_error(Code::InvalidArgument, msg::kVideoLastNeedsFirst);
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
  if (refs && (!m || !runs_soon(*m, "video", "edit"))) {
    return make_error(Code::NotFound, msg::kNoReferenceModel);
  }
  if (!refs && (!m || !runs_soon(*m, "video", "generate"))) {
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
  if (req.last) {
    VALTZ_ASSIGN(project::Asset la, p->asset(*req.last));
    if (la.kind != project::AssetKind::Image ||
        !jget(vp, "last_frame", false)) {
      return make_error(Code::Unsupported, msg::kVideoEndsOnPicture);
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
  // picture takes the preset's Turbo LoRA instead -- unless its adapter
  // runs as a plain LoRA, which an ordinary denoise opens on a picture
  // with.
  if (req.first && !jget(overrides, "taomate_lora", false)) {
    overrides["taomate"] = false;
  }
  // A clip CONTINUED is settled as one (TuningContext::continuation): the
  // catalog's continuation presets, not the ones that keep a picture.
  auto tctx = tuning_context(*m);
  tctx.continuation = req.continue_from.has_value();
  VALTZ_TRY(check_preset_loras_(*m, req.preference, overrides,
                                tctx.continuation));

  project::Recipe r;
  r.op = std::string(engine::kOpGenerateVideo);
  r.model = m->id;
  r.deterministic = false;
  r.params = {{"prompt", templated(*m, words)},
              {"seed", req.seed >= 0 ? req.seed : random_seed()},
              {"width", width},
              {"height", height},
              {"frames", frames},
              {"fps", fps},
              {"intermediate", intermediate_of(*p)}};
  stamp_origin(r, *m);
  // The adapter or branch is part of what made the clip: a rebuild
  // applies it too.
  apply_tuning_(r, *m, models::resolve_tuning(*m, tctx, req.preference,
                                              false, overrides),
                req.steps);
  if (req.first) {
    // The picture it opens on is <Picture 1> to the model (H3's I2VA:
    // "<Picture 1> (from [Shot 1]) is fully referenced"): its mentions
    // and tags so named, any other dropped. Opening on one and ending on
    // another (FL2VA), the two are named as that case of the guide names
    // them: "Picture 1", "Picture 2".
    const std::string form = keyframe_tag(*m, req.last.has_value());
    if (form.find("{n}") != std::string::npos) {
      std::vector<std::string> tags;
      for (const auto& id : pf.inline_media) {
        const bool last = id && req.last && *id == *req.last;
        tags.push_back(id && *id == *req.first ? numbered(form, 1)
                       : last                  ? numbered(form, 2)
                                               : std::string());
      }
      r.params["prompt"] = templated(*m, assist::tag_inline(pf.text, tags));
    }
    r.inputs.push_back({"first", *req.first, 0});
    if (req.last) {
      r.inputs.push_back({"last", *req.last, 0});
    }
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
                               req.prompt_asset, req.prompt_name));
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
                           std::string_view language,
                           assist::OutlineFrames frames) const
{
  const models::ModelEntry* m = _catalog.find(model);
  if (!m) {
    return make_error(Code::NotFound, msg::kUnknownModel,
                      {{"model", model}});
  }
  // A clip model opens on a picture, and ends on the next, as it can.
  const Json vp = jget(m->engine, "vpipe", Json::object());
  const auto picture = [&](int from, int skip) {
    for (int i = std::max(0, from); i < static_cast<int>(row.size()); ++i) {
      if (row[i] == assist::RefKind::Image && i != skip) {
        return i;
      }
    }
    return -1;
  };
  if (!jget(vp, "first_frame", false) || frames.first < -1) {
    frames.first = -1;
  } else if (frames.first == -1) {
    frames.first = picture(0, -1);
  }
  if (frames.first < 0 || !jget(vp, "last_frame", false) ||
      frames.last < -1) {
    frames.last = -1;
  } else if (frames.last == -1) {
    frames.last = picture(0, frames.first);
  }
  if (frames.seconds <= 0) {
    const Json d = jget(vp, "defaults", Json::object());
    const double fps = jget(d, "fps", 24.0);
    frames.seconds = fps > 0 ? jget(d, "frames", 0) / fps : 0;
  }
  return assist::prompt_outline(jget(m->prompting, "outline", Json()),
                                language.empty() ? _cfg.language : language,
                                row, frames);
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
  // A clip or a sound is CUT to the clip's length by the model's encoder
  // unless the request keeps it whole ("whole": the encoder told so).
  const auto kept_whole = [&](AssetId id, const std::string& role) {
    return role == "reference" &&
           std::ranges::find(req.whole, id) != req.whole.end();
  };
  // A reference is WHOLE: a span of something is a composition whose
  // layer marks it, drawn as it is when the job starts (DESIGN §6a).
  auto add = [&](AssetId id, const std::string& role) -> Status {
    VALTZ_ASSIGN(project::Asset a, p.asset(id));
    Json e = {{"asset", id.str()}, {"role", role}};
    if (a.kind != project::AssetKind::Image && kept_whole(id, role)) {
      e["whole"] = true;
    }
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

// ---- ASSET OPERATIONS (asset-ops.h, DESIGN §4j) ------------------------

Result<Controller::AssetOpResult>
Controller::run_asset_operation(AssetOpRequest req)
{
  const AssetOperation* op = find_asset_operation(req.operation);
  if (!op) {
    return make_error(Code::InvalidArgument, msg::kUnknownOperation,
                      {{"operation", req.operation}});
  }
  // One command each, its kind the operation's name: undone, the request
  // goes -- its assets, its task -- and, once made, what it made.
  auto undo = command_(req.project, std::string(op->name), req.asset,
                       req.layer);
  project::Project* p = project(req.project);
  if (!p) {
    return make_error(Code::NotFound, msg::kProjectNotOpen);
  }
  VALTZ_ASSIGN(project::Asset a, p->asset(req.asset));
  if (req.model.empty()) {
    req.model = "auto";
  }
  if (!req.options.is_object()) {
    req.options = Json::object();
  }
  if (op->name == "transcribe") {
    return transcribe_(req, *p, a);
  }
  if (op->name == "summarize") {
    return summarize_(req, *p, a);
  }
  if (op->name == "separate") {
    return separate_(req, *p, a);
  }
  return upscale_(req, *p, a);
}

Json
Controller::asset_operations_json() const
{
  const auto caps = capabilities();
  // The helper, which watches video when it can (summarize's first pick).
  const models::ModelEntry* helper = assistant_model();
  Json out = Json::array();
  for (const auto& op : asset_operations()) {
    Json takes = Json::array();
    for (const auto& t : op.takes) {
      const auto it = std::ranges::find(
          caps, t.capability, &models::CapabilityStatus::capability);
      const auto avail = it != caps.end()
                             ? it->availability
                             : models::Availability::NotOffered;
      // What Auto runs -- or, none here, what it would once downloaded:
      // an upscale Settings › Upscaling's pick where it is installed.
      const bool up = t.capability == models::Capability::UpscaleVideo ||
                      t.capability == models::Capability::UpscaleImage;
      const models::ModelEntry* m = operation_model_(
          t.capability, "auto",
          up ? chosen_upscaler_(t.capability) : helper);
      if (!m && it != caps.end() && !it->chosen.empty()) {
        m = _catalog.find(it->chosen);
      }
      Json tk = {{"kind", project::to_str(t.kind)},
                 {"op", std::string(t.op)},
                 {"capability", models::to_str(t.capability)},
                 {"ready", avail == models::Availability::Ready},
                 {"availability", models::to_str(avail)},
                 {"model", m ? m->id : std::string()},
                 {"model_name", m ? m->name : std::string()},
                 {"family", m ? m->family : std::string()}};
      takes.push_back(std::move(tk));
    }
    Json makes = Json::array();
    for (const auto& k : op.makes) {
      makes.push_back(std::string(k));
    }
    out.push_back({{"name", std::string(op.name)},
                   {"label", std::string(op.label)},
                   {"on_layer", op.on_layer},
                   {"makes", std::move(makes)},
                   {"takes", std::move(takes)}});
  }
  return out;
}

const models::ModelEntry*
Controller::operation_model_(
    models::Capability cap, const std::string& model,
    const models::ModelEntry* prefer,
    const std::function<bool(const models::ModelEntry&)>& fits) const
{
  if (!engine_runs_(cap)) {
    return nullptr;
  }
  const auto serves = [&](const models::ModelEntry& e) {
    return e.has(cap) && (!fits || fits(e));
  };
  const auto installed = [&](const models::ModelEntry& e) {
    return _store->info(e).state == models::InstallState::Installed;
  };
  // Named: installed, or downloading (its job waits for it).
  if (!model.empty() && model != "auto") {
    const auto* e = _catalog.find(model);
    return e && serves(*e) && coming_(*e) ? e : nullptr;
  }
  if (prefer && serves(*prefer) && installed(*prefer)) {
    return prefer;
  }
  const models::ModelEntry* best = nullptr;
  for (const bool soon : {false, true}) {
    for (const auto& e : _catalog.models()) {
      if (serves(e) && (soon ? coming_(e) : installed(e)) &&
          (!best || e.rank > best->rank)) {
        best = &e;
      }
    }
    if (best) {
      break;
    }
  }
  return best;
}

Result<Controller::AssetOpResult>
Controller::make_from_(
    ProjectId pid, project::Project& p, const project::Asset& src,
    std::vector<OpMade> made, bool beside,
    const std::function<void(const std::vector<AssetId>&)>& before,
    const std::function<void(const std::vector<AssetId>&)>& failed)
{
  if (made.empty()) {
    return make_error(Code::Internal, "an operation that makes nothing");
  }
  AssetOpResult out;
  std::string title;
  for (auto& m : made) {
    // Each says which of the job's outputs it holds: rebuilt alone, it
    // keeps that one.
    if (!m.output.empty()) {
      m.recipe.params["output"] = m.output;
    }
    VALTZ_ASSIGN(project::Asset a,
                 p.define_derived(m.name, m.kind, m.recipe, 96));
    // Beside what it is made from, in the list.
    if (beside && !src.folder.empty()) {
      (void)p.set_asset_folder(a.id, src.folder);
    }
    post_("assets.changed", JobId{}, {{"project", pid},
                                      {"asset", a.id},
                                      {"reason", "defined"}});
    if (out.made.empty()) {
      title = a.name;
    }
    out.made.push_back(a.id);
  }
  if (before) {
    before(out.made);
  }
  std::vector<std::pair<std::string, AssetId>> companions;
  for (std::size_t i = 1; i < made.size(); ++i) {
    companions.emplace_back(made[i].output, out.made[i]);
  }
  auto job = submit_build_(pid, out.made.front(), title,
                           std::move(companions));
  if (!job.ok()) {
    if (failed) {
      failed(out.made);
    }
    return job.error();
  }
  out.job = *job;
  return out;
}

Result<Controller::AssetOpResult>
Controller::transcribe_(const AssetOpRequest& req, project::Project& p,
                        const project::Asset& src)
{
  // A sound, or a clip with one (a composition's: its mix).
  const bool timed = src.kind == project::AssetKind::Audio ||
                     src.kind == project::AssetKind::Video;
  if (!timed || !sounds_(p, src, 0) ||
      ((src.cls == project::AssetClass::Flat ||
        src.cls == project::AssetClass::Generated) && src.head == 0)) {
    return make_error(Code::InvalidArgument, msg::kTranscribeNeedsSound,
                      {{"name", src.name}});
  }
  // The speech model; its voice detector; the tagger when there is one.
  const models::ModelEntry* m =
      operation_model_(models::Capability::Transcribe, req.model);
  if (!m) {
    return make_error(Code::NotFound, msg::kNoTranscriber);
  }
  const Json listen = jget(jget(m->engine, "vpipe", Json::object()),
                           "listen", Json::object());
  const auto installed = [&](const std::string& id) {
    const auto* e = _catalog.find(id);
    return e && coming_(*e);
  };
  const auto vad = jget<std::string>(listen, "vad", "");
  if (vad.empty() || !installed(vad)) {
    return make_error(Code::NotFound, msg::kNoTranscriber);
  }
  const auto tagger = jget<std::string>(listen, "tagger", "");
  project::Recipe r;
  r.op = std::string(engine::kOpTranscribeAudio);
  r.model = m->id;
  r.deterministic = true;
  r.params = {{"name", src.name}, {"vad", vad}};
  if (!tagger.empty() && installed(tagger)) {
    r.params["tagger"] = tagger;
  }
  if (const auto language = jget<std::string>(req.options, "language", "");
      !language.empty()) {
    r.params["language"] = language;
  }
  const bool drawn = src.cls == project::AssetClass::Composition;
  r.inputs.push_back({"source", src.id, drawn ? 0 : src.head});
  stamp_origin(r, *m);
  std::vector<OpMade> made;
  made.push_back({std::format("{} transcript", src.name),
                  project::AssetKind::Text, std::move(r), ""});
  return make_from_(req.project, p, src, std::move(made), true);
}

Result<Controller::AssetOpResult>
Controller::summarize_(const AssetOpRequest& req, project::Project& p,
                       const project::Asset& src)
{
  // A clip, or a timeline with a picture (one of sound alone is kind
  // audio): what it shows, as it plays.
  const bool drawn = src.cls == project::AssetClass::Composition;
  if (src.kind != project::AssetKind::Video ||
      (!drawn && src.head == 0)) {
    return make_error(Code::InvalidArgument, msg::kSummarizeNeedsPicture,
                      {{"name", src.name}});
  }
  // The model: the one named; else the helper, when it watches video;
  // else the best that does.
  const models::ModelEntry* m = operation_model_(
      models::Capability::VideoSummary, req.model, assistant_model());
  if (!m) {
    return make_error(Code::NotFound, msg::kNoVideoWatcher);
  }
  // How often: as asked, else the setting -- Auto by the clip's length,
  // a timeline's by its own frames.
  double every = jget(req.options, "every", 0.0);
  if (every > 0) {
    every = std::clamp(every, 0.125, 60.0);
  } else {
    double seconds = 0;
    if (auto n = clip_frames_(p, src); n.ok() && n->second.num > 0) {
      seconds = static_cast<double>(n->first) / n->second.to_double();
    }
    if (seconds <= 0 && !drawn) {
      if (auto v = p.version(src.id);
          v.ok() && v->info.duration.valid()) {
        seconds = v->info.duration.seconds();
      }
    }
    every = video_every(seconds);
  }
  // How large: as asked, else the setting; the long edge and the short,
  // a portrait clip's turned.
  const auto level = jget<std::string>(req.options, "size", _video_size);
  const media::PixelSize most = video_frame_most(level);
  project::Recipe r;
  r.op = std::string(engine::kOpSummarizeVideo);
  r.model = m->id;
  // Its sampler draws afresh each time: asked again, it may say it
  // otherwise.
  r.deterministic = false;
  r.params = {{"name", src.name},
              {"every", every},
              {"max_width", most.width},
              {"max_height", most.height},
              {"language", assist::language_name(_cfg.language)}};
  r.inputs.push_back({"source", src.id, drawn ? 0 : src.head});
  stamp_origin(r, *m);
  std::vector<OpMade> made;
  made.push_back({std::format("{} summary", src.name),
                  project::AssetKind::Text, std::move(r), ""});
  return make_from_(req.project, p, src, std::move(made), true);
}

Result<Controller::AssetOpResult>
Controller::separate_(const AssetOpRequest& req, project::Project& p,
                      const project::Asset& src)
{
  // A sound: imported, made, or a composition of sound alone -- its mix.
  const bool drawn = src.cls == project::AssetClass::Composition;
  if (src.kind != project::AssetKind::Audio || !sounds_(p, src, 0) ||
      (!drawn && src.head == 0)) {
    return make_error(Code::InvalidArgument, msg::kSeparateNeedsSound,
                      {{"name", src.name}});
  }
  // The SPLIT asked for (catalog engine.vpipe.separate `split`): "vocals"
  // -- the voice and the accompaniment. A model serves it when it is its
  // split; each of its two outputs is one of the split's sources.
  const auto split = jget<std::string>(req.options, "split", "vocals");
  const auto block = [](const models::ModelEntry& e) {
    return jget(jget(e.engine, "vpipe", Json::object()), "separate",
                Json::object());
  };
  const models::ModelEntry* m = operation_model_(
      models::Capability::AudioSeparate, req.model, nullptr,
      [&](const models::ModelEntry& e) {
        return jget<std::string>(block(e), "split", "") == split;
      });
  if (!m) {
    return make_error(Code::NotFound, msg::kNoSeparator);
  }
  std::vector<std::string> sources;
  for (const auto& s : jget(block(*m), "sources", Json::array())) {
    if (s.is_string()) {
      sources.push_back(s.get<std::string>());
    }
  }
  if (sources.empty()) {
    return make_error(Code::Internal, std::format(
        "{} names no sources for its split", m->name));
  }
  project::Recipe r;
  r.op = std::string(engine::kOpSeparateAudio);
  r.model = m->id;
  r.deterministic = true;
  // The checkpoint by its file: its folder may hold another model.
  r.params = {{"name", src.name}, {"split", split}, {"checkpoint", m->file}};
  r.inputs.push_back({"source", src.id, drawn ? 0 : src.head});
  stamp_origin(r, *m);
  std::vector<OpMade> made;
  for (const auto& source : sources) {
    made.push_back({std::format("{} {}", src.name, source),
                    project::AssetKind::Audio, r, source});
  }
  return make_from_(req.project, p, src, std::move(made), true);
}

Result<Controller::AssetOpResult>
Controller::upscale_(const AssetOpRequest& req, project::Project& p,
                     const project::Asset& comp)
{
  VALTZ_TRY(unlocked_(comp, req.layer));
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
  VALTZ_ASSIGN(project::Asset src, p.asset(*layer->source));
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
  VALTZ_ASSIGN(project::AssetVersion v, p.version(src.id));
  // One size: the crop's one key (or none: nothing to upscale).
  const media::KeyedCrop keys = crop_keys_of(comp, req.layer);
  if (keys.place.keys.size() > 1) {
    return make_error(Code::InvalidArgument, msg::kUpscaleKeyed,
                      {{"name", src.name}});
  }
  const media::Crop c = keys.place.at(0);
  const media::PixelSize from{v.info.frame.width, v.info.frame.height};
  // Its size either way: a flip is not a scale.
  const double sx = std::max(1.0, std::abs(c.scale_x));
  const double sy = std::max(1.0, std::abs(c.scale_y));
  if ((sx <= 1.0 && sy <= 1.0) || from.width <= 0 || from.height <= 0) {
    return make_error(Code::InvalidArgument, msg::kUpscaleNotScaled,
                      {{"name", src.name}});
  }
  // Even sides, as the writer takes them.
  auto even = [](double x) {
    return std::max(2, static_cast<int>(std::lround(x / 2)) * 2);
  };
  const media::PixelSize to{even(from.width * sx), even(from.height * sy)};
  // The upscaler: the one named, else Settings › Upscaling's pick where
  // it is installed, else the best installed that runs -- else one
  // downloading (its job waits for it).
  const models::ModelEntry* m =
      operation_model_(need, req.model, chosen_upscaler_(need));
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
  const auto seed = jget<std::int64_t>(req.options, "seed", 0);
  project::Recipe r;
  r.model = m->id;
  r.deterministic = false;
  if (clip) {
    r.op = std::string(engine::kOpUpscaleVideo);
    r.params = {{"width", to.width},
                {"height", to.height},
                {"first", first},
                {"count", count},
                {"seed", seed},
                {"group", jget(up, "group", 25)},
                {"overlap", jget(up, "overlap", 4)},
                {"intermediate", intermediate_of(p)}};
  } else {
    r.op = std::string(engine::kOpUpscaleImage);
    r.params = {{"width", to.width},
                {"height", to.height},
                {"seed", seed}};
    // Its eyes (VOSR's DINOv2), by catalog id: resolved at submit.
    if (const auto enc = jget<std::string>(up, "encoder", "");
        !enc.empty()) {
      r.params["encoder"] = enc;
    }
  }
  // Its arithmetic, as Settings › Upscaling has it: the stages' int8
  // GEMMs, Sage, Sol (graph-builder's accel_of).
  r.params["tuning"] = upscale_tuning(*m);
  r.inputs.push_back({"source", src.id, v.number});
  stamp_origin(r, *m);
  std::vector<OpMade> made;
  made.push_back({std::format("{} {}×{}", src.name, to.width, to.height),
                  clip ? project::AssetKind::Video
                       : project::AssetKind::Image,
                  std::move(r), ""});
  // Its layer, to show it once it is made (apply_upscale_).
  const UpscaleTarget target{req.asset, req.layer, from, to};
  return make_from_(
      req.project, p, src, std::move(made), false,
      [&](const std::vector<AssetId>& ids) {
        std::lock_guard lk(_upscale_mu);
        _upscale_targets[ids.front()] = target;
      },
      [&](const std::vector<AssetId>& ids) {
        std::lock_guard lk(_upscale_mu);
        _upscale_targets.erase(ids.front());
      });
}

Result<fs::path>
Controller::coreml_package_(const models::ModelEntry& e,
                            const models::InstallInfo& info) const
{
  const fs::path dir = info.dir / (e.package.empty() ? e.id : e.package);
  std::error_code ec;
  if (fs::is_directory(dir, ec)) {
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) {
        break;
      }
      const auto ext = it->path().extension().string();
      if (ext == ".mlpackage" || ext == ".mlmodelc") {
        return it->path();
      }
    }
  }
  return make_error(Code::NotFound, msg::kModelNotUnpacked,
                    {{"model", e.name}});
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
  VALTZ_TRY(unlocked_(comp, layer_id));
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
  if (!req.voice && !req.voices.empty()) {
    req.voice = req.voices.front();
  }
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
    if (!runs_soon(*m, "audio", "generate")) {
      return make_error(Code::NotFound, msg::kNoSpeechModel);
    }
    VALTZ_ASSIGN(project::RecipeInput pin,
                 capture_prompt_(*p, req.project, pf.positional,
                                 req.prompt_asset, req.prompt_name));
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
  if (decoder && !coming_(*decoder)) {
    return make_error(Code::NotFound, msg::kModelNotInstalled,
                      {{"model", decoder->name}});
  }
  if (!runs_soon(*m, "audio", "generate")) {
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
                               req.prompt_asset, req.prompt_name));
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
  const Json vp = jget(m.engine, "vpipe", Json::object());
  const Json spc = jget(vp, "speech", Json::object());
  if (jget<std::string>(spc, "format", "") == "drama3") {
    return drama_speech_(req, m, p, words, prompt);
  }
  // Its fields, then its words (DESIGN §4g); the language named when the
  // prompt does not -- the model speaks markedly better told it.
  assist::SpeechText sp = assist::split_speech(words);
  if (assist::prompt_words(sp.text).empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  if (sp.language.empty()) {
    sp.language = assist::speech_language(sp.text);
  }
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

Result<JobId>
Controller::drama_speech_(GenerateAudioRequest& req,
                          const models::ModelEntry& m, project::Project& p,
                          const std::string& words,
                          const project::RecipeInput& prompt)
{
  const Json spc = jget(jget(m.engine, "vpipe", Json::object()), "speech",
                        Json::object());
  const int most = std::clamp(jget(spc, "speakers", 16), 1,
                              assist::kDrama3Speakers);
  // The voices, speaker 0 first: those given; else the one asked for,
  // then every other sound in the row.
  std::vector<AssetId> voices = req.voices;
  if (voices.empty()) {
    if (req.voice) {
      voices.push_back(*req.voice);
    }
    for (const auto& id : req.row) {
      if (std::ranges::find(voices, id) == voices.end() && voiced_(p, id)) {
        voices.push_back(id);
      }
    }
  }
  if (voices.size() > static_cast<std::size_t>(most)) {
    voices.resize(static_cast<std::size_t>(most));
  }
  // The script as the model reads it: MOSS's fields, should the prompt
  // have them, its words only -- the instruction a direction over them
  // -- and a scene's names made its speakers' tags.
  const assist::SpeechText sp = assist::split_speech(words);
  if (assist::prompt_words(sp.text).empty()) {
    return make_error(Code::InvalidArgument, msg::kPromptEmpty);
  }
  const assist::Drama3Script script = assist::drama3_script(
      sp.text, static_cast<int>(voices.size()));
  std::string text = script.text;
  if (!sp.instruction.empty() && script.names.empty() &&
      text.find("<|speaker:") == std::string::npos) {
    text = std::format("[{}] {}", one_line(sp.instruction), text);
  }

  project::Recipe r;
  r.op = std::string(engine::kOpGenerateSpeech);
  r.model = m.id;
  r.deterministic = false;
  r.params = {{"prompt", words},
              {"text", text},
              {"speakers", script.names},
              {"seed", req.seed >= 0 ? req.seed : random_seed()}};
  stamp_origin(r, m);
  r.inputs.push_back(prompt);
  // Each voice and its words: given, else heard -- its transcript, made
  // now when it has none (the speech then waits for it).
  Json vs = Json::array();
  for (std::size_t i = 0; i < voices.size(); ++i) {
    VALTZ_ASSIGN(project::Asset v, p.asset(voices[i]));
    if (!voiced_(p, voices[i])) {
      return make_error(Code::InvalidArgument, msg::kVoiceKind,
                        {{"name", v.name}});
    }
    r.inputs.push_back({"voice", voices[i], 0});
    const std::string given =
        i < req.voice_texts.size() ? one_line(req.voice_texts[i])
                                   : std::string();
    if (!given.empty()) {
      vs.push_back({{"words", given}});
      continue;
    }
    std::optional<AssetId> t = transcript_of_(p, voices[i]);
    if (!t) {
      AssetOpRequest heard;
      heard.project = req.project;
      heard.asset = voices[i];
      heard.operation = "transcribe";
      auto made = transcribe_(heard, p, v);
      if (!made.ok()) {
        if (made.error().key == msg::kNoTranscriber.key) {
          return make_error(Code::NotFound, msg::kVoiceNeedsWords,
                            {{"name", v.name}});
        }
        return made.error();
      }
      t = made->made.front();
    }
    r.inputs.push_back({"voice_text", *t, 0});
    vs.push_back({{"transcript", t->str()}});
  }
  r.params["voices"] = std::move(vs);
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

std::optional<AssetId>
Controller::transcript_of_(project::Project& p, AssetId voice) const
{
  auto deps = p.dependents(voice, false);
  if (!deps.ok()) {
    return std::nullopt;
  }
  std::optional<AssetId> built;
  std::optional<AssetId> coming;
  for (const auto& d : *deps) {
    auto a = p.asset(d);
    auto r = a.ok() ? p.recipe(a->recipe)
                    : Result<project::Recipe>(a.error());
    if (!r.ok() || r->op != engine::kOpTranscribeAudio) {
      continue;
    }
    if (a->head > 0) {
      // Heard as it is now -- not of an earlier version.
      auto st = p.staleness(d);
      if (st.ok() && st->fresh()) {
        built = d;
      }
    } else if (building_(p.id(), d)) {
      coming = d;
    }
  }
  return built ? built : coming;
}

Status
Controller::voice_words_(project::Project& p, Json& params,
                         double keep) const
{
  Json vs = jget(params, "voices", Json::array());
  if (!vs.is_array()) {
    return ok_status();
  }
  for (auto& v : vs) {
    if (const auto w = jget<std::string>(v, "words", ""); !w.empty()) {
      v["text"] = w;
      continue;
    }
    const auto id = AssetId::parse(jget<std::string>(v, "transcript", ""));
    if (!id) {
      return make_error(Code::InvalidArgument,
                        "a voice to clone needs the words said in it");
    }
    VALTZ_ASSIGN(project::Asset t, p.asset(*id));
    VALTZ_ASSIGN(project::AssetVersion tv, p.version(*id));
    const assist::Drama3Voice heard = assist::drama3_voice(
        jget(tv.outputs, "transcript", Json::object()), keep);
    if (heard.text.empty()) {
      // Named by what it is of: "Voice transcript" was heard in "Voice".
      std::string of = t.name;
      if (of.ends_with(" transcript")) {
        of.resize(of.size() - std::string_view(" transcript").size());
      }
      return make_error(Code::InvalidArgument, msg::kVoiceNoSpeech,
                        {{"name", of}});
    }
    v["text"] = heard.text;
    v["seconds"] = heard.seconds;
  }
  params["voices"] = std::move(vs);
  return ok_status();
}

std::optional<JobId>
Controller::building_(ProjectId pid, AssetId asset) const
{
  for (const auto& j : _jobs.list()) {
    if (j.project == pid && j.asset == asset && j.purpose == "build" &&
        (j.state == JobState::Queued || j.state == JobState::Running)) {
      return j.id;
    }
  }
  return std::nullopt;
}

void
Controller::release_held_(JobId job, JobState st)
{
  std::vector<std::pair<JobId, HeldBuild>> go;
  std::vector<std::pair<JobId, HeldBuild>> fail;
  {
    std::lock_guard lk(_downloads_mu);
    for (auto it = _held.begin(); it != _held.end();) {
      if (it->second.after.erase(job) == 0) {
        ++it;
        continue;
      }
      if (st != JobState::Finished) {
        fail.emplace_back(it->first, std::move(it->second));
      } else if (it->second.after.empty()) {
        go.emplace_back(it->first, std::move(it->second));
      } else {
        ++it;
        continue;
      }
      it = _held.erase(it);
    }
  }
  auto failed = [&](JobId id, const Error& e) {
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Failed;
    ev.error = e.code;
    ev.text = e.message;
    ev.data = message_fields(e);
    on_build_event_(ev);
  };
  for (auto& [id, h] : fail) {
    const std::string names = h.names.empty() ? std::string()
                                              : h.names.front();
    VALTZ_LOG_INFO("build", "job {}: {} was not made", id.str(), names);
    failed(id, make_error(Code::NotFound, msg::kHeldInputFailed,
                          {{"name", names}}));
  }
  for (auto& [id, h] : go) {
    VALTZ_LOG_INFO("build", "job {}: what it waited for is made, queued",
                   id.str());
    auto r = submit_build_(h.project, h.asset, h.title,
                           std::move(h.companions), id);
    if (!r.ok()) {
      failed(id, r.error());
    }
  }
}

bool
Controller::can_edit_(const models::ModelEntry& m, bool fleet) const
{
  // The catalog says the model edits; the engine must also have an edit
  // graph, and the family must say how that graph takes references.
  const Json vp = jget(m.engine, "vpipe", Json::object());
  if (!m.has(models::Capability::ImageEdit) ||
      !jget(vp, "edit", Json()).is_object()) {
    return false;
  }
  return (_engine->available() && _engine->supports(engine::kOpEditImage)) ||
         (fleet && fleet_runs_(std::format("image/edit/{}", m.id)));
}

bool
Controller::fleet_runs_(const std::string& key) const
{
  if (!_fleet) {
    return false;
  }
  // Never sent: what only a member runs does not run.
  const auto a = key.find('/');
  const auto b = key.find('/', a + 1);
  if (a != std::string::npos && b != std::string::npos) {
    const auto* m = _catalog.find(key.substr(b + 1));
    if (m && offload_policy_(task_of(*m, key.substr(0, a),
                                     key.substr(a + 1, b - a - 1)),
                             m->id) == fleet::kOffloadNever) {
      return false;
    }
  }
  for (const auto& member : _fleet->members()) {
    const Json self = jget(member, "self", Json::object());
    if (jget(self, "accepting", false) &&
        listed(jget(self, "runs", Json()), key)) {
      return true;
    }
  }
  return false;
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
Controller::submit_build_(
    ProjectId pid, AssetId aid, std::string title,
    std::vector<std::pair<std::string, AssetId>> companions,
    std::optional<JobId> reuse)
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
  const JobId id = reuse ? *reuse : JobId::make();
  // What its commit joins: the command it is submitted in -- now, for a
  // build held to be submitted later.
  if (project::Workspace* w = workspace_(pid); w && !reuse) {
    if (const auto seq = w->undo().recording()) {
      std::lock_guard lk(_job_commands_mu);
      _job_commands[id] = seq;
    }
  }
  // An input still being made (a voice's transcript): held until it is.
  if (!reuse) {
    HeldBuild h{pid, aid, title, companions, {}, {}};
    for (const auto& in : r.inputs) {
      auto ia = p->asset(in.asset);
      if (!ia.ok() || ia->origin != project::Origin::Derived ||
          ia->head > 0 || in.version > 0) {
        continue;
      }
      if (const auto j = building_(pid, in.asset)) {
        h.after.insert(*j);
        h.names.push_back(ia->name);
      }
    }
    if (!h.after.empty()) {
      JobRecord rec;
      rec.id = id;
      rec.op = r.op;
      rec.purpose = "build";
      rec.title = std::move(title);
      rec.project = pid;
      rec.asset = aid;
      rec.companions = std::move(companions);
      rec.recipe = r;
      rec.created_ms = project::now_ms();
      _jobs.add(rec);
      VALTZ_LOG_INFO("build", "job {}: waits for {} to be made", id.str(),
                     h.names.front());
      {
        std::lock_guard lk(_downloads_mu);
        _held.emplace(id, std::move(h));
      }
      post_("job.queued", rec.id, {{"title", rec.title}, {"op", rec.op},
                                   {"purpose", rec.purpose},
                                   {"project", pid}, {"asset", aid}});
      return id;
    }
  }
  VALTZ_ASSIGN(auto inputs, p->resolve_inputs(r));
  if (!_catalog.find(r.model)) {
    return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                      {{"model", r.model}});
  }
  // Speech in voices cloned by their words (Drama 3): what each says, as
  // its transcript heard it -- in what is sent, here or to a member.
  if (r.op == engine::kOpGenerateSpeech && r.params.contains("voices")) {
    const auto* sm = _catalog.find(r.model);
    const double keep = jget(
        jget(jget(sm->engine, "vpipe", Json::object()), "speech",
             Json::object()),
        "voice_seconds", 12.0);
    VALTZ_TRY(voice_words_(*p, r.params, keep));
  }
  // Where it runs (DESIGN §11): here, when this Mac has what it needs
  // and is free; else, in a fleet, a member that has it and is idle. A
  // weight file of one's own stays here: it is not sent.
  engine::ModelRef ref;
  Json helpers = Json::object();
  Status here = _engine->supports(r.op)
                    ? resolve_build_(r.model, r.params, ref, helpers)
                    : Status(make_error(Code::Unsupported,
                                        msg::kEngineCannotRun,
                                        {{"operation", r.op}}));
  bool own_files = false;
  for (const auto& k : own_file_keys()) {
    own_files = own_files ||
                !jget<std::string>(r.params, k.c_str(), "").empty();
  }
  std::vector<std::string> needs = {r.model};
  for (const char* k : {"lora", "decoder", "encoder", "branch", "vad",
                        "tagger"}) {
    if (const auto id = jget<std::string>(r.params, k, ""); !id.empty()) {
      needs.push_back(id);
    }
  }
  std::vector<Json> members;
  if (!own_files) {
    members = fleet_candidates_(r.op, needs, here.ok());
  }
  // Set to run on the fleet only: never here, and refused while no member
  // can take it (a weight file of one's own included: it is not sent).
  const bool fleet_only =
      offload_policy_(r.op, r.model) == fleet::kOffloadAlways;
  if (fleet_only && members.empty()) {
    return make_error(Code::Unsupported, msg::kFleetOnlyNoMember);
  }
  // What it lacks here is downloading (DESIGN §3a): it waits for those
  // downloads -- and nothing else does.
  std::vector<std::string> awaits;
  if (members.empty() && !here.ok() && _engine->supports(r.op)) {
    awaits = awaited_(needs);
  }
  if (members.empty() && !here.ok() && awaits.empty()) {
    return here.error();
  }

  engine::JobSpec spec;
  spec.id = id;
  spec.op = r.op;
  spec.model = ref;
  spec.params = r.params;
  spec.params.update(helpers);
  spec.output_dir = p->blobs().tmp_dir();
  project::BlobRef rendered_base;
  for (const auto& in : inputs) {
    VALTZ_ASSIGN(project::Asset ia, p->asset(in.asset));
    const bool picture_in = in.role == "base" || in.role == "reference" ||
                            in.role == "first" || in.role == "last";
    // The history's "base as sent": an edit's base, a clip's opening.
    const bool opening = in.role == "base" || in.role == "first";
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
      if (opening && ia.kind == project::AssetKind::Image &&
          rendered_base.empty()) {
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
    if (ia.kind == project::AssetKind::Video) {
      // A clip's references and the clip it continues are vpipe's to
      // read: a turned one is handed over upright.
      VALTZ_TRY(upright_input_(pid, ji, r.op == engine::kOpGenerateVideo));
    }
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
        if (opening && rendered_base.empty()) {
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
  rec.companions = std::move(companions);
  rec.recipe = r;
  rec.inputs = inputs;
  rec.rendered_base = rendered_base;
  rec.created_ms = project::now_ms();
  if (reuse) {
    // Held until now: listed since it was asked for.
    _jobs.update(id, [&](JobRecord& j) {
      j.recipe = rec.recipe;
      j.inputs = rec.inputs;
      j.rendered_base = rec.rendered_base;
    });
  } else {
    _jobs.add(rec);
    post_("job.queued", rec.id, {{"title", rec.title}, {"op", rec.op},
                                 {"purpose", rec.purpose},
                                 {"project", pid}, {"asset", aid}});
  }

  engine::JobSink sink = [this](const engine::JobEvent& ev) {
    on_build_event_(ev);
  };
  if (!awaits.empty()) {
    std::string names;
    for (const auto& id : awaits) {
      names += (names.empty() ? "" : ", ") + _catalog.find(id)->name;
    }
    VALTZ_LOG_INFO("build", "job {}: waits for {} to download",
                   rec.id.str(), names);
    bool ended = false;
    {
      std::lock_guard lk(_downloads_mu);
      // A download that ended since awaited_ looked: resolved below.
      std::erase_if(awaits, [&](const std::string& id) {
        return !_downloads.contains(id);
      });
      ended = awaits.empty();
      if (!ended) {
        _waiting.emplace(rec.id,
                         Waiting{std::move(spec), std::move(sink), r.model,
                                 r.params,
                                 {awaits.begin(), awaits.end()}});
      }
    }
    if (!ended) {
      return rec.id;
    }
    // It ended before the job was held: as it ended for a waiting one.
    if (Status st = resolve_build_(r.model, r.params, spec.model, helpers);
        !st.ok()) {
      engine::JobEvent ev;
      ev.job = rec.id;
      ev.kind = engine::JobEventKind::Failed;
      ev.error = st.code();
      ev.text = st.error().message;
      ev.data = message_fields(st.error());
      sink(ev);
      return rec.id;
    }
    spec.params.update(helpers);
  }
  if (members.empty()) {
    VALTZ_TRY(_engine->submit(std::move(spec), std::move(sink)));
    return rec.id;
  }
  fleet::Offer offer{spec.id, r.op, r.model, rec.title, r.params,
                     spec.inputs};
  const fs::path out = spec.output_dir;
  std::optional<engine::JobSpec> local;
  if (here.ok() && !fleet_only) {
    local = std::move(spec);
  }
  VALTZ_TRY(run_remote_(std::move(members), std::move(offer), out,
                        std::move(local), std::move(sink)));
  return rec.id;
}

Status
Controller::resolve_build_(const std::string& model, const Json& params,
                           engine::ModelRef& ref, Json& helpers) const
{
  const auto* m = _catalog.find(model);
  if (!m) {
    return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                      {{"model", model}});
  }
  VALTZ_ASSIGN(ref, resolve_model_(*m, true));
  const Json& rp = params;
  // A helper at work on a build (a video summary): loaded as its chat
  // loads it -- the same drafter, sampler and warm hold -- so either finds
  // the model the other kept.
  if (m->role == "assistant") {
    helpers.update(helper_params_(*m));
  }

  // A run-time adapter the recipe applies (the Turbo LoRA), by its file.
  if (const auto lora = jget<std::string>(rp, "lora", "");
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
  // LoRAs by their files, in their slots: "lora_file" the first,
  // "lora2_file" .. "lora8_file" the rest -- a slot left out kept empty
  // in its place (a slot is its index).
  for (int k = 1; k <= engine::kLoraSlots; ++k) {
    const std::string key =
        k == 1 ? std::string("lora_file") : std::format("lora{}_file", k);
    const auto f = jget<std::string>(rp, key.c_str(), "");
    if (f.empty()) {
      continue;
    }
    std::error_code ec;
    if (!fs::is_regular_file(f, ec)) {
      return make_error(Code::NotFound, msg::kLoraMissing, {{"file", f}});
    }
    if (k == 1) {
      ref.lora = f;
    } else {
      ref.more_loras.resize(std::max<std::size_t>(ref.more_loras.size(),
                                                  k - 1));
      ref.more_loras[k - 2] = f;
    }
  }
  // A generator's own decoder (YuE2's VAE), by its folder -- unless a
  // checkpoint of one's own takes its place (below).
  if (const auto dec = jget<std::string>(rp, "decoder", "");
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
  if (const auto enc = jget<std::string>(rp, "encoder", "");
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
  // A listener's helpers (DESIGN §4h): the voice detector and the
  // tagger, CoreML packages unpacked beside their archives (catalog
  // `package`), by catalog id -- the stages are handed the package.
  for (const auto& [key, out] : {std::pair{"vad", "vad_path"},
                                 std::pair{"tagger", "tagger_path"}}) {
    const auto id = jget<std::string>(rp, key, "");
    if (id.empty()) {
      continue;
    }
    const auto* he = _catalog.find(id);
    if (!he) {
      return make_error(Code::NotFound, msg::kRecipeModelUnknown,
                        {{"model", id}});
    }
    const auto hi = _store->info(*he);
    if (hi.state != models::InstallState::Installed) {
      return make_error(Code::NotFound, msg::kModelNotInstalled,
                        {{"model", he->name}});
    }
    VALTZ_ASSIGN(fs::path pkg, coreml_package_(*he, hi));
    helpers[out] = pkg.string();
  }
  // Community checkpoints for its DiT and VAE, files or folders.
  for (const auto& [key, slot] :
       {std::pair{"dit_file", &ref.dit}, std::pair{"vae_file", &ref.vae},
        std::pair{"encoder_weights", &ref.encoder_weights},
        std::pair{"vae_weights", &ref.vae_weights}}) {
    const auto f = jget<std::string>(rp, key, "");
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
  if (const auto branch = jget<std::string>(rp, "branch", "");
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
  return ok_status();
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
  // A result committed is the job's, asked for before any hold an agent
  // took since: never refused by it.
  CoreWrites own;
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
    // A job that makes several assets (a separation's sources): the one
    // this output is -- by its recipe's "output" -- else none asked for it
    // (one rebuilt alone: its own kept, the other dropped).
    AssetId target = rec->asset;
    project::Recipe recipe = rec->recipe;
    if (const auto out = jget<std::string>(ev.data, "output", "");
        !out.empty() &&
        jget<std::string>(rec->recipe.params, "output", "") != out) {
      target = AssetId{};
      for (const auto& [name, id] : rec->companions) {
        if (name != out) {
          continue;
        }
        auto a = p->asset(id);
        auto r = a.ok() ? p->recipe(a->recipe)
                        : Result<project::Recipe>(a.error());
        if (r.ok()) {
          target = id;
          recipe = std::move(*r);
        }
      }
      if (target.is_nil()) {
        VALTZ_LOG_INFO("build", "job {}: its {} is not asked for, dropped",
                       ev.job.str(), out);
        std::error_code ec;
        fs::remove(ev.output, ec);
        break;
      }
    }
    project::Project::BuildRecord b;
    b.recipe = recipe;
    b.inputs = rec->inputs;
    b.output = ev.output;
    b.info = ev.output_info;
    // Made by a fleet member: its engine, its name.
    b.engine = rec->runner.empty() ? engine_description()
                                   : rec->runner_engine;
    b.host = rec->runner.empty() ? _host : rec->runner_name;
    b.model_digest = rec->model_digest;
    // What it made beside its file: a song's score; a transcript's
    // lines and events, as data.
    if (const auto score = jget<std::string>(ev.data, "score", "");
        !score.empty()) {
      b.outputs["score"] = score;
    }
    for (const char* key : {"transcript", "summary"}) {
      if (const Json t = jget(ev.data, key, Json()); t.is_object()) {
        b.outputs[key] = t;
      }
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
    auto v = p->commit_build(target, std::move(b));
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
                                     {"asset", target},
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
      // Every asset it made, its own first.
      if (!rec->companions.empty()) {
        Json all = Json::array({rec->asset.str()});
        for (const auto& c : rec->companions) {
          all.push_back(c.second.str());
        }
        d["assets"] = std::move(all);
      }
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
    // Worded for the person where it says how (a fleet member's refusal).
    if (const auto key = jget<std::string>(ev.data, "key", "");
        !key.empty()) {
      d["key"] = key;
      d["args"] = jget(ev.data, "args", Json::object());
    }
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
  if (!req.video.empty()) {
    if (!f->video) {
      return make_error(Code::InvalidArgument, msg::kExportEncodingInvalid,
                        {{"format", req.format},
                         {"why", "it is not a movie"}});
    }
    if (auto why = engine::video_encoding_problem(req.video, req.format);
        !why.empty()) {
      return make_error(Code::InvalidArgument, msg::kExportEncodingInvalid,
                        {{"format", req.format}, {"why", why}});
    }
    spec.params["video"] = engine::to_json(req.video);
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
  if (req.format == "pdf") {
    // A still as a PDF: its pages in one file, its vectors kept -- drawn
    // in the export lane (media::write_pdf) from the files it reads.
    VALTZ_ASSIGN(media::PdfDocument doc,
                 pdf_document(req.project, req.asset));
    for (const auto& file : media::files_of(doc)) {
      VALTZ_ASSIGN(ContentHash h, hash_file(file));
      spec.inputs.push_back({"file", file, h, {}});
    }
    spec.params["pdf"] = media::to_json(doc);
  } else if (!drawn) {
    // A flat or generated asset: its file, as it is (a turned clip drawn
    // upright on its way: the graph's).
    VALTZ_ASSIGN(fs::path path, p->media_path(req.asset));
    VALTZ_ASSIGN(project::AssetVersion v, p->version(req.asset));
    spec.inputs.push_back({"source", path, v.content, v.info});
    VALTZ_TRY(upright_input_(req.project, spec.inputs.back(), false));
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
  // A folder being moved: it stops copying, the move undone.
  {
    std::lock_guard lk(_paths_mu);
    if (!id.is_nil() && id == _moving) {
      _moving_stop = true;
      return ok_status();
    }
  }
  // Waiting for a download: it waits no more.
  std::optional<Waiting> w;
  bool held = false;
  {
    std::lock_guard lk(_downloads_mu);
    if (auto it = _waiting.find(id); it != _waiting.end()) {
      w = std::move(it->second);
      _waiting.erase(it);
    }
    held = _held.erase(id) > 0;
  }
  // Held for an input being made: nothing was sent.
  if (held) {
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Cancelled;
    on_build_event_(ev);
    return ok_status();
  }
  if (w) {
    engine::JobEvent ev;
    ev.job = id;
    ev.kind = engine::JobEventKind::Cancelled;
    w->sink(ev);
    return ok_status();
  }
  // Sent to a fleet member: told to stop there.
  if (auto r = _jobs.get(id); r && !r->runner.empty() && _fleet) {
    _fleet->cancel(id);
    return ok_status();
  }
  return _engine->cancel(id);
}

// ---- the fleet (fleet/fleet.h, DESIGN §11) ------------------------------


namespace {

// The engine ops that hold the machine while they run: a Mac running
// one is busy.
bool
holds_engine(std::string_view op)
{
  return op == engine::kOpGenerateImage || op == engine::kOpEditImage ||
         op == engine::kOpGenerateVideo || op == engine::kOpGenerateAudio ||
         op == engine::kOpGenerateSpeech || op == engine::kOpUpscaleVideo ||
         op == engine::kOpUpscaleImage || op == engine::kOpChat ||
         op == engine::kOpTranscribeAudio || op == engine::kOpExportMedia ||
         op == engine::kOpQuantizeModel || op == engine::kOpSummarizeVideo ||
         op == engine::kOpSeparateAudio;
}

// A member's refusal, worded for the person.
const Message&
refusal(std::string_view why)
{
  return why == "busy"                               ? msg::kFleetBusy
         : why == "schedule" || why == "not-accepting" ? msg::kFleetNotTaking
         : why == "cannot-run"                       ? msg::kFleetCannotRun
         : why == "lost" || why == "left" || why == "stopped"
             ? msg::kFleetLost
             : msg::kFleetDeclined;
}

}  // namespace

bool
Controller::busy_here_() const
{
  for (const auto& j : _jobs.list()) {
    // (One waiting for a download holds nothing yet.)
    if ((j.state == JobState::Queued || j.state == JobState::Running) &&
        j.runner.empty() && holds_engine(j.op) && !waiting_(j.id)) {
      return true;
    }
  }
  return false;
}

std::vector<Json>
Controller::fleet_candidates_(const std::string& op,
                              const std::vector<std::string>& models,
                              bool here) const
{
  std::vector<Json> out;
  if (!_fleet) {
    return out;
  }
  // A dev knob: as if this Mac were busy, to send what it could run.
  static const bool prefer = [] {
    const char* e = std::getenv("VALTZ_FLEET_PREFER");
    return e != nullptr && std::string_view(e) == "remote";
  }();
  // Its offload policy (fleet::Config): never sent; sent when this Mac
  // cannot run it or is busy (local first); else a member first.
  const std::string policy =
      offload_policy_(op, models.empty() ? "" : models.front());
  if (policy == fleet::kOffloadNever) {
    return out;
  }
  if (policy == fleet::kOffloadLocalFirst && here && !busy_here_() &&
      !prefer) {
    return out;
  }
  const auto* m = models.empty() ? nullptr : _catalog.find(models.front());
  for (auto& member : _fleet->members()) {
    const Json self = jget(member, "self", Json::object());
    if (!jget(self, "accepting", false) || jget(self, "busy", false) ||
        jget(self, "serving", false) ||
        !listed(jget(self, "ops", Json()), op)) {
      continue;
    }
    if (op == engine::kOpChat || op == engine::kOpAgentTurn) {
      if (jget<std::string>(self, "assistant", "").empty()) {
        continue;
      }
    } else if (!std::ranges::all_of(models, [&](const std::string& id) {
                 return listed(jget(self, "installed", Json()), id);
               })) {
      continue;
    }
    const Json machine = jget(self, "machine", Json::object());
    if (m && jget(machine, "ram_gb", 0u) < m->min_ram_gb) {
      continue;
    }
    out.push_back(std::move(member));
  }
  // The roomiest first.
  std::ranges::stable_sort(out, [](const Json& a, const Json& b) {
    auto ram = [](const Json& x) {
      return jget(jget(jget(x, "self", Json::object()), "machine",
                       Json::object()),
                  "ram_gb", 0);
    };
    return ram(a) > ram(b);
  });
  return out;
}

std::string
Controller::offload_policy_(std::string_view op, std::string_view model) const
{
  if (!_fleet) {
    return fleet::kOffloadLocalFirst;
  }
  const fleet::Config c = _fleet->config();
  return c.joined() ? c.offload(op, model) : fleet::kOffloadLocalFirst;
}

Status
Controller::run_remote_(std::vector<Json> members, fleet::Offer offer,
                        fs::path out_dir,
                        std::optional<engine::JobSpec> local,
                        engine::JobSink sink)
{
  if (members.empty()) {
    if (!local) {
      return make_error(Code::Unsupported, msg::kEngineCannotRun,
                        {{"operation", offer.op}});
    }
    VALTZ_LOG_INFO("fleet", "{}: here after all", offer.title);
    _jobs.update(offer.id, [](JobRecord& r) {
      r.runner.clear();
      r.runner_name.clear();
      r.runner_engine.clear();
    });
    post_("job.runner", offer.id, {{"runner", "local"}});
    return _engine->submit(std::move(*local), std::move(sink));
  }
  // Its inputs go by content: each named by its hash.
  for (auto& in : offer.inputs) {
    if (in.hash.is_zero()) {
      VALTZ_ASSIGN(in.hash, hash_file(in.path));
    }
  }
  const Json m = members.front();
  members.erase(members.begin());
  const auto id = jget<std::string>(m, "id", "");
  const auto name = jget<std::string>(m, "name", "");
  const auto engine_desc = jget<std::string>(
      jget(m, "self", Json::object()), "engine", "");
  _jobs.update(offer.id, [&](JobRecord& r) {
    r.runner = id;
    r.runner_name = name;
    r.runner_engine = engine_desc;
  });
  post_("job.runner", offer.id, {{"runner", name}});
  VALTZ_LOG_INFO("fleet", "{}: sent to {}", offer.title, name);
  auto started = std::make_shared<std::atomic<bool>>(false);
  auto rest = std::make_shared<std::vector<Json>>(std::move(members));
  auto keep = std::make_shared<std::optional<engine::JobSpec>>(
      std::move(local));
  const fleet::Offer again = offer;
  return _fleet->submit(
      id, offer, out_dir,
      [this, started, rest, keep, again, out_dir, sink,
       name](const engine::JobEvent& ev) {
        if (ev.kind == engine::JobEventKind::Started) {
          started->store(true);
        }
        const auto why = jget<std::string>(ev.data, "fleet", "");
        if (ev.kind != engine::JobEventKind::Failed || why.empty()) {
          sink(ev);
          return;
        }
        // Declined, or gone before it began: the next member, or here.
        if (!started->load() && !withdrawn_(ev.job) &&
            (!rest->empty() || keep->has_value())) {
          VALTZ_LOG_INFO("fleet", "{}: {} ({})", again.title, ev.text, why);
          if (run_remote_(std::move(*rest), again, out_dir,
                          std::move(*keep), sink)
                  .ok()) {
            return;
          }
        }
        // Worded for the person: who, and why.
        engine::JobEvent e = ev;
        const Message& said = refusal(why);
        const MessageArgs args = {{"member", name}};
        e.text = render_message(said.english, args);
        e.data.update(message_fields(said, args));
        sink(e);
      });
}

Json
Controller::fleet_self_() const
{
  const bool up = _engine && _engine->available();
  // What this Mac shares: a task or a model withheld is not said to be
  // here, so a member never offers it (and one that does is declined).
  const fleet::Config fc = _fleet ? _fleet->config() : fleet::Config{};
  Json ops = Json::array();
  for (std::string_view op : kFleetTasks) {
    if (up && _engine->supports(op) && fc.shares(op, "")) {
      ops.push_back(std::string(op));
    }
  }
  Json installed = Json::array();
  Json runs = Json::array();
  for (const auto& m : _catalog.models()) {
    if (_store->info(m).state != models::InstallState::Installed ||
        fc.withheld_models.contains(m.id)) {
      continue;
    }
    installed.push_back(m.id);
    for (const char* modality : {"image", "video", "audio"}) {
      for (const char* op : {"generate", "edit"}) {
        if (runs_here(m, modality, op) &&
            fc.shares(task_of(m, modality, op), m.id)) {
          runs.push_back(std::format("{}/{}/{}", modality, op, m.id));
        }
      }
    }
  }
  Json features = Json::array();
  const auto caps = up ? models::resolve_capabilities(
                             _catalog, *_store, _hw,
                             [this](models::Capability c) {
                               return engine_runs_(c);
                             })
                       : std::vector<models::CapabilityStatus>{};
  // A feature is the task it is done by: withheld, it is not offered.
  auto feature_task = [](models::Capability c) -> std::string_view {
    using models::Capability;
    switch (c) {
    case Capability::TextToImage: return engine::kOpGenerateImage;
    case Capability::ImageEdit: return engine::kOpEditImage;
    case Capability::TextToVideo:
    case Capability::ImageToVideo:
    case Capability::ReferenceToVideo: return engine::kOpGenerateVideo;
    case Capability::UpscaleImage: return engine::kOpUpscaleImage;
    case Capability::UpscaleVideo: return engine::kOpUpscaleVideo;
    case Capability::PromptEnhance:
    case Capability::Intent:
    case Capability::Caption: return engine::kOpChat;
    case Capability::TextToAudio: return engine::kOpGenerateAudio;
    case Capability::TextToSpeech: return engine::kOpGenerateSpeech;
    case Capability::Transcribe: return engine::kOpTranscribeAudio;
    case Capability::VideoSummary: return engine::kOpSummarizeVideo;
    default: return {};
    }
  };
  for (const auto& c : caps) {
    const auto task = feature_task(c.capability);
    // A separation runs here alone: a job's several outputs do not come
    // back from a member yet.
    if (c.capability == models::Capability::AudioSeparate) {
      continue;
    }
    if (c.availability == models::Availability::Ready &&
        (task.empty() || fc.shares(task, ""))) {
      features.push_back(models::to_str(c.capability));
    }
  }
  const auto* a = up ? assistant_model() : nullptr;
  const bool chat = a && fc.shares(engine::kOpChat, a->id) &&
                    _store->info(*a).state ==
                        models::InstallState::Installed;
  return {{"ops", ops},
          {"installed", installed},
          {"runs", runs},
          {"features", features},
          {"assistant", chat ? a->id : std::string()},
          // Its context as this Mac's memory gives it -- and its agent
          // model's, which serves a member's agent turns: a sender's
          // agent compacts its conversations to it (DESIGN §9b).
          {"assistant_context", chat ? helper_context_(*a) : 0},
          {"agent_context", [&] {
             const auto* m = up ? agent_model() : nullptr;
             return m && _store->info(*m).state ==
                             models::InstallState::Installed
                        ? helper_context_(*m)
                        : 0;
           }()},
          {"busy", busy_here_()},
          {"machine", {{"chip", _hw.chip},
                       {"ram_gb", _hw.ram_gb()},
                       {"gpu_cores", _hw.gpu_cores}}},
          {"engine", engine_description()},
          {"version", VALTZ_VERSION}};
}

Status
Controller::fleet_serve_(const fleet::Offer& o, const std::string& from,
                         const fs::path& out_dir, engine::JobSink sink)
{
  // A member's agent session over: what this Mac kept for it let go.
  if (o.op == "agent-close") {
    if (_agent) {
      _agent->close_served(from, jget<std::string>(o.params, "context", ""));
    }
    engine::JobEvent done;
    done.job = o.id;
    done.kind = engine::JobEventKind::Finished;
    sink(done);
    return ok_status();
  }
  const Json me = fleet_self_();
  if (!_engine || !_engine->available() || !_engine->supports(o.op) ||
      !listed(jget(me, "ops", Json()), o.op) ||
      (!o.model.empty() && !listed(jget(me, "installed", Json()), o.model))) {
    return make_error(Code::Unsupported, msg::kEngineCannotRun,
                      {{"operation", o.op}});
  }
  // Files of the sender's own are never sent, so never named: a path in
  // a job from elsewhere is refused.
  auto named = own_file_keys();
  for (const char* k : {"mtp_model", "draft_model", "output"}) {
    named.emplace_back(k);
  }
  for (const auto& k : named) {
    if (o.params.contains(k)) {
      return make_error(Code::InvalidArgument,
                        std::format("a fleet job names a file ({})", k));
    }
  }
  engine::JobSpec spec;
  if (o.op == engine::kOpChat) {
    VALTZ_ASSIGN(spec, chat_spec_(o.id, jget<std::string>(o.params, "text",
                                                          ""),
                                  o.inputs,
                                  jget(o.params, "max_new_tokens", 512)));
  } else if (o.op == engine::kOpAgentTurn) {
    VALTZ_ASSIGN(spec, _agent->served_turn(o, from, out_dir));
  } else {
    engine::ModelRef ref;
    Json helpers = Json::object();
    VALTZ_TRY(resolve_build_(o.model, o.params, ref, helpers));
    spec.id = o.id;
    spec.op = o.op;
    spec.model = std::move(ref);
    spec.params = o.params;
    spec.params.update(helpers);
    spec.inputs = o.inputs;
  }
  spec.output_dir = out_dir;
  JobRecord rec;
  rec.id = o.id;
  rec.op = o.op;
  rec.purpose = "fleet";
  rec.title = o.title;
  rec.runner_name = from;
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  const Json about = {{"from", from}, {"title", o.title}, {"op", o.op},
                      {"model", o.model}};
  Json queued = about;
  queued["state"] = "queued";
  post_("fleet.serving", o.id, queued);
  return _engine->submit(
      std::move(spec), [this, sink, about](const engine::JobEvent& ev) {
        Json d = about;
        switch (ev.kind) {
        case engine::JobEventKind::Started:
          _jobs.update(ev.job, [](JobRecord& r) {
            r.state = JobState::Running;
          });
          d["state"] = "running";
          post_("fleet.serving", ev.job, d);
          break;
        case engine::JobEventKind::Progress:
          _jobs.update(ev.job, [&](JobRecord& r) {
            r.progress = std::max(0.0f, ev.progress);
          });
          d["state"] = "running";
          d["progress"] = ev.progress;
          d["phase"] = ev.data;
          post_("fleet.progress", ev.job, d);
          break;
        case engine::JobEventKind::Finished:
        case engine::JobEventKind::Failed:
        case engine::JobEventKind::Cancelled:
          finish_job_(ev.job,
                      ev.kind == engine::JobEventKind::Finished
                          ? JobState::Finished
                      : ev.kind == engine::JobEventKind::Failed
                          ? JobState::Failed
                          : JobState::Cancelled,
                      ev.text);
          d["state"] = engine::to_str(ev.kind);
          d["message"] = ev.text;
          post_("fleet.serving", ev.job, d);
          break;
        default:
          break;
        }
        sink(ev);
      });
}

Json
Controller::fleet_status() const
{
  if (!_fleet) {
    return {{"enabled", false}};
  }
  Json s = _fleet->status();
  s["enabled"] = true;
  s["self"] = fleet_self_();
  // The tasks, each with the models a job of it names: what the offload
  // policies and the sharing are set for.
  const bool up = _engine && _engine->available();
  Json tasks = Json::array();
  for (std::string_view op : kFleetTasks) {
    Json ms = Json::array();
    for (const auto& m : _catalog.models()) {
      if (does_task(m, op)) {
        ms.push_back({{"id", m.id},
                      {"name", m.name},
                      {"family", m.family},
                      {"installed", _store->info(m).state ==
                                        models::InstallState::Installed}});
      }
    }
    tasks.push_back({{"op", std::string(op)},
                     {"runs", up && _engine->supports(op)},
                     {"models", std::move(ms)}});
  }
  s["tasks"] = std::move(tasks);
  return s;
}

Status
Controller::fleet_configure(const Json& j)
{
  if (!_fleet) {
    return make_error(Code::Unsupported, msg::kFleetOff);
  }
  fleet::Config c = _fleet->config();
  auto text = [&](const char* k) {
    std::string v = jget<std::string>(j, k, "");
    const auto a = v.find_first_not_of(" \t\r\n");
    const auto b = v.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string()
                                  : std::string(utf8_prefix(
                                        v.substr(a, b - a + 1), 63));
  };
  if (j.contains("member_name")) {
    if (auto n = text("member_name"); !n.empty()) {
      c.member_name = std::move(n);
    }
  }
  if (j.contains("discoverable")) {
    c.discoverable = jget(j, "discoverable", false);
  }
  if (j.contains("accept")) {
    const auto a = jget<std::string>(j, "accept", "");
    if (a == "always" || a == "ask" || a == "never") {
      c.accept = a;
    }
  }
  if (j.contains("schedule")) {
    c.schedule = fleet::schedule_from_json(j["schedule"]);
  }
  // Offload policies and sharing: those named change, the rest stay. A
  // task is a fleet task; a model, one of the catalog's.
  auto change = [&](const char* table, auto known,
                    std::map<std::string, std::string>& policies,
                    std::set<std::string>& withheld) -> Status {
    const Json off = jget(jget(j, "offload", Json::object()), table,
                          Json::object());
    for (const auto& [k, v] : off.items()) {
      const auto p = v.is_string() ? v.get<std::string>() : std::string();
      if (!known(k)) {
        return make_error(Code::InvalidArgument, msg::kFleetUnknownTask,
                          {{"name", k}});
      }
      if (!p.empty() && !fleet::is_offload_policy(p)) {
        return make_error(Code::InvalidArgument, msg::kFleetUnknownPolicy,
                          {{"policy", p}});
      }
      if (p.empty()) {
        policies.erase(k);
      } else {
        policies[k] = p;
      }
    }
    const Json share = jget(jget(j, "share", Json::object()), table,
                            Json::object());
    for (const auto& [k, v] : share.items()) {
      if (!known(k)) {
        return make_error(Code::InvalidArgument, msg::kFleetUnknownTask,
                          {{"name", k}});
      }
      if (v.is_boolean() && !v.get<bool>()) {
        withheld.insert(k);
      } else {
        withheld.erase(k);
      }
    }
    return ok_status();
  };
  VALTZ_TRY(change("tasks", [](const std::string& op) {
    return std::ranges::find(kFleetTasks, op) != std::end(kFleetTasks);
  }, c.offload_tasks, c.withheld_tasks));
  VALTZ_TRY(change("models", [this](const std::string& id) {
    return _catalog.find(id) != nullptr;
  }, c.offload_models, c.withheld_models));
  // A fleet made or joined -- its name and secret -- or left.
  const std::string secret = jget<std::string>(j, "secret", "");
  if (!secret.empty() && secret.size() < 8) {
    return make_error(Code::InvalidArgument, msg::kFleetSecretShort);
  }
  if (j.contains("fleet")) {
    const std::string name = text("fleet");
    if (name.empty()) {
      c.fleet.clear();
      c.key.clear();
    } else if (secret.empty() && (name != c.fleet || c.key.empty())) {
      return make_error(Code::InvalidArgument, msg::kFleetNeedsSecret);
    } else {
      if (!secret.empty()) {
        c.key = fleet::derive_key(name, secret);
      }
      c.fleet = name;
    }
  } else if (!secret.empty()) {
    if (c.fleet.empty()) {
      return make_error(Code::InvalidArgument, msg::kFleetNameEmpty);
    }
    c.key = fleet::derive_key(c.fleet, secret);
  }
  VALTZ_TRY(_fleet->set_config(c));
  // What it offers told the members at once.
  if (j.contains("share")) {
    _fleet->self_changed();
  }
  post_("fleet.changed", JobId{});
  return ok_status();
}

void
Controller::fleet_browse(bool on)
{
  if (_fleet) {
    _fleet->browse(on);
  }
}

void
Controller::fleet_answer(JobId job, bool accept)
{
  if (_fleet) {
    _fleet->answer(job, accept);
  }
}

void
Controller::fleet_connect(const std::string& host, int port)
{
  if (_fleet) {
    _fleet->connect(host, port);
  }
}

}
