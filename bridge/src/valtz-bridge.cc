#include "valtz-bridge.h"

#include "valtz/base/log.h"
#include "valtz/base/text.h"
#include "valtz/controller/controller.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <set>

namespace valtz::bridge {

namespace fs = std::filesystem;

namespace {

std::string g_create_error;

// A markup's objects listed with it in assets_json up to this many; past
// it, its stamp and count alone (asset_op "markup-objects" has them).
constexpr std::size_t kInlineObjects = 300;

std::string
ok(Json extra = Json::object())
{
  extra["ok"] = true;
  return to_text(extra);
}

std::string
err(const Error& e)
{
  Json j = message_fields(e);   // message, and key + args when it has one
  j["ok"] = false;
  j["code"] = to_str(e.code);
  return to_text(j);
}

std::string
err(Code c, std::string msg)
{
  return err(make_error(c, std::move(msg)));
}

Result<Json>
parse(const std::string& s)
{
  Json j = Json::parse(s, nullptr, /*allow_exceptions=*/false);
  if (j.is_discarded() || !j.is_object()) {
    return make_error(Code::InvalidArgument, "request is not a JSON object");
  }
  return j;
}

template <class IdT>
Result<IdT>
parse_id(const std::string& s)
{
  auto id = IdT::parse(s);
  if (!id) {
    return make_error(Code::InvalidArgument,
                      std::format("'{}' is not an id", s));
  }
  return *id;
}

// Everything the bridge hands out must not throw into Swift.
// `inline`: the asset at each inline picture of the prompt, in order;
// "" (or anything else unreadable) for one that is not an asset.
std::vector<std::optional<AssetId>>
inline_pictures(const Json& j, const char* key = "inline")
{
  std::vector<std::optional<AssetId>> out;
  for (const auto& v : jget(j, key, Json::array())) {
    out.push_back(v.is_string() ? AssetId::parse(v.get<std::string>())
                                : std::nullopt);
  }
  return out;
}

// A request's reference row ("row": asset ids in order) and the prompt
// asset its prompt came from ("prompt_asset").
std::vector<AssetId>
row_of(const Json& j)
{
  std::vector<AssetId> out;
  for (const auto& id : jget(j, "row", std::vector<std::string>{})) {
    if (auto a = AssetId::parse(id)) {
      out.push_back(*a);
    }
  }
  return out;
}

std::optional<AssetId>
prompt_asset_of(const Json& j)
{
  return AssetId::parse(jget<std::string>(j, "prompt_asset", ""));
}

template <class Fn>
std::string
guarded(Fn&& fn)
{
  try {
    return fn();
  } catch (const std::exception& e) {
    return err(Code::Internal, e.what());
  } catch (...) {
    return err(Code::Internal, "unknown exception");
  }
}

}

struct Core::Impl {
  std::unique_ptr<Controller> ctl;

  // Clip stacks drawn for the player (media::StackRenderer), by plan
  // token: a compositor draws with one while the app makes the next.
  std::mutex plans_mu;
  std::unordered_map<std::uint64_t,
                     std::shared_ptr<const media::StackRenderer>> plans;
  std::uint64_t next_plan = 0;

  std::shared_ptr<const media::StackRenderer>
  plan(std::uint64_t token)
  {
    std::lock_guard lk(plans_mu);
    auto it = plans.find(token);
    return it == plans.end() ? nullptr : it->second;
  }

  // Preview frames waiting to be fetched by token. Bounded: an app that
  // stops fetching loses old frames, not memory.
  std::mutex mu;
  // Each entry may hold an engine buffer (engine/tensor.h), so keep few.
  std::deque<std::pair<std::uint64_t, engine::TensorPtr>> previews;
  std::uint64_t next_token = 0;

  // The preview behind `token`, taken out of the queue: the caller holds
  // the engine's buffer only as long as it converts it.
  engine::TensorPtr
  take(std::uint64_t token)
  {
    std::lock_guard lk(mu);
    for (auto it = previews.begin(); it != previews.end(); ++it) {
      if (it->first == token) {
        engine::TensorPtr t = std::move(it->second);
        previews.erase(it);
        return t;
      }
    }
    return nullptr;
  }
};

Core*
Core::create(const std::string& config_json)
{
  try {
    ControllerConfig cfg;
    Json j = Json::parse(config_json.empty() ? "{}" : config_json, nullptr,
                         false);
    if (j.is_object()) {
      cfg.with_engine = jget(j, "with_engine", true);
      cfg.language = jget<std::string>(j, "language", "");
      // A member of a fleet (DESIGN §11); "fleet_config": where its
      // configuration is kept, empty the standard place.
      cfg.fleet = jget(j, "fleet", false);
      cfg.fleet_config = jget<std::string>(j, "fleet_config", "");
      for (const auto& r : jget(j, "model_roots",
                                std::vector<std::string>{})) {
        cfg.model_roots.emplace_back(r);
      }
    }
    auto c = Controller::create(cfg);
    if (!c.ok()) {
      g_create_error = c.error().message;
      return nullptr;
    }
    auto* core = new Core();
    core->_impl = new Impl();
    core->_impl->ctl = std::move(*c);
    return core;
  } catch (const std::exception& e) {
    g_create_error = e.what();
    return nullptr;
  }
}

std::string
Core::create_error()
{
  return g_create_error;
}

std::string
Core::version() const
{
  return _impl->ctl->version();
}

std::string
Core::engine() const
{
  return _impl->ctl->engine_description();
}

std::string
Core::hardware_json() const
{
  Json j = _impl->ctl->hardware();
  const auto* a = _impl->ctl->assistant_model();
  j["assistant_model"] = a ? a->id : "";
  return to_text(j);
}

std::string
Core::capability_tree_json() const
{
  return guarded([&] { return ok(_impl->ctl->capability_tree()); });
}

std::string
Core::extensions_json() const
{
  return guarded([&] { return ok(_impl->ctl->extensions()); });
}

std::string
Core::storage_report_json() const
{
  return guarded([&] { return ok(_impl->ctl->storage_report()); });
}

std::string
Core::link_model(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto r = _impl->ctl->link_model(jget<std::string>(*j, "model", ""),
                                    jget<std::string>(*j, "path", ""));
    if (!r.ok()) {
      return err(r.error());
    }
    if (!jget(*r, "linked", false)) {
      // Refused: why, first, and the whole check.
      Json out = jget(*r, "notes", Json::array()).empty()
          ? Json::object()
          : (*r)["notes"][0];
      out.erase("level");
      out["ok"] = false;
      out["code"] = to_str(Code::InvalidArgument);
      out["check"] = *r;
      return to_text(out);
    }
    return ok({{"check", *r}});
  });
}

std::string
Core::check_model(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto r = _impl->ctl->check_model(jget<std::string>(*j, "model", ""),
                                     jget<std::string>(*j, "path", ""));
    return r.ok() ? ok({{"check", *r}}) : err(r.error());
  });
}

std::string
Core::unlink_model(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->unlink_model(jget<std::string>(*j, "model", ""),
                                       jget(*j, "parts", false));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::fleet_status() const
{
  return guarded([&] { return ok(_impl->ctl->fleet_status()); });
}

std::string
Core::fleet_configure(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->fleet_configure(*j);
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::fleet_browse(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    _impl->ctl->fleet_browse(jget(*j, "on", false));
    return ok();
  });
}

std::string
Core::fleet_answer(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto id = JobId::parse(jget<std::string>(*j, "job", ""));
    if (!id) {
      return err(Code::InvalidArgument, "bad job id");
    }
    _impl->ctl->fleet_answer(*id, jget(*j, "accept", false));
    return ok();
  });
}

std::string
Core::assistants_json() const
{
  return guarded([&] { return ok(_impl->ctl->assistants()); });
}

std::string
Core::choose_assistant(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    // {"model"}: the helper chosen; {"keep_loaded"}: how long it stays;
    // {"drafter", "drafter_bits"}: what it drafts with; {"video_every"}:
    // a video summary's seconds a frame (0 Auto); {"video_size"}: its
    // frames' size (standard | high | highest); {"context"}: its
    // context at most, tokens (0 Auto).
    if (j->contains("video_size")) {
      auto st = _impl->ctl->set_video_size(
          jget<std::string>(*j, "video_size", "standard"));
      if (!st.ok() || (!j->contains("model") &&
                       !j->contains("keep_loaded") &&
                       !j->contains("drafter") &&
                       !j->contains("video_every") &&
                       !j->contains("context"))) {
        return st.ok() ? ok() : err(st.error());
      }
    }
    if (j->contains("context")) {
      auto st = _impl->ctl->set_assistant_context(
          jget<std::int64_t>(*j, "context", 0));
      if (!st.ok() || (!j->contains("model") &&
                       !j->contains("keep_loaded") &&
                       !j->contains("drafter") &&
                       !j->contains("video_every"))) {
        return st.ok() ? ok() : err(st.error());
      }
    }
    if (j->contains("video_every")) {
      auto st = _impl->ctl->set_video_every(jget(*j, "video_every", 0.0));
      if (!st.ok() || (!j->contains("model") &&
                       !j->contains("keep_loaded") &&
                       !j->contains("drafter"))) {
        return st.ok() ? ok() : err(st.error());
      }
    }
    if (j->contains("drafter")) {
      auto st = _impl->ctl->set_assistant_drafter(
          jget<std::string>(*j, "drafter", "mtp"),
          jget(*j, "drafter_bits", 8));
      if (!st.ok() || (!j->contains("model") && !j->contains("keep_loaded"))) {
        return st.ok() ? ok() : err(st.error());
      }
    }
    if (j->contains("keep_loaded")) {
      auto st = _impl->ctl->set_assistant_keep_loaded(
          jget(*j, "keep_loaded", Controller::kDefaultKeepLoaded));
      if (!st.ok() || !j->contains("model")) {
        return st.ok() ? ok() : err(st.error());
      }
    }
    auto st = _impl->ctl->choose_assistant(
        jget<std::string>(*j, "model", ""));
    return st.ok() ? ok() : err(st.error());
  });
}

// ---- the agent ------------------------------------------------------------

std::string
Core::agent_start(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = ProjectId::parse(jget<std::string>(*j, "project", ""));
    if (!pid) {
      return err(Code::InvalidArgument, "bad project id");
    }
    AgentRequest req;
    req.project = *pid;
    req.prompt = jget<std::string>(*j, "prompt", "");
    if (auto c = AssetId::parse(jget<std::string>(*j, "composition", ""))) {
      req.composition = *c;
    }
    req.row = row_of(*j);
    req.review = jget(*j, "review", true);
    req.backend = jget<std::string>(*j, "backend", "");
    req.stage = jget(*j, "stage", Json::object());
    auto id = _impl->ctl->agent_start(std::move(req));
    if (!id.ok()) {
      return err(id.error());
    }
    return ok({{"agent", *id}});
  });
}

std::string
Core::agent_say(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->agent_say(jget<std::string>(*j, "agent", ""),
                                    jget<std::string>(*j, "text", ""),
                                    jget(*j, "stage", Json::object()));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::agent_review(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->agent_review(
        jget<std::string>(*j, "agent", ""), jget(*j, "step", 0),
        jget<std::string>(*j, "decision", "approve"),
        jget(*j, "args", Json()), jget<std::string>(*j, "note", ""));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::agent_control(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    const auto id = jget<std::string>(*j, "agent", "");
    const auto action = jget<std::string>(*j, "action", "");
    auto& c = *_impl->ctl;
    Status st = action == "pause"    ? c.agent_pause(id)
              : action == "resume" ? c.agent_resume(id)
              : action == "stop"   ? c.agent_stop(id)
              : action == "review" ? c.agent_set_review(id, true)
              : action == "auto"   ? c.agent_set_review(id, false)
                                   : make_error(Code::InvalidArgument,
                                                "no such agent action");
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::agent_state(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    Json st = _impl->ctl->agent_state(jget<std::string>(*j, "agent", ""));
    if (st.is_null()) {
      return err(Code::NotFound, "no such agent session");
    }
    // Each step's arguments and result as text, as the app shows them;
    // an ask_user's question by itself.
    for (auto& step : st["steps"]) {
      step["args_text"] = to_text(jget(step, "args", Json::object()));
      if (step.contains("result")) {
        step["result_text"] = to_text(step["result"]);
      }
      if (jget<std::string>(step, "tool", "") == "ask_user") {
        step["question"] = jget<std::string>(
            jget(step, "args", Json::object()), "question", "");
      }
    }
    return ok({{"session", std::move(st)}});
  });
}

std::string
Core::agents(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = ProjectId::parse(jget<std::string>(*j, "project", ""));
    if (!pid) {
      return err(Code::InvalidArgument, "bad project id");
    }
    return ok({{"agents", _impl->ctl->agents(*pid)}});
  });
}

std::string
Core::agent_settings() const
{
  return guarded([&] { return ok(_impl->ctl->agent_settings()); });
}

std::string
Core::set_agent_settings(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->set_agent_settings(*j);
    return st.ok() ? ok(_impl->ctl->agent_settings()) : err(st.error());
  });
}

std::string
Core::set_agent_key(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    std::string key = jget<std::string>(*j, "key", "");
    auto st = _impl->ctl->set_agent_key(jget<std::string>(*j, "provider", ""),
                                        key);
    std::fill(key.begin(), key.end(), '\0');
    return st.ok() ? ok(_impl->ctl->agent_settings()) : err(st.error());
  });
}

std::string
Core::log_since(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    return ok(_impl->ctl->log_since(
        jget<std::uint64_t>(*j, "seq", 0),
        jget<std::size_t>(*j, "max", 4096)));
  });
}

void
Core::clear_log()
{
  _impl->ctl->clear_log();
}

std::string
Core::machine_status_json() const
{
  return guarded([&] { return to_text(_impl->ctl->machine_status()); });
}

std::string
Core::memory_held_json() const
{
  return guarded([&] { return to_text(_impl->ctl->memory_held()); });
}

std::string
Core::wired_limit_json() const
{
  return guarded([&] { return ok(_impl->ctl->wired_limit()); });
}

std::string
Core::set_wired_limit(std::uint64_t bytes)
{
  return guarded([&] {
    Status st = _impl->ctl->set_wired_limit(bytes);
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::gpu_thermal_json(int window_ms) const
{
  return guarded([&] {
    return to_text(_impl->ctl->gpu_thermal(window_ms));
  });
}

std::string
Core::capabilities_json() const
{
  return guarded([&] {
    Json arr = Json::array();
    for (const auto& s : _impl->ctl->capabilities()) {
      arr.push_back(s);
    }
    return to_text(arr);
  });
}

std::string
Core::auto_json() const
{
  return guarded([&] {
    const auto& cat = _impl->ctl->catalog();
    Json out = Json::object();
    for (const auto& a : _impl->ctl->auto_models()) {
      Json models = Json::array();
      for (const auto& id : a.order) {
        const auto* m = cat.find(id);
        if (!m) { continue; }
        models.push_back({{"model", id}, {"name", m->name},
                          {"runs", _impl->ctl->runs(*m, a.modality, a.op)}});
      }
      out[a.modality][a.op] = {{"chosen", a.chosen},
                               {"waits", a.waits},
                               {"models", std::move(models)}};
    }
    return to_text(out);
  });
}

std::string
Core::catalog_json() const
{
  Json arr = Json::array();
  const Controller& c = *_impl->ctl;
  for (const auto& m : c.catalog().models()) {
    Json j = m;
    // Each preset as it runs here (the Favor selector's Fast, Med, Fine):
    // its steps, its few-step adapter, the LoRAs it needs and lacks.
    Json presets = Json::object();
    Json steps = Json::object();
    for (const char* p : {"speed", "balanced", "quality"}) {
      presets[p] = c.preset_summary(m, p);
      steps[p] = presets[p]["steps"];
    }
    j["steps"] = std::move(steps);
    j["presets"] = std::move(presets);
    const auto* t = c.turbo_adapter(m);
    j["turbo"] = t ? t->name : std::string();
    arr.push_back(std::move(j));
  }
  return to_text(arr);
}

std::string
Core::jobs_json() const
{
  return to_text(_impl->ctl->jobs().to_json());
}

std::string
Core::tasks_json() const
{
  return guarded([&] { return ok({{"tasks", _impl->ctl->tasks()}}); });
}

void
Core::rescan_models()
{
  _impl->ctl->rescan_models();
}

std::string
Core::default_project_path() const
{
  if (const char* over = std::getenv("VALTZ_DEFAULT_PROJECT")) {
    return over;  // dev / test runs
  }
  return _impl->ctl->paths().default_project().string();
}

std::string
Core::paths_json() const
{
  const AppPaths p = _impl->ctl->paths();
  Json roots = Json::array();
  for (const auto& r : _impl->ctl->model_store().roots()) {
    roots.push_back(r.string());
  }
  auto& c = _impl->ctl->cache();
  return to_text(Json{{"projects", p.projects.string()},
              {"default_project", p.default_project().string()},
              {"models", p.models.string()},
              {"model_roots", roots},
              {"cache", c.root().string()},
              {"cache_bytes", c.size_bytes()},
              {"cache_budget_bytes", c.budget_bytes()},
              {"cache_internal", c.on_internal_volume()},
              {"engine", p.engine.string()}});
}

std::string
Core::locations_json() const
{
  return guarded([&] { return ok(_impl->ctl->locations()); });
}

std::string
Core::set_location(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto r = _impl->ctl->set_location(
        jget<std::string>(*j, "what", ""),
        jget<std::string>(*j, "path", ""),
        jget<std::string>(*j, "how", "move"));
    if (!r.ok()) {
      return err(r.error());
    }
    return ok({{"job", r->is_nil() ? Json() : Json(r->str())}});
  });
}

std::string
Core::thumbnail(const std::string& project_id, const std::string& asset_id,
                std::int32_t max_px, bool plain)
{
  return guarded([&] {
    auto pid = parse_id<ProjectId>(project_id);
    auto aid = parse_id<AssetId>(asset_id);
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto path = _impl->ctl->thumbnail(*pid, *aid, max_px, plain);
    return path.ok() ? ok({{"path", path->string()}}) : err(path.error());
  });
}

std::string
Core::create_project(const std::string& path, const std::string& name)
{
  return guarded([&] {
    auto r = _impl->ctl->create_project(path, name);
    return r.ok() ? ok({{"project", r->str()}}) : err(r.error());
  });
}

std::string
Core::open_project(const std::string& path)
{
  return guarded([&] {
    auto r = _impl->ctl->open_project(path);
    if (!r.ok()) {
      return err(r.error());
    }
    auto* p = _impl->ctl->project(*r);
    Json out = {{"project", r->str()}, {"name", p ? p->name() : ""}};
    // What the open found: resumed changes, an old package, extensions
    // its save holds data of that are not here.
    const Json found = _impl->ctl->open_report(*r);
    for (const char* k : {"recovered", "legacy", "missing_extensions"}) {
      if (found.contains(k)) {
        out[k] = found[k];
      }
    }
    return ok(std::move(out));
  });
}

std::string
Core::create_untitled(const std::string& dir, const std::string& name)
{
  return guarded([&] {
    auto r = _impl->ctl->create_untitled(dir, name);
    return r.ok() ? ok({{"project", r->str()}}) : err(r.error());
  });
}

std::string
Core::close_project(const std::string& project_id)
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto st = _impl->ctl->close_project(*id, false);
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::discard_project(const std::string& project_id)
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto st = _impl->ctl->close_project(*id, true);
    return st.ok() ? ok() : err(st.error());
  });
}

namespace {

template <class Fn>
std::string
project_call(const std::string& project_id, Fn&& fn)
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    Status st = fn(*id);
    return st.ok() ? ok({{"project", id->str()}}) : err(st.error());
  });
}

}

std::string
Core::save_project(const std::string& project_id)
{
  return project_call(project_id, [&](ProjectId id) {
    return _impl->ctl->save_project(id);
  });
}

std::string
Core::save_project_as(const std::string& project_id, const std::string& path)
{
  return project_call(project_id, [&](ProjectId id) {
    return _impl->ctl->save_project_as(id, path);
  });
}

std::string
Core::revert_project(const std::string& project_id)
{
  return project_call(project_id, [&](ProjectId id) {
    return _impl->ctl->revert_project(id);
  });
}

std::string
Core::undo(const std::string& project_id)
{
  return project_call(project_id, [&](ProjectId id) {
    return _impl->ctl->undo(id);
  });
}

std::string
Core::redo(const std::string& project_id)
{
  return project_call(project_id, [&](ProjectId id) {
    return _impl->ctl->redo(id);
  });
}

std::string
Core::project_state(const std::string& project_id) const
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    return ok({{"state", _impl->ctl->project_state(*id)}});
  });
}

std::string
Core::set_project_ephemeral(const std::string& project_id, bool ephemeral)
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto st = _impl->ctl->set_ephemeral(*id, ephemeral);
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::view_state(const std::string& project_id) const
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto v = _impl->ctl->view_state(*id);
    return v.ok() ? ok({{"view", *v}}) : err(v.error());
  });
}

std::string
Core::set_view_state(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto id = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!id.ok()) {
      return err(id.error());
    }
    auto st = _impl->ctl->set_view_state(*id, jget(*j, "view",
                                                   Json::object()));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::history_json(const std::string& project_id) const
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto states = _impl->ctl->history(*id);
    if (!states.ok()) {
      return err(states.error());
    }
    Json arr = Json::array();
    for (const auto& s : *states) {
      Json j = s.entry;
      j.erase("picture");
      j["path"] = s.file.string();
      arr.push_back(std::move(j));
    }
    return ok({{"entries", std::move(arr)}});
  });
}

std::string
Core::assets_json(const std::string& project_id) const
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto* p = _impl->ctl->project(*id);
    if (!p) {
      return err(Code::NotFound, "project is not open");
    }
    // Before the records: a write while they are read makes the next
    // revision another, so the list is read again.
    const std::string revision = p->revision();
    auto assets = p->assets();
    if (!assets.ok()) {
      return err(assets.error());
    }
    // Each asset by id, for a composition's stand-in below: read once.
    std::map<AssetId, std::size_t> listed;
    for (std::size_t i = 0; i < assets->size(); ++i) {
      listed[(*assets)[i].id] = i;
    }
    Json arr = Json::array();
    for (auto& a : *assets) {
      // A markup's OBJECTS travel apart (2026-10-23): 16 000 of them were
      // 5 MB in every list, read again -- and decoded by the app -- at
      // each edit. Its stamp and count always; the objects themselves only
      // for a few hundred (the app's copy follows its own edits, fetching
      // the rest by stamp: asset_op "markup-objects").
      Json objects;
      if (a.markup) {
        objects = std::move(a.markup->objects);
        a.markup->objects = Json::array();
      }
      Json j = a;
      if (a.markup) {
        Json& mk = j["markup"];
        mk["stamp"] = a.markup->stamp.empty()
                          ? Controller::objects_stamp(objects)
                          : a.markup->stamp;
        mk["count"] = objects.size();
        if (objects.size() <= kInlineObjects) {
          mk["objects"] = std::move(objects);
        } else {
          mk.erase("objects");
        }
      }
      // Its head read once, its file and words found from it.
      std::optional<project::AssetVersion> head;
      if (a.head > 0) {
        if (auto v = p->version(a.id, a.head); v.ok()) {
          head = std::move(*v);
        }
      }
      if (head) {
        j["info"] = head->info;
        // How long it took to make (controller/job-timing.h).
        if (!head->timing.empty()) {
          j["timing"] = head->timing;
        }
        // What it made beside its file: a song's score (ABC).
        if (const auto sc = jget<std::string>(head->outputs, "score", "");
            !sc.empty()) {
          j["score"] = sc;
        }
        if (auto path = p->media_path(a, *head); path.ok()) {
          j["path"] = path->string();
        }
      }
      // A composition: the frame its layers lie on, a timeline's length
      // (its frames), and whether it is the project's.
      const bool comp = a.cls == project::AssetClass::Still ||
                        a.cls == project::AssetClass::Composition;
      if (comp && a.kind != project::AssetKind::Audio) {
        if (auto f = _impl->ctl->own_frame(*id, a.id); f.ok()) {
          j["own_frame"] = {{"w", f->width}, {"h", f->height}};
        }
      }
      if (a.cls == project::AssetClass::Composition) {
        if (auto n = _impl->ctl->composition_length(*id, a.id); n.ok()) {
          j["length"] = *n;
        }
      }
      if (comp && _impl->ctl->project_composition(*id) == a.id) {
        j["project"] = true;
      }
      // "class" is a word Swift keeps: the app reads it as this.
      j["asset_class"] = project::to_str(a.cls);
      // A composition has no file of its own; the app is handed its
      // STAND-IN -- its bottom-most take of its own kind (a still's
      // picture, a timeline's clip or sound) -- for a poster, a size,
      // and what the player plays it as. The core draws the rest.
      if (comp && !j.contains("path")) {
        for (const auto& l : a.layers) {
          if (!l.source) {
            continue;
          }
          // The list's own (a markup of thousands read again was 40 ms).
          const auto at = listed.find(*l.source);
          const project::Asset* s =
              at != listed.end() ? &(*assets)[at->second] : nullptr;
          if (!s || s->kind != a.kind ||
              (s->cls != project::AssetClass::Flat &&
               s->cls != project::AssetClass::Generated)) {
            continue;
          }
          auto v = p->version(s->id, l.source_version > 0 ? l.source_version
                                                          : s->head);
          if (auto path = v.ok() ? p->media_path(*s, *v)
                                 : Result<fs::path>(v.error());
              path.ok()) {
            j["path"] = path->string();
            j["stand_in"] = true;
            j["info"] = v->info;
          }
          break;
        }
      }
      if (a.linked) {
        auto chk = p->check_link(a.id);  // stat only; cheap
        j["link_state"] = chk.ok() ? project::to_str(chk->state) : "missing";
        j.erase("link");  // the bookmark is of no use to the UI
      }
      // A text: its words -- a prompt's tags and all, and how much it
      // has made (once anything, it is kept as it is).
      if (a.kind == project::AssetKind::Text && head) {
        if (auto t = p->read_text(a, *head); t.ok()) {
          j["text"] = std::string(utf8_prefix(*t, 65536));
        }
      }
      // Of what came from it, the prompts the assistant's suggestions
      // made from it (Controller::apply_suggestion) apart.
      if (Controller::is_prompt(a)) {
        int uses = 0;
        int enhanced = 0;
        if (auto deps = p->dependents(a.id, false); deps.ok()) {
          for (const auto& d : *deps) {
            auto da = p->asset(d);
            ++(da.ok() && Controller::is_prompt(*da) ? enhanced : uses);
          }
        }
        j["uses"] = uses;
        j["enhanced"] = enhanced;
      }
      if (a.origin == project::Origin::Derived) {
        auto st = p->staleness(a.id);
        j["stale"] = st.ok() ? project::to_str(st->reason) : "unknown";
        if (auto r = p->recipe(a.recipe); r.ok()) {
          j["recipe"] = *r;
        }
      }
      arr.push_back(std::move(j));
    }
    auto folders = p->folders();
    return ok({{"assets", arr},
               {"folders", folders.ok() ? *folders : Json::array()},
               {"revision", revision}});
  });
}

std::string
Core::atlas_json(const std::string& project_id) const
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto* p = _impl->ctl->project(*id);
    if (!p) {
      return err(Code::NotFound, "project is not open");
    }
    // Before the records, as the asset list's.
    const std::string revision = p->revision();
    auto graph = _impl->ctl->atlas(*id);
    if (!graph.ok()) {
      return err(graph.error());
    }
    Json j = std::move(*graph);
    j["revision"] = revision;
    return ok(std::move(j));
  });
}

std::string
Core::assets_revision(const std::string& project_id) const
{
  return guarded([&] {
    auto id = parse_id<ProjectId>(project_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto* p = _impl->ctl->project(*id);
    if (!p) {
      return err(Code::NotFound, "project is not open");
    }
    return ok({{"revision", p->revision()}});
  });
}

std::string
Core::asset_op(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    auto& c = *_impl->ctl;
    const std::string op = jget<std::string>(*j, "op", "");
    const std::string name = jget<std::string>(*j, "name", "");
    const std::string folder = jget<std::string>(*j, "folder", "");
    auto asset = [&](const char* key) {
      return parse_id<AssetId>(jget<std::string>(*j, key, ""));
    };
    // A MARKUP asset's objects -- {"asset"} -> {"objects", "stamp"}: what
    // the asset list leaves out past a few hundred (assets_json).
    if (op == "markup-objects") {
      auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
      if (!aid.ok()) {
        return err(aid.error());
      }
      auto r = c.markup_objects(*pid, *aid);
      if (!r.ok()) {
        return err(r.error());
      }
      return ok({{"objects", std::move(r->first)},
                 {"stamp", std::move(r->second)}});
    }
    if (op == "create-folder") {
      auto f = c.create_folder(*pid, name);
      return f.ok() ? ok({{"folder", *f}}) : err(f.error());
    }
    if (op == "rename-folder") {
      auto st = c.rename_folder(*pid, folder, name);
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "delete-folder") {
      auto st = c.delete_folder(*pid, folder);
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "new-composition") {
      // {"still": bool, "width", "height" (0 x 0: sound alone),
      //  "rate_num", "rate_den", "as_project", "claim" (false: a blank
      //  one of the asset list's, never the project's by itself)}
      const bool still = jget(*j, "still", false);
      const auto num = jget<std::int64_t>(*j, "rate_num", 0);
      const auto den = jget<std::int64_t>(*j, "rate_den", 1);
      auto made = c.create_composition(
          *pid,
          still ? project::AssetClass::Still
                : project::AssetClass::Composition,
          {jget<std::int32_t>(*j, "width", 0),
           jget<std::int32_t>(*j, "height", 0)},
          num > 0 && den > 0 ? Rational(num, den) : Rational(0, 1), name,
          jget(*j, "as_project", false), jget(*j, "claim", true));
      return made.ok() ? ok({{"asset", made->str()}}) : err(made.error());
    }
    // The project's OUTPUT (Controller::project_output): {"output":
    // {"color", "fps": [num, den], "channels", "sample_rate"}}.
    if (op == "output") {
      auto o = c.project_output(*pid);
      return o.ok() ? ok({{"output", Json(*o)}}) : err(o.error());
    }
    if (op == "set-output") {
      auto st = c.set_project_output(
          *pid, jget(*j, "output", Json::object())
                    .get<project::OutputSettings>());
      return st.ok() ? ok() : err(st.error());
    }
    // Information › Project's Set Up: {"still": bool, "width", "height"
    // (0 x 0: sound alone), "output"} -> {"asset"}.
    if (op == "set-up-project") {
      const bool still = jget(*j, "still", false);
      auto made = c.set_up_project(
          *pid,
          still ? project::AssetClass::Still
                : project::AssetClass::Composition,
          {jget<std::int32_t>(*j, "width", 0),
           jget<std::int32_t>(*j, "height", 0)},
          jget(*j, "output", Json::object())
              .get<project::OutputSettings>());
      return made.ok() ? ok({{"asset", made->str()}}) : err(made.error());
    }
    // The assistant's suggestion applied (Controller::apply_suggestion):
    // {"prompt", "inline", "prompt_asset", "prompt_name" -- the box's
    // words before it, as Start sends them -- "enhanced",
    // "enhanced_inline", "row", "for"} -> {"before", "asset"}: the words
    // before it kept, the suggestion a prompt made from them.
    if (op == "apply-suggestion") {
      Controller::SuggestionApplied req;
      req.project = *pid;
      req.before = jget<std::string>(*j, "prompt", "");
      req.before_inline = inline_pictures(*j);
      req.from = prompt_asset_of(*j);
      req.enhanced = jget<std::string>(*j, "enhanced", "");
      req.enhanced_inline = inline_pictures(*j, "enhanced_inline");
      req.row = row_of(*j);
      req.name = jget<std::string>(*j, "prompt_name", "");
      req.written_for = jget<std::string>(*j, "for", "");
      auto r = c.apply_suggestion(req);
      return r.ok() ? ok({{"before", r->before.str()},
                          {"asset", r->enhanced.str()}})
                    : err(r.error());
    }
    auto aid = asset("asset");
    if (!aid.ok()) {
      return err(aid.error());
    }
    if (op == "move" || op == "remove") {
      auto st = op == "move" ? c.move_asset(*pid, *aid, folder)
                             : c.remove_asset(*pid, *aid);
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "rename") {
      auto st = c.rename_asset(*pid, *aid, name);
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "project") {
      auto st = c.set_project_composition(*pid, *aid);
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "place") {
      // "join": the job it is the result of (its undo command).
      std::optional<JobId> job;
      if (const auto s = jget<std::string>(*j, "join", ""); !s.empty()) {
        job = JobId::parse(s);
      }
      auto placed = c.place_in_project(
          *pid, *aid, jget<std::string>(*j, "layer", ""), job);
      if (!placed.ok()) {
        return err(placed.error());
      }
      // Another kind than the project's is not placed.
      return *placed ? ok({{"asset", (*placed)->str()}, {"placed", true}})
                     : ok({{"placed", false}});
    }
    if (op == "instantiate") {
      std::optional<AssetId> onto;
      if (!jget<std::string>(*j, "onto", "").empty()) {
        auto o = asset("onto");
        if (!o.ok()) {
          return err(o.error());
        }
        onto = *o;
      }
      // The layer it goes to ("" is layer 0): present or not.
      std::optional<std::string> at;
      if (j->contains("at")) {
        at = jget<std::string>(*j, "at", "");
      }
      // "new_layer": a drop on the timeline -- always a layer of its own,
      // where it was put.
      auto put = c.instantiate(*pid, *aid, onto, at,
                               jget<std::int64_t>(*j, "offset", 0),
                               !jget(*j, "new_layer", false));
      return put.ok() ? ok({{"asset", put->first.str()},
                            {"layer", put->second}})
                      : err(put.error());
    }
    if (op == "place-result") {
      // A generation's result landing (Controller::place_result).
      std::optional<AssetId> onto;
      if (!jget<std::string>(*j, "onto", "").empty()) {
        auto o = asset("onto");
        if (!o.ok()) {
          return err(o.error());
        }
        onto = *o;
      }
      std::optional<std::string> at;
      if (j->contains("at")) {
        at = jget<std::string>(*j, "at", "");
      }
      std::optional<JobId> job;
      if (const auto s = jget<std::string>(*j, "join", ""); !s.empty()) {
        job = JobId::parse(s);
      }
      auto put = c.place_result(*pid, *aid, onto, at,
                                jget<std::int64_t>(*j, "offset", 0), job);
      return put.ok() ? ok({{"asset", put->first.str()},
                            {"layer", put->second}})
                      : err(put.error());
    }
    if (op == "guide") {
      auto g = c.continuation_guide(
          *pid, *aid, jget(*j, "seconds", 0.0),
          jget<std::string>(*j, "model", ""), name,
          jget<std::string>(*j, "capture_name", ""));
      return g.ok() ? ok(*g) : err(g.error());
    }
    Result<AssetId> made = make_error(Code::InvalidArgument,
                                      "op is not an asset-list operation");
    if (op == "grab") {
      made = c.grab_frame(*pid, *aid, jget<std::int64_t>(*j, "frame", 0),
                          name);
    } else if (op == "modify") {
      made = c.derive_modified(*pid, *aid, name);
    } else if (op == "capture") {
      made = c.capture(*pid, *aid,
                       jget(*j, "layers", std::vector<std::string>{}), name);
    } else if (op == "flatten") {
      made = c.flatten_asset(*pid, *aid, name);
    }
    return made.ok() ? ok({{"asset", made->str()}}) : err(made.error());
  });
}

std::string
Core::versions_json(const std::string& project_id,
                    const std::string& asset_id) const
{
  return guarded([&] {
    auto pid = parse_id<ProjectId>(project_id);
    auto aid = parse_id<AssetId>(asset_id);
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto* p = _impl->ctl->project(*pid);
    if (!p) {
      return err(Code::NotFound, "project is not open");
    }
    auto vs = p->versions(*aid);
    if (!vs.ok()) {
      return err(vs.error());
    }
    Json arr = Json::array();
    for (const auto& v : *vs) {
      Json j = v;
      if (auto path = p->media_path(*aid, v.number); path.ok()) {
        j["path"] = path->string();
      }
      arr.push_back(std::move(j));
    }
    return ok({{"versions", arr}});
  });
}

std::string
Core::media_path(const std::string& project_id, const std::string& asset_id,
                 std::uint32_t version) const
{
  return guarded([&] {
    auto pid = parse_id<ProjectId>(project_id);
    auto aid = parse_id<AssetId>(asset_id);
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto* p = _impl->ctl->project(*pid);
    if (!p) {
      return err(Code::NotFound, "project is not open");
    }
    auto path = p->media_path(*aid, version);
    return path.ok() ? ok({{"path", path->string()}}) : err(path.error());
  });
}

std::string
Core::import_files(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    std::vector<fs::path> files;
    for (const auto& s : jget(*j, "paths", std::vector<std::string>{})) {
      files.emplace_back(s);
    }
    project::ImportOptions opts;
    auto placement = jget<std::string>(*j, "placement", "auto");
    opts.placement = placement == "copy"   ? project::Placement::Copy
                     : placement == "link" ? project::Placement::Link
                                           : project::Placement::Auto;
    auto r = _impl->ctl->import_files(*pid, std::move(files), opts);
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::generate_image(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    GenerateImageRequest req;
    req.project = *pid;
    req.prompt = jget<std::string>(*j, "prompt", "");
    req.negative = jget<std::string>(*j, "negative", "");
    req.model = jget<std::string>(*j, "model", "");
    req.name = jget<std::string>(*j, "name", "");
    req.width = jget(*j, "width", 0);
    req.height = jget(*j, "height", 0);
    req.steps = jget(*j, "steps", 0);
    req.preference = jget<std::string>(*j, "preference", "balanced");
    req.tuning = jget(*j, "tuning", Json::object());
    req.seed = jget<std::int64_t>(*j, "seed", -1);
    req.preview = jget(*j, "preview", true);
    req.mode = jget<std::string>(*j, "mode", "auto");
    if (auto b = AssetId::parse(jget<std::string>(*j, "base", ""))) {
      req.base = *b;
    }
    req.base_adjust = media::adjustments_from_json(
        jget(*j, "base_adjust", Json::object()));
    req.base_crop = media::crop_from_json(
        jget(*j, "base_crop", Json::object()));
    for (const auto& s : jget(*j, "references",
                              std::vector<std::string>{})) {
      if (auto id = AssetId::parse(s)) {
        req.references.push_back(*id);
      }
    }
    req.inline_pictures = inline_pictures(*j);
    req.row = row_of(*j);
    req.prompt_asset = prompt_asset_of(*j);
    req.prompt_name = jget<std::string>(*j, "prompt_name", "");
    auto r = _impl->ctl->generate_image(std::move(req));
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::generate_video(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    GenerateVideoRequest req;
    req.project = *pid;
    req.prompt = jget<std::string>(*j, "prompt", "");
    req.model = jget<std::string>(*j, "model", "");
    req.name = jget<std::string>(*j, "name", "");
    req.width = jget(*j, "width", 0);
    req.height = jget(*j, "height", 0);
    req.frames = jget(*j, "frames", 0);
    req.fps = jget(*j, "fps", 0.0);
    req.steps = jget(*j, "steps", 0);
    req.preference = jget<std::string>(*j, "preference", "balanced");
    req.tuning = jget(*j, "tuning", Json::object());
    req.seed = jget<std::int64_t>(*j, "seed", -1);
    req.turbo = jget(*j, "turbo", true);
    if (auto f = AssetId::parse(jget<std::string>(*j, "first", ""))) {
      req.first = *f;
    }
    if (auto l = AssetId::parse(jget<std::string>(*j, "last", ""))) {
      req.last = *l;
    }
    req.first_adjust = media::adjustments_from_json(
        jget(*j, "first_adjust", Json::object()));
    req.first_crop = media::crop_from_json(
        jget(*j, "first_crop", Json::object()));
    for (const auto& id : jget(*j, "references",
                               std::vector<std::string>{})) {
      if (auto a = AssetId::parse(id)) {
        req.references.push_back(*a);
      }
    }
    for (const auto& id : jget(*j, "whole", std::vector<std::string>{})) {
      if (auto a = AssetId::parse(id)) {
        req.whole.push_back(*a);
      }
    }
    if (auto c = AssetId::parse(jget<std::string>(*j, "continue", ""))) {
      req.continue_from = *c;
    }
    req.tail_seconds = jget(*j, "tail_seconds", 0.0);
    req.reference_sound = jget(*j, "reference_sound", false);
    req.inline_media = inline_pictures(*j);
    req.row = row_of(*j);
    req.prompt_asset = prompt_asset_of(*j);
    req.prompt_name = jget<std::string>(*j, "prompt_name", "");
    auto r = _impl->ctl->generate_video(std::move(req));
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::run_asset_operation(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    if (!aid.ok()) {
      return err(aid.error());
    }
    Controller::AssetOpRequest req;
    req.project = *pid;
    req.asset = *aid;
    req.operation = jget<std::string>(*j, "operation", "");
    req.layer = jget<std::string>(*j, "layer", "");
    req.model = jget<std::string>(*j, "model", "auto");
    req.options = jget(*j, "options", Json::object());
    auto r = _impl->ctl->run_asset_operation(std::move(req));
    if (!r.ok()) {
      return err(r.error());
    }
    Json made = Json::array();
    for (const auto& id : r->made) {
      made.push_back(id.str());
    }
    return ok({{"assets", made},
               {"asset", made.empty() ? std::string()
                                      : made.front().get<std::string>()},
               {"job", r->job.str()}});
  });
}

std::string
Core::asset_operations()
{
  return guarded([&] {
    return ok({{"operations", _impl->ctl->asset_operations_json()}});
  });
}

std::string
Core::generate_audio(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    GenerateAudioRequest req;
    req.project = *pid;
    req.prompt = jget<std::string>(*j, "prompt", "");
    req.lyrics = jget<std::string>(*j, "lyrics", "");
    req.model = jget<std::string>(*j, "model", "");
    req.name = jget<std::string>(*j, "name", "");
    req.plan = jget<std::string>(*j, "plan", "full");
    req.score = jget<std::string>(*j, "score", "");
    req.max_seconds = jget(*j, "max_seconds", 0.0);
    req.steps = jget(*j, "steps", 0);
    req.preference = jget<std::string>(*j, "preference", "balanced");
    req.tuning = jget(*j, "tuning", Json::object());
    req.seed = jget<std::int64_t>(*j, "seed", -1);
    req.row = row_of(*j);
    req.prompt_asset = prompt_asset_of(*j);
    req.prompt_name = jget<std::string>(*j, "prompt_name", "");
    // Speech (a model that speaks): a voice to clone, about how long.
    if (const auto v = jget<std::string>(*j, "voice", ""); !v.empty()) {
      auto vid = parse_id<AssetId>(v);
      if (!vid.ok()) {
        return err(vid.error());
      }
      req.voice = *vid;
    }
    // Drama 3's voices, speaker 0 first, and the words each says.
    for (const auto& v : jget(*j, "voices", Json::array())) {
      auto vid = parse_id<AssetId>(v.is_string() ? v.get<std::string>()
                                                 : std::string());
      if (!vid.ok()) {
        return err(vid.error());
      }
      req.voices.push_back(*vid);
    }
    for (const auto& t : jget(*j, "voice_texts", Json::array())) {
      req.voice_texts.push_back(t.is_string() ? t.get<std::string>()
                                              : std::string());
    }
    req.seconds = jget(*j, "seconds", 0.0);
    auto r = _impl->ctl->generate_audio(std::move(req));
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::capture_prompt(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    auto a = _impl->ctl->capture_prompt(
        *pid, jget<std::string>(*j, "prompt", ""), inline_pictures(*j),
        row_of(*j), prompt_asset_of(*j),
        jget<std::string>(*j, "prompt_name", ""));
    return a.ok() ? ok({{"asset", a->str()}}) : err(a.error());
  });
}

std::string
Core::set_prompt_text(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto st = _impl->ctl->set_prompt_text(*pid, *aid,
                                          jget<std::string>(*j, "text", ""));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::video_reference_tags(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    std::vector<AssetId> refs;
    for (const auto& id : jget(*j, "references",
                               std::vector<std::string>{})) {
      if (auto a = AssetId::parse(id)) {
        refs.push_back(*a);
      }
    }
    std::optional<AssetId> cont;
    if (auto c = AssetId::parse(jget<std::string>(*j, "continue", ""))) {
      cont = *c;
    }
    auto t = _impl->ctl->video_reference_tags(
        *pid, jget<std::string>(*j, "model", ""), refs, cont);
    return t.ok() ? ok({{"tags", *t}}) : err(t.error());
  });
}

std::string
Core::prompt_outline(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    std::vector<assist::RefKind> row;
    for (const auto& k : jget(*j, "row", std::vector<std::string>{})) {
      row.push_back(k == "image"   ? assist::RefKind::Image
                    : k == "video" ? assist::RefKind::Video
                    : k == "audio" ? assist::RefKind::Audio
                                   : assist::RefKind::Other);
    }
    assist::OutlineFrames frames;
    frames.first = jget(*j, "first", -1);
    frames.last = jget(*j, "last", -1);
    frames.seconds = jget(*j, "seconds", 0.0);
    auto t = _impl->ctl->prompt_outline(jget<std::string>(*j, "model", ""),
                                        row, {}, frames);
    return t.ok() ? ok({{"text", *t}}) : err(t.error());
  });
}

std::string
Core::song_text(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    const auto words = assist::tag_pictures(
        jget<std::string>(*j, "text", ""), {}, "");
    const auto s = assist::split_song(words);
    return ok({{"style", s.style}, {"lyrics", s.lyrics},
               {"sections", s.sections}, {"lines", s.lines}});
  });
}

std::string
Core::set_adjustments(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto st = _impl->ctl->set_adjustments(
        *pid, *aid,
        media::adjustments_from_json(jget(*j, "adjust", Json::object())),
        jget<std::string>(*j, "layer", ""));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::set_crop(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto st = _impl->ctl->set_crop(
        *pid, *aid, media::crop_from_json(jget(*j, "crop", Json::object())),
        jget<std::string>(*j, "layer", ""));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::set_layer_time(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    // {"in", "out", "rate_num", "rate_den", "offset", "duration"}
    const Json t = jget(*j, "time", Json::object());
    project::LayerTime lt;
    lt.in = jget<std::int64_t>(t, "in", -1);
    lt.out = jget<std::int64_t>(t, "out", -1);
    const auto num = jget<std::int64_t>(t, "rate_num", 0);
    const auto den = jget<std::int64_t>(t, "rate_den", 1);
    lt.rate = num > 0 && den > 0 ? Rational(num, den) : Rational(0, 1);
    lt.offset = jget<std::int64_t>(t, "offset", 0);
    lt.duration = jget<std::int64_t>(t, "duration", 0);
    auto st = _impl->ctl->set_layer_time(
        *pid, *aid, jget<std::string>(*j, "layer", ""), lt);
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::rendered_path(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto path = _impl->ctl->rendered(*pid, *aid);
    return path.ok() ? ok({{"path", path->string()}}) : err(path.error());
  });
}

std::string
Core::set_canvas(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto st = jget(*j, "reset", false)
        ? _impl->ctl->reset_canvas(*pid, *aid)
        : _impl->ctl->set_canvas(
              *pid, *aid, {jget(*j, "width", 0), jget(*j, "height", 0)},
              jget(*j, "anchor_x", 0.5), jget(*j, "anchor_y", 0.5));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::set_timeline(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    auto st = _impl->ctl->set_timeline(*pid, *aid,
                                       jget<std::int64_t>(*j, "frames", 0));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::stack_plan(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    // The layer being edited, as the panels hold its tracks now.
    std::optional<Controller::LiveTracks> live;
    if (const Json l = jget(*j, "live", Json()); l.is_object()) {
      live.emplace();
      live->layer = jget<std::string>(l, "layer", "");
      live->adjust = media::keyed_adjustments_from_json(
          jget(l, "adjust", Json::object()));
      live->crop = media::keyed_crop_from_json(
          jget(l, "crop", Json::object()));
      // Its look given, or its sound alone (a composition of sound).
      live->look = l.contains("adjust") || l.contains("crop");
      if (const Json snd = jget(l, "sound", Json()); snd.is_object()) {
        live->sound = media::keyed_sound_from_json(snd);
      }
    }
    // The markup objects being edited: drawn by the app, over it.
    std::set<std::string> hidden;
    for (const auto& h : jget(*j, "hidden", Json::array())) {
      if (h.is_string()) {
        hidden.insert(h.get<std::string>());
      }
    }
    auto stack = _impl->ctl->movie_stack(*pid, *aid, false, live, hidden);
    if (!stack.ok()) {
      return err(stack.error());
    }
    const Rational rate = stack->rate.num > 0 ? stack->rate
                                              : Rational{24, 1};
    // Where each clip plays on the timeline (its segments), and what the
    // player sounds: the layers' spans with their volume ramps, a layer
    // its pitch or equalizer shapes as its shaped span (media/sound.h) --
    // never the whole mix, which a gain or a band would make again.
    auto sound = _impl->ctl->playable_sound_plan(*pid, *aid, live);
    if (!sound.ok()) {
      return err(sound.error());
    }
    const double fps = rate.to_double();
    const double end = sound->seconds;
    // Each layer where it lies in time (the timeline draws them): its
    // span in timeline seconds, its source's there, what it is, its file
    // (a sound's waveform). Forever is -1.
    Json spans = Json::array();
    for (const auto& l : stack->layers) {
      const auto& t = l.timing;
      const auto fin = [](double v) { return std::isfinite(v) ? v : -1.0; };
      spans.push_back({{"id", l.id},
                       {"start", t.start},
                       {"length", fin(t.length)},
                       {"in", t.in},
                       {"out", fin(t.timed && std::isfinite(t.length)
                                       ? t.source_at(t.end()) : t.out)},
                       {"timed", t.timed},
                       {"video", l.video},
                       {"audio_only", l.audio_only},
                       {"file", l.file.string()}});
    }
    Json segs = Json::array();
    for (const auto& l : stack->layers) {
      if (!l.video) {
        continue;
      }
      Json ss = Json::array();
      for (const auto& g : media::time_segments(l.timing, end, 1.0 / fps)) {
        ss.push_back({g.at, g.length, g.source, g.source_length});
      }
      segs.push_back(std::move(ss));
    }
    Json audio = Json::array();
    std::string mix;
    {
      for (std::size_t i = 0; i < sound->layers.size(); ++i) {
        const auto& sl = sound->layers[i];
        if (sl.file.empty()) {
          continue;
        }
        Json ss = Json::array();
        for (const auto& g : media::time_segments(sl.timing, end,
                                                  1.0 / fps)) {
          ss.push_back({g.at, g.length, g.source, g.source_length});
        }
        Json vol = Json::array();
        for (const auto& [t, v] : media::volume_points(*sound, i)) {
          vol.push_back({t, v});
        }
        audio.push_back({{"file", sl.file.string()},
                         {"segments", std::move(ss)},
                         {"volume", std::move(vol)}});
      }
    }
    // Sound alone (a composition with no frame): nothing to draw -- the
    // player plays its sound, the plan has no renderer.
    if (!stack->canvas.framed() && !stack->canvas.set() &&
        std::ranges::none_of(stack->layers, [](const auto& l) {
          return l.video;
        }) &&
        (stack->layers.empty() || !stack->layers.front().video)) {
      auto n = media::timeline_frames(*stack, rate);
      return ok({{"plan", 0}, {"width", 0}, {"height", 0},
                 {"frames", n.ok() ? *n : 0}, {"rate_num", rate.num},
                 {"rate_den", rate.den}, {"clips", Json::array()},
                 {"segments", Json::array()}, {"audio", audio},
                 {"mix", mix}, {"seconds", end}, {"layers", spans}});
    }
    auto r = media::StackRenderer::make(std::move(*stack), rate);
    if (!r.ok()) {
      return err(r.error());
    }
    std::shared_ptr<const media::StackRenderer> plan = std::move(*r);
    Json clips = Json::array();
    for (const auto& c : plan->clips()) {
      clips.push_back(c.string());
    }
    const auto size = plan->size();
    const auto frames = plan->frames();
    std::uint64_t token = 0;
    {
      std::lock_guard lk(_impl->plans_mu);
      token = ++_impl->next_plan;
      _impl->plans[token] = std::move(plan);
    }
    return ok({{"plan", token}, {"width", size.width},
               {"height", size.height}, {"frames", frames},
               {"rate_num", rate.num}, {"rate_den", rate.den},
               {"clips", clips}, {"segments", segs}, {"audio", audio},
               {"mix", mix}, {"seconds", end}, {"layers", spans}});
  });
}

std::string
Core::release_stack_plan(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    std::lock_guard lk(_impl->plans_mu);
    _impl->plans.erase(jget<std::uint64_t>(*j, "plan", 0));
    return ok();
  });
}

std::string
Core::set_keys(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok() || !aid.ok()) {
      return err(Code::InvalidArgument, "bad id");
    }
    const std::string kind = jget<std::string>(*j, "kind", "");
    const Json keys = jget(*j, "keys", Json::object());
    // A layer's track, on a clip with a stack ("" is the clip itself).
    const std::string layer = jget<std::string>(*j, "layer", "");
    Status st = kind == "adjust"
        ? _impl->ctl->set_adjustment_keys(
              *pid, *aid, media::keyed_adjustments_from_json(keys), layer)
        : kind == "crop"
        ? _impl->ctl->set_crop_keys(*pid, *aid,
                                    media::keyed_crop_from_json(keys), layer)
        : Status(make_error(Code::InvalidArgument,
                            "kind is adjust or crop"));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::crop_placement(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    const auto p = media::crop_placement(
        media::crop_from_json(jget(*j, "crop", Json::object())),
        jget(*j, "width", 0.0), jget(*j, "height", 0.0));
    const auto& m = p.transform;
    return ok({{"canvas", {p.canvas_w, p.canvas_h}},
               {"transform", {m.a, m.b, m.c, m.d, m.tx, m.ty}}});
  });
}

std::string
Core::tuning(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto r = _impl->ctl->tuning(
        jget<std::string>(*j, "model", ""),
        jget<std::string>(*j, "preference", "balanced"),
        jget(*j, "edit", false), jget(*j, "tuning", Json::object()),
        jget(*j, "continues", false));
    return r.ok() ? ok(*r) : err(r.error());
  });
}

std::string
Core::upscale_settings() const
{
  return guarded([&] { return ok(_impl->ctl->upscale_settings()); });
}

std::string
Core::set_upscale_tuning(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->set_upscale_tuning(
        jget<std::string>(*j, "model", ""),
        jget(*j, "values", Json::object()), jget(*j, "reset", false));
    return st.ok() ? ok(_impl->ctl->upscale_settings()) : err(st.error());
  });
}

std::string
Core::use_upscaler(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->use_upscaler(jget<std::string>(*j, "kind", ""),
                                       jget<std::string>(*j, "model", ""));
    return st.ok() ? ok(_impl->ctl->upscale_settings()) : err(st.error());
  });
}

std::string
Core::checkpoint_kind(const std::string& request_json) const
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    return ok(_impl->ctl->classify_weights(
        jget<std::string>(*j, "path", "")));
  });
}

std::string
Core::layer_op(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    if (!aid.ok()) {
      return err(aid.error());
    }
    Controller& c = *_impl->ctl;
    const auto op = jget<std::string>(*j, "op", "");
    const auto layer = jget<std::string>(*j, "layer", "");
    // A still's page, where the op is about one (DESIGN §6a).
    std::optional<std::int64_t> page;
    if (j->contains("page")) {
      page = jget<std::int64_t>(*j, "page", 0);
    }
    if (op == "add") {
      auto r = c.add_layer(*pid, *aid, jget<std::string>(*j, "above", ""),
                           page);
      return r.ok() ? ok({{"layer", *r}}) : err(r.error());
    }
    if (op == "page-add") {
      std::optional<std::int64_t> after;
      if (j->contains("after")) {
        after = jget<std::int64_t>(*j, "after", 0);
      }
      auto r = c.add_page(*pid, *aid, after);
      return r.ok() ? ok({{"page", *r}}) : err(r.error());
    }
    auto strings = [](const Json& a) {
      std::vector<std::string> out;
      for (const auto& s : a.is_array() ? a : Json::array()) {
        if (s.is_string()) {
          out.push_back(s.get<std::string>());
        }
      }
      return out;
    };
    if (op == "markup-target") {
      // A still's page, or a timeline's frame.
      std::optional<std::int64_t> at = page;
      if (j->contains("frame")) {
        at = jget<std::int64_t>(*j, "frame", 0);
      }
      auto r = c.markup_layer(*pid, *aid,
                              strings(jget(*j, "selected", Json())), at);
      return r.ok() ? ok({{"layer", *r}}) : err(r.error());
    }
    // The markup's objects PATCHED: {"upsert": [objects], "remove":
    // [ids]} -> {"objects": those now as kept that changed (put in, or
    // resolved for an anchor), "removed", "stamp", "count"} -- what the
    // app's copy of them needs to follow (Controller::patch_markup_objects).
    if (op == "patch-objects") {
      auto r = c.patch_markup_objects(
          *pid, *aid, jget<std::string>(*j, "layer", ""),
          jget(*j, "upsert", Json::array()),
          strings(jget(*j, "remove", Json())));
      if (!r.ok()) {
        return err(r.error());
      }
      return ok({{"objects", std::move(r->changed)},
                 {"removed", r->removed},
                 {"stamp", r->stamp},
                 {"count", r->count}});
    }
    // Layer folders: "group" {"layers", "name"} -> {"folder"}; "ungroup",
    // "folder-rename" {"name"}, "folder-show" / "folder-hide" {"folder"};
    // "place" {"layers", "above" (none: the bottom), "folder"}.
    const auto folder = jget<std::string>(*j, "folder", "");
    if (op == "group") {
      auto r = c.group_layers(*pid, *aid, strings(jget(*j, "layers", Json())),
                              jget<std::string>(*j, "name", ""));
      return r.ok() ? ok({{"folder", *r}}) : err(r.error());
    }
    if (op == "ungroup" || op == "folder-rename" || op == "folder-show" ||
        op == "folder-hide" || op == "place") {
      Status fst = ok_status();
      if (op == "ungroup") {
        fst = c.ungroup_layers(*pid, *aid, folder);
      } else if (op == "folder-rename") {
        fst = c.rename_layer_folder(*pid, *aid, folder,
                                    jget<std::string>(*j, "name", ""));
      } else if (op == "place") {
        std::optional<std::string> above;
        if (j->contains("above")) {
          above = jget<std::string>(*j, "above", "");
        }
        fst = c.place_layers(*pid, *aid, strings(jget(*j, "layers", Json())),
                             above, folder);
      } else {
        fst = c.set_folder_visible(*pid, *aid, folder, op == "folder-show");
      }
      return fst.ok() ? ok() : err(fst.error());
    }
    if (op == "split") {
      // The timeline's scissors: at timeline frame "frame".
      auto r = c.split_layer(*pid, *aid, layer,
                             jget<std::int64_t>(*j, "frame", 0));
      return r.ok() ? ok({{"layer", r->layer},
                          {"first", r->first.str()},
                          {"second", r->second.str()}})
                    : err(r.error());
    }
    // A markup's drawing: "drawing" -> {"path"} ("" none); "duplicate"
    // {"name"?} -> {"layer"}.
    if (op == "drawing") {
      auto r = c.markup_drawing(*pid, *aid, layer);
      return r.ok() ? ok({{"path", r->string()}}) : err(r.error());
    }
    // DRAWINGS (media/markup.h): the point "x", "y" (markup pixels) and
    // "reach" (pixels) name one. "drawing-at" -> {"x", "y", "w", "h"}
    // (w 0: none there); "lift-drawing" -> {"object"}: lifted, an object
    // of kind "drawing"; "place-drawings": the lifted ones laid back
    // (fused); "copy-drawings" {"at"?: bool, "objects": the lifted ones}
    // -> {"path"} (a PNG in the working copy, the caller's to remove);
    // "drawing-file" {"hash"} -> {"path"}: a lifted drawing's picture.
    const auto point = [&]() -> std::optional<std::array<double, 2>> {
      if (!j->contains("x") || !j->contains("y")) {
        return std::nullopt;
      }
      return std::array<double, 2>{jget(*j, "x", 0.0), jget(*j, "y", 0.0)};
    };
    const int reach = std::clamp(jget(*j, "reach", 4), 0, 64);
    if (op == "drawing-at") {
      auto r = c.drawing_at(*pid, *aid, layer, jget(*j, "x", 0.0),
                            jget(*j, "y", 0.0), reach);
      return r.ok() ? ok({{"x", r->x}, {"y", r->y}, {"w", r->w},
                          {"h", r->h}})
                    : err(r.error());
    }
    if (op == "lift-drawing") {
      auto r = c.lift_drawing(*pid, *aid, layer, jget(*j, "x", 0.0),
                              jget(*j, "y", 0.0), reach);
      return r.ok() ? ok({{"object", *r}}) : err(r.error());
    }
    if (op == "copy-drawings") {
      auto r = c.copy_drawings(*pid, *aid, layer, point(), reach,
                               strings(jget(*j, "objects", Json())));
      return r.ok() ? ok({{"path", r->string()}}) : err(r.error());
    }
    if (op == "drawing-file") {
      project::BlobRef b;
      b.hash = jget(*j, "hash", ContentHash{});
      b.ext = "png";
      auto* p = c.project(*pid);
      if (!p || b.empty()) {
        return err(Code::InvalidArgument, "no such drawing");
      }
      return ok({{"path", p->blobs().path_of(b).string()}});
    }
    if (op == "paste-drawing") {
      // {"png" (a file) | "png_base64" (its bytes), "dx", "dy"}: a
      // drawing floating on the markup -> {"object"}.
      const auto b64 = jget<std::string>(*j, "png_base64", "");
      Result<std::string> made = make_error(Code::InvalidArgument,
                                            "not base64");
      if (!b64.empty()) {
        if (auto bytes = unbase64(b64)) {
          made = c.paste_drawing_data(*pid, *aid, layer, *bytes,
                                      jget(*j, "dx", 0.0),
                                      jget(*j, "dy", 0.0));
        }
      } else {
        made = c.paste_drawing(*pid, *aid, layer,
                               jget<std::string>(*j, "png", ""),
                               jget(*j, "dx", 0.0), jget(*j, "dy", 0.0));
      }
      return made.ok() ? ok({{"object", *made}}) : err(made.error());
    }
    if (op == "duplicate") {
      auto r = c.duplicate_layer(*pid, *aid, layer,
                                 jget<std::string>(*j, "name", ""));
      return r.ok() ? ok({{"layer", *r}}) : err(r.error());
    }
    if (op == "canvas") {
      auto r = c.canvas_size(*pid, *aid);
      return r.ok() ? ok({{"width", r->width}, {"height", r->height}})
                    : err(r.error());
    }
    Status st = make_error(Code::InvalidArgument, "unknown layer op");
    if (op == "move") {
      st = c.move_layer(*pid, *aid, layer, jget(*j, "by", 0));
    } else if (op == "show" || op == "hide") {
      // "layers": several at once -- a track of the track view's -- one
      // command.
      st = j->contains("layers")
               ? c.set_layers_visible(*pid, *aid,
                                      strings(jget(*j, "layers", Json())),
                                      op == "show")
               : c.set_layer_visible(*pid, *aid, layer, op == "show");
    } else if (op == "lock" || op == "unlock") {
      st = j->contains("layers")
               ? c.set_layers_locked(*pid, *aid,
                                     strings(jget(*j, "layers", Json())),
                                     op == "lock")
               : c.set_layer_locked(*pid, *aid, layer, op == "lock");
    } else if (op == "separator") {
      st = c.set_layer_separator(*pid, *aid, layer, jget(*j, "on", true));
    } else if (op == "rename") {
      st = c.rename_layer(*pid, *aid, layer,
                          jget<std::string>(*j, "name", ""));
    } else if (op == "source") {
      auto src = parse_id<AssetId>(jget<std::string>(*j, "source", ""));
      if (!src.ok()) {
        return err(src.error());
      }
      // "join": the generation it is the result of (its undo command).
      std::optional<JobId> job;
      if (const auto s = jget<std::string>(*j, "join", ""); !s.empty()) {
        job = JobId::parse(s);
      }
      st = c.set_layer_source(*pid, *aid, layer, *src, job);
    } else if (op == "remove") {
      st = c.remove_layer(*pid, *aid, layer);
    } else if (op == "page-remove") {
      st = c.remove_page(*pid, *aid, page.value_or(-1));
    } else if (op == "pages") {
      // The layer on pages [first, first + count); count 0: to the last.
      st = c.set_layer_pages(*pid, *aid, layer, jget<std::int64_t>(
                                 *j, "first", 0),
                             jget<std::int64_t>(*j, "count", 0));
    } else if (op == "stroke") {
      st = c.paint_stroke(*pid, *aid, layer,
                          media::stroke_from_json(jget(*j, "stroke", Json())));
    } else if (op == "objects") {
      st = c.set_markup_objects(*pid, *aid, layer,
                                jget(*j, "objects", Json::array()));
    } else if (op == "arrange") {
      st = c.arrange_markup(*pid, *aid, layer,
                            strings(jget(*j, "objects", Json())),
                            jget<std::string>(*j, "how", ""));
    } else if (op == "connectors") {
      // {"objects", "add"}: their default connectors added, or all
      // taken off.
      st = c.markup_connectors(*pid, *aid, layer,
                               strings(jget(*j, "objects", Json())),
                               jget(*j, "add", true));
    } else if (op == "materialize") {
      st = c.materialize_markup(*pid, *aid, layer,
                                strings(jget(*j, "objects", Json())));
    } else if (op == "clear-drawing") {
      // {"x", "y"}?: that drawing alone.
      st = c.clear_drawing(*pid, *aid, layer, point(), reach);
    } else if (op == "place-drawings") {
      st = c.place_drawings(*pid, *aid, layer);
    } else if (op == "merge") {
      st = c.merge_layers(*pid, *aid, layer,
                          jget<std::string>(*j, "with", ""));
    } else if (op == "mask" || op == "unmask") {
      st = c.set_layer_mask(*pid, *aid, layer, op == "mask");
    } else if (op == "decompose") {
      st = c.decompose(*pid, *aid, layer);
    } else if (op == "slide") {
      st = c.slide_layer(*pid, *aid, layer,
                         jget<std::int64_t>(*j, "offset", 0));
    } else if (op == "stretch") {
      st = c.stretch_layer(*pid, *aid, layer,
                           jget<std::int64_t>(*j, "length", 1));
    } else if (op == "move-key") {
      st = c.move_layer_key(*pid, *aid, layer,
                            jget<std::int64_t>(*j, "from", 0),
                            jget<std::int64_t>(*j, "to", 0));
    } else if (op == "flatten") {
      // The layer's composition made flat, in its place (Flatten First).
      auto made = c.flatten_layer(*pid, *aid, layer);
      return made.ok() ? ok({{"asset", made->str()}}) : err(made.error());
    } else if (op == "speed") {
      st = c.set_speed_keys(*pid, *aid,
                            media::keyed_speed_from_json(
                                jget(*j, "keys", Json::object())),
                            layer);
    } else if (op == "sound") {
      // "follow_speed": the pitch follows the speed; left out, kept.
      std::optional<bool> follow;
      if (const auto f = j->find("follow_speed"); f != j->end()) {
        follow = f->is_boolean() ? f->get<bool>()
                 : f->is_number() && f->get<double>() != 0;
      }
      st = c.set_sound_keys(*pid, *aid,
                            media::keyed_sound_from_json(
                                jget(*j, "keys", Json::object())),
                            layer, follow);
    } else if (op == "transition") {
      st = c.set_transition(*pid, *aid, layer,
                            jget<std::string>(*j, "to", ""),
                            jget<std::string>(*j, "kind", ""));
    }
    return st.ok() ? ok() : err(st.error());
  });
}

namespace {

// A flatten request's parts (Core::flatten, flatten_surface).
struct FlattenRequest {
  ProjectId                            project;
  AssetId                              asset;
  std::optional<Controller::LayerLook> look;
  std::optional<std::string>           only;
  std::set<std::string>                hidden;
  std::int64_t                         page = 0;
  double                               scale = 1;
};

Result<FlattenRequest>
flatten_request(const Json& j)
{
  auto pid = parse_id<ProjectId>(jget<std::string>(j, "project", ""));
  auto aid = parse_id<AssetId>(jget<std::string>(j, "asset", ""));
  if (!pid.ok() || !aid.ok()) {
    return make_error(Code::InvalidArgument, "project and asset ids");
  }
  FlattenRequest r{*pid, *aid, std::nullopt, std::nullopt, {},
                   jget<std::int64_t>(j, "page", 0),
                   jget<double>(j, "scale", 1.0)};
  if (const Json l = jget(j, "look", Json()); l.is_object()) {
    r.look = Controller::LayerLook{
        jget<std::string>(l, "layer", ""),
        media::adjustments_from_json(jget(l, "adjust", Json::object())),
        media::crop_from_json(jget(l, "crop", Json::object()))};
  }
  if (j.contains("only")) {
    r.only = jget<std::string>(j, "only", "");
  }
  // Markup objects the app is drawing itself, as it edits them.
  for (const auto& h : jget(j, "hidden", Json::array())) {
    if (h.is_string()) {
      r.hidden.insert(h.get<std::string>());
    }
  }
  return r;
}

}

std::string
Core::flatten(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto r = flatten_request(*j);
    if (!r.ok()) {
      return err(r.error());
    }
    auto st = _impl->ctl->flatten(r->project, r->asset,
                                  jget<std::string>(*j, "path", ""), r->look,
                                  r->only, r->hidden, r->page);
    return st.ok() ? ok() : err(st.error());
  });
}

bool
stack_render(Core* core, std::uint64_t plan, std::int64_t frame,
             CFArrayRef clips, CVPixelBufferRef out)
{
  if (!core || !core->_impl || !out) {
    return false;
  }
  auto r = core->_impl->plan(plan);
  if (!r) {
    return false;
  }
  std::vector<CVPixelBufferRef> frames;
  const CFIndex n = clips ? CFArrayGetCount(clips) : 0;
  for (CFIndex i = 0; i < n; ++i) {
    const void* v = CFArrayGetValueAtIndex(clips, i);
    frames.push_back(v && CFGetTypeID(v) == CVPixelBufferGetTypeID()
                         ? static_cast<CVPixelBufferRef>(
                               const_cast<void*>(v))
                         : nullptr);
  }
  try {
    auto st = r->render(frame, frames, out);
    if (!st.ok()) {
      VALTZ_LOG_WARN("bridge", "stack_render: {}", st.error().message);
    }
    return st.ok();
  } catch (...) {
    return false;
  }
}

CGImageRef
stack_still(Core* core, std::uint64_t plan, std::int64_t frame)
{
  if (!core || !core->_impl) {
    return nullptr;
  }
  auto r = core->_impl->plan(plan);
  if (!r) {
    return nullptr;
  }
  try {
    auto img = r->still(frame);
    if (!img.ok()) {
      VALTZ_LOG_WARN("bridge", "stack_still: {}", img.error().message);
      return nullptr;
    }
    return *img;
  } catch (...) {
    return nullptr;
  }
}

IOSurfaceRef
flatten_surface(Core* core, const std::string& request_json)
{
  if (!core || !core->_impl) {
    return nullptr;
  }
  try {
    auto j = parse(request_json);
    auto r = j.ok() ? flatten_request(*j)
                    : Result<FlattenRequest>(j.error());
    if (!r.ok()) {
      VALTZ_LOG_WARN("bridge", "flatten_surface: {}", r.error().message);
      return nullptr;
    }
    auto s = core->_impl->ctl->flatten_surface(r->project, r->asset, r->look,
                                               r->only, r->hidden, r->page,
                                               r->scale);
    if (!s.ok()) {
      VALTZ_LOG_WARN("bridge", "flatten_surface: {}", s.error().message);
      return nullptr;
    }
    return *s;
  } catch (...) {
    return nullptr;
  }
}

std::string
Core::adjustment_chain(const std::string& adjustments_json) const
{
  return guarded([&] {
    auto j = parse(adjustments_json);
    if (!j.ok()) {
      return err(j.error());
    }
    return ok({{"chain", media::to_json(media::filter_chain(
                             media::adjustments_from_json(*j)))}});
  });
}

std::string
Core::color_mix() const
{
  return guarded([&] {
    Json bands = Json::array();
    for (const auto& b : media::kMixHueBands) {
      bands.push_back({{"key", b.key}, {"degrees", b.degrees}});
    }
    return ok({{"kernel", media::color_mix_kernel_source()},
               {"kernel_name", media::kColorMixKernel},
               {"filter", media::kColorMixFilter},
               {"samples", media::kMixSamples},
               {"bands", std::move(bands)},
               {"hue_degrees", media::kMixHueDegrees},
               {"luminance_stops", media::kMixLuminanceStops}});
  });
}

std::string
Core::color_grade() const
{
  return guarded([&] {
    return ok({{"kernel", media::color_grade_kernel_source()},
               {"kernel_name", media::kColorGradeKernel},
               {"filter", media::kColorGradeFilter},
               {"balance_reach", media::kGradeBalanceReach},
               {"strength", media::kGradeStrength}});
  });
}

std::string
Core::color_mix_hue(double r, double g, double b) const
{
  return guarded([&] {
    const auto h = media::mix_hue_of(r, g, b);
    if (!h) { return ok({{"grey", true}}); }
    return ok({{"grey", false}, {"hue", h->hue},
               {"saturation", h->saturation}});
  });
}

std::string
Core::enhance_prompt(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    EnhancePromptRequest req;
    req.prompt = jget<std::string>(*j, "prompt", "");
    req.target = jget<std::string>(*j, "target", "image");
    req.style = jget<std::string>(*j, "style", "");
    req.model = jget<std::string>(*j, "model", "");
    req.mode = jget<std::string>(*j, "mode", "auto");
    req.size = jget<std::string>(*j, "size", "");
    req.seconds = jget(*j, "seconds", 0.0);
    if (auto pid = ProjectId::parse(jget<std::string>(*j, "project", ""))) {
      req.project = *pid;
    }
    if (auto b = AssetId::parse(jget<std::string>(*j, "base", ""))) {
      req.base = *b;
    }
    if (auto l = AssetId::parse(jget<std::string>(*j, "last", ""))) {
      req.last = *l;
    }
    for (const auto& s : jget(*j, "references",
                              std::vector<std::string>{})) {
      if (auto id = AssetId::parse(s)) {
        req.references.push_back(*id);
      }
    }
    req.inline_pictures = inline_pictures(*j);
    req.row = row_of(*j);
    // A clip's: as generate_video reads them.
    req.frames = jget(*j, "frames", 0);
    req.fps = jget(*j, "fps", 0.0);
    if (auto c = AssetId::parse(jget<std::string>(*j, "continue", ""))) {
      req.continue_from = *c;
    }
    req.reference_sound = jget(*j, "reference_sound", false);
    auto r = _impl->ctl->enhance_prompt(std::move(req));
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::export_asset(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
    if (!pid.ok()) {
      return err(pid.error());
    }
    auto aid = parse_id<AssetId>(jget<std::string>(*j, "asset", ""));
    if (!aid.ok()) {
      return err(aid.error());
    }
    ExportRequest req;
    req.project = *pid;
    req.asset = *aid;
    req.format = jget<std::string>(*j, "format", "");
    req.destination = jget<std::string>(*j, "path", "");
    req.quality = jget(*j, "quality", 0);
    req.video = engine::video_encoding_from_json(
        jget(*j, "video", Json::object()));
    auto r = _impl->ctl->export_asset(std::move(req));
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::detect_intent(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    std::vector<assist::Attachment> atts;
    for (const auto& a : jget(*j, "attachments", Json::array())) {
      atts.push_back({jget<std::string>(a, "kind", ""),
                      jget<std::string>(a, "name", ""),
                      jget<std::string>(a, "caption", "")});
    }
    auto r = _impl->ctl->detect_intent(jget<std::string>(*j, "text", ""),
                                       std::move(atts),
                                       jget(*j, "use_model", false));
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::download_model(const std::string& model_id,
                     const std::string& hf_token, const std::string& source)
{
  return guarded([&] {
    auto r = _impl->ctl->download_model(model_id, hf_token, source);
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::download_sources_json() const
{
  return guarded([&] { return ok(_impl->ctl->download_sources()); });
}

std::string
Core::set_download_source(const std::string& name)
{
  return guarded([&] {
    Status st = _impl->ctl->set_download_source(name);
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::stop_download(const std::string& model_id, bool discard)
{
  return guarded([&] {
    Status st = _impl->ctl->stop_download(model_id, discard);
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::capture_sources_json() const
{
  return guarded([&] { return ok(_impl->ctl->capture_sources()); });
}

std::string
Core::capture_op(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    Controller& c = *_impl->ctl;
    const auto op = jget<std::string>(*j, "op", "");
    if (op == "state") {
      return ok(c.capture_state());
    }
    if (op == "start") {
      auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
      if (!pid.ok()) {
        return err(pid.error());
      }
      // Into a composition of sound alone, or ("" / absent) the asset
      // list alone.
      std::optional<AssetId> into;
      if (const auto a = jget<std::string>(*j, "asset", ""); !a.empty()) {
        auto aid = parse_id<AssetId>(a);
        if (!aid.ok()) {
          return err(aid.error());
        }
        into = *aid;
      }
      // A voice-over: onto `asset`, from its frame "voice_over_at".
      std::optional<std::int64_t> at;
      if (j->contains("voice_over_at")) {
        at = jget<std::int64_t>(*j, "voice_over_at", 0);
      }
      auto st = c.start_capture(*pid, into,
                                jget<std::string>(*j, "source", ""), at);
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "cue") {
      auto st = c.cue_capture(jget(*j, "host", 0.0));
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "stop") {
      auto made = c.stop_capture();
      return made.ok() ? ok({{"asset", made->str()}}) : err(made.error());
    }
    if (op == "cancel") {
      auto st = c.cancel_capture();
      return st.ok() ? ok() : err(st.error());
    }
    return err(Code::InvalidArgument, "unknown capture op");
  });
}

std::string
Core::camera_op(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    Controller& c = *_impl->ctl;
    const auto op = jget<std::string>(*j, "op", "");
    if (op == "sources") {
      return ok(c.camera_sources());
    }
    if (op == "state") {
      return ok(c.camera_state());
    }
    if (op == "start") {
      auto pid = parse_id<ProjectId>(jget<std::string>(*j, "project", ""));
      if (!pid.ok()) {
        return err(pid.error());
      }
      auto st = c.start_camera(*pid, jget<std::string>(*j, "camera", ""),
                               jget(*j, "sound", true));
      return st.ok() ? ok() : err(st.error());
    }
    if (op == "snap" || op == "stop-recording") {
      auto made = op == "snap" ? c.camera_snap() : c.camera_stop_recording();
      return made.ok() ? ok({{"asset", made->str()}}) : err(made.error());
    }
    if (op == "record" || op == "stop") {
      auto st = op == "record" ? c.camera_record() : c.stop_camera();
      return st.ok() ? ok() : err(st.error());
    }
    return err(Code::InvalidArgument, "unknown camera op");
  });
}

IOSurfaceRef
camera_frame(Core* core)
{
  if (!core || !core->_impl) {
    return nullptr;
  }
  return core->_impl->ctl->camera_frame().surface;
}

std::string
Core::quantize_model(const std::string& model_id)
{
  return guarded([&] {
    auto r = _impl->ctl->quantize_model(model_id);
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::compress_model(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto r = _impl->ctl->compress_model(jget<std::string>(*j, "model", ""),
                                        jget(*j, "bits", 8),
                                        jget(*j, "keep_original", false));
    return r.ok() ? ok({{"job", r->str()}}) : err(r.error());
  });
}

std::string
Core::use_compressed(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto st = _impl->ctl->use_compressed(
        jget<std::string>(*j, "model", ""), jget(*j, "compressed", true));
    return st.ok() ? ok() : err(st.error());
  });
}

std::string
Core::keep_compressed(const std::string& request_json)
{
  return guarded([&] {
    auto j = parse(request_json);
    if (!j.ok()) {
      return err(j.error());
    }
    auto r = _impl->ctl->keep_compressed(
        jget<std::string>(*j, "model", ""), jget(*j, "compressed", true));
    return r.ok() ? ok({{"freed", *r}}) : err(r.error());
  });
}

std::string
Core::cancel(const std::string& job_id)
{
  return guarded([&] {
    auto id = parse_id<JobId>(job_id);
    if (!id.ok()) {
      return err(id.error());
    }
    auto st = _impl->ctl->cancel(*id);
    return st.ok() ? ok() : err(st.error());
  });
}

Event
Core::next_event(std::int32_t timeout_ms)
{
  Event out;
  try {
    valtz::Event ev;
    if (!_impl->ctl->events().wait(ev, timeout_ms)) {
      return out;
    }
    out.valid = true;
    out.json = ev.to_json();
    if (ev.tensor) {
      std::lock_guard lk(_impl->mu);
      out.image_token = ++_impl->next_token;
      _impl->previews.emplace_back(out.image_token, std::move(ev.tensor));
      while (_impl->previews.size() > 4) {
        _impl->previews.pop_front();  // releases the oldest buffer
      }
    }
  } catch (...) {
    out.valid = false;
  }
  return out;
}

void
Core::shutdown()
{
  _impl->ctl->shutdown();
}

namespace {

// The frames of a planar [C,H,W] picture (one) or [F,C,H,W] clip (F),
// as the bridge can show them: u8 or f16, contiguous, 1-4 channels. 0
// when it cannot.
std::int64_t
frame_count(const engine::Tensor& t)
{
  using DT = engine::Tensor::DType;
  if (t.shape.size() < 3 || !t.data || !t.contiguous() ||
      (t.dtype != DT::U8 && t.dtype != DT::F16)) {
    return 0;
  }
  const auto n = t.shape.size();
  const auto c = t.shape[n - 3];
  const auto h = t.shape[n - 2];
  const auto w = t.shape[n - 1];
  if (c < 1 || c > 4 || h <= 0 || w <= 0) {
    return 0;
  }
  const std::size_t frame = static_cast<std::size_t>(c * h * w) *
                            t.element_size();
  const std::int64_t f = n > 3 ? t.shape[n - 4] : 1;
  return f > 0 && t.byte_size >= frame * static_cast<std::size_t>(f) ? f
                                                                     : 0;
}

// Planes -> interleaved RGBA, one pass: typed loads and stores the
// compiler vectorizes (a memcpy a sample was 4.5x slower: 10.8 ms
// against 2.4 ms for a 124-frame 416x240 clip). Grey goes to all three;
// with no alpha plane the picture is opaque.
template <class T>
void
interleave(const T* src, std::size_t plane, std::int64_t c, T opaque,
           T* dst)
{
  const T* r = src;
  const T* g = c >= 3 ? src + plane : src;
  const T* b = c >= 3 ? src + 2 * plane : src;
  if (c == 4) {
    const T* a = src + 3 * plane;
    for (std::size_t i = 0; i < plane; ++i) {
      dst[4 * i] = r[i];
      dst[4 * i + 1] = g[i];
      dst[4 * i + 2] = b[i];
      dst[4 * i + 3] = a[i];
    }
    return;
  }
  for (std::size_t i = 0; i < plane; ++i) {
    dst[4 * i] = r[i];
    dst[4 * i + 1] = g[i];
    dst[4 * i + 2] = b[i];
    dst[4 * i + 3] = opaque;
  }
}

// Frame `index` of a planar [C,H,W] / [F,C,H,W] tensor (frame_count()
// says it has one), u8 or f16, straight alpha, sRGB-encoded -> an
// interleaved CGImage. One CPU pass by necessity: the preview arrives in
// CPU memory (vpipe's TAE writes no Metal buffer yet, DESIGN §15 item 0)
// and Core Graphics wants interleaved pixels.
CGImageRef
frame_to_image(const engine::Tensor& t, std::int64_t index)
{
  const auto n = t.shape.size();
  const auto c = t.shape[n - 3];
  const auto h = t.shape[n - 2];
  const auto w = t.shape[n - 1];
  const std::size_t plane = static_cast<std::size_t>(h * w);
  const std::size_t esz = t.element_size();
  const std::size_t frame = plane * static_cast<std::size_t>(c) * esz;
  const std::uint8_t* src = t.data + frame * static_cast<std::size_t>(index);
  auto* px = new std::vector<std::uint8_t>(plane * 4 * esz);
  if (esz == 1) {
    interleave<std::uint8_t>(src, plane, c, 255, px->data());
  } else {
    // Half floats moved as their bits; opaque is 1.0, 0x3c00.
    interleave<std::uint16_t>(
        reinterpret_cast<const std::uint16_t*>(src), plane, c, 0x3c00,
        reinterpret_cast<std::uint16_t*>(px->data()));
  }
  CGDataProviderRef dp = CGDataProviderCreateWithData(
      px, px->data(), px->size(), [](void* info, const void*, size_t) {
        delete static_cast<std::vector<std::uint8_t>*>(info);
      });
  const bool half = esz == 2;
  CGColorSpaceRef cs = CGColorSpaceCreateWithName(
      half ? kCGColorSpaceExtendedSRGB : kCGColorSpaceSRGB);
  CGBitmapInfo info =
      static_cast<CGBitmapInfo>(kCGImageAlphaLast) |
      (half ? (static_cast<CGBitmapInfo>(kCGBitmapFloatComponents) |
               static_cast<CGBitmapInfo>(kCGBitmapByteOrder16Little))
            : static_cast<CGBitmapInfo>(kCGBitmapByteOrderDefault));
  CGImageRef out = CGImageCreate(
      static_cast<size_t>(w), static_cast<size_t>(h), 8 * esz, 32 * esz,
      static_cast<size_t>(w) * 4 * esz, cs, info, dp, nullptr, false,
      kCGRenderingIntentDefault);
  CGColorSpaceRelease(cs);
  CGDataProviderRelease(dp);
  return out;
}

}

CGImageRef
take_preview(Core* core, std::uint64_t token)
{
  if (!core || !token) {
    return nullptr;
  }
  engine::TensorPtr t = core->_impl->take(token);
  const std::int64_t frames = t ? frame_count(*t) : 0;
  // A clip shows its last frame: where the motion ends up.
  return frames > 0 ? frame_to_image(*t, frames - 1) : nullptr;
}

CFArrayRef
take_preview_clip(Core* core, std::uint64_t token)
{
  if (!core || !token) {
    return nullptr;
  }
  engine::TensorPtr t = core->_impl->take(token);
  const std::int64_t frames = t ? frame_count(*t) : 0;
  if (frames <= 0) {
    return nullptr;
  }
  CFMutableArrayRef out = CFArrayCreateMutable(
      kCFAllocatorDefault, static_cast<CFIndex>(frames),
      &kCFTypeArrayCallBacks);
  for (std::int64_t i = 0; i < frames; ++i) {
    if (CGImageRef img = frame_to_image(*t, i)) {
      CFArrayAppendValue(out, img);
      CGImageRelease(img);
    }
  }
  return out;  // t, the engine's buffer, released on return
}

}
