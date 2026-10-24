// Where Valtz keeps its projects, models and cache, and those folders
// moved (Controller::set_location; base/paths.h, base/tree-copy.h).
//
// A move is careful of the work it could break: it starts only while
// nothing runs; a copy across drives reads the old files while jobs
// started meanwhile read them too -- they are still whole -- and the
// folders switch, and the old files go, only once no job is left that
// could hold a path into them.

#include "valtz/controller/controller.h"

#include "valtz/base/log.h"
#include "valtz/base/tree-copy.h"
#include "valtz/models/model-check.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <set>
#include <thread>

namespace valtz {

namespace fs = std::filesystem;

namespace {

bool
same_place(const fs::path& a, const fs::path& b)
{
  return a.lexically_normal() == b.lexically_normal();
}

// One holds the other: moving there would copy a folder into itself.
bool
nested(const fs::path& a, const fs::path& b)
{
  return !same_place(a, b) && (path_within(a, b) || path_within(b, a));
}

bool
writable(const fs::path& dir)
{
  const fs::path probe = dir / ".valtz-write-test";
  {
    std::ofstream out(probe, std::ios::trunc);
    out << "valtz";
    if (!out) {
      return false;
    }
  }
  std::error_code ec;
  fs::remove(probe, ec);
  return true;
}

// The compressed copies' marks moved with them name where their
// originals are now (a mark's "from": the original kept beside it).
void
rebase_marks(const fs::path& root, const fs::path& from, const fs::path& to)
{
  std::error_code ec;
  for (const auto& d : fs::directory_iterator(root / "compressed", ec)) {
    const fs::path mark = d.path() / models::ModelStore::kCompressedMarker;
    Json m;
    {
      std::ifstream in(mark);
      if (!in) {
        continue;
      }
      m = Json::parse(in, nullptr, /*allow_exceptions=*/false);
    }
    const fs::path was = jget<std::string>(m, "from", "");
    if (!m.is_object() || was.empty() || !path_within(was, from)) {
      continue;
    }
    m["from"] = (to / was.lexically_normal().lexically_relative(
                          from.lexically_normal())).string();
    std::ofstream out(mark, std::ios::trunc);
    out << to_text(m, 2) << "\n";
  }
}

}

AppPaths
Controller::paths() const
{
  std::lock_guard lk(_paths_mu);
  return _paths;
}

bool
Controller::models_moving_() const
{
  std::lock_guard lk(_paths_mu);
  return !_moving.is_nil() && _moving_what == "models";
}

bool
Controller::others_live_(JobId self) const
{
  for (const auto& r : _jobs.list()) {
    if (r.id != self &&
        (r.state == JobState::Queued || r.state == JobState::Running)) {
      return true;
    }
  }
  return false;
}

fs::path
Controller::download_base_(const models::ModelEntry& e) const
{
  const auto info = _store->info(e);
  if (info.state == models::InstallState::Partial && !info.linked) {
    if (fs::path r = _store->own_root_of(info.dir); !r.empty()) {
      return r;
    }
  }
  return _store->download_root();
}

Status
Controller::save_locations_()
{
  const AppPaths d = AppPaths::defaults(_cfg.support_root);
  Locations l;
  if (!same_place(_paths.projects, d.projects)) {
    l.projects = _paths.projects;
  }
  if (!same_place(_paths.models, d.models)) {
    l.models = _paths.models;
  }
  if (!same_place(_paths.cache, d.cache)) {
    l.cache = _paths.cache;
  }
  l.models_kept = _paths.models_kept;
  return write_locations(_paths.support, l);
}

Json
Controller::locations() const
{
  const AppPaths p = paths();
  const AppPaths d = AppPaths::defaults(_cfg.support_root);
  auto item = [&](const char* what, const fs::path& at,
                  const fs::path& def) {
    std::error_code ec;
    const bool there = fs::is_directory(at, ec);
    return Json{{"path", at.string()},
                {"default", def.string()},
                {"is_default", same_place(at, def)},
                {"there", there},
                {"internal", volume_is_internal(at)},
                {"missing_at_launch", _locations_missing.contains(what)}};
  };
  Json out = {{"projects", item("projects", p.projects, d.projects)},
              {"models", item("models", p.models, d.models)},
              {"cache", item("cache", p.cache, d.cache)}};
  Json kept = Json::array();
  for (const auto& k : p.models_kept) {
    std::error_code ec;
    kept.push_back({{"path", k.string()},
                    {"there", fs::is_directory(k, ec)},
                    {"internal", volume_is_internal(k)}});
  }
  out["models"]["kept"] = std::move(kept);
  std::uint64_t held = 0;
  for (const auto& r : _store->owned_roots()) {
    held += tree_size(r).bytes;
  }
  out["models"]["bytes"] = held;
  out["cache"]["bytes"] = _cache->size_bytes();
  out["cache"]["in_use"] = _cache->root().string();
  std::lock_guard lk(_paths_mu);
  out["moving"] = _moving.is_nil()
      ? Json()
      : Json{{"job", _moving}, {"what", _moving_what},
             {"to", _moving_to.string()}};
  return out;
}

Result<JobId>
Controller::set_location(const std::string& what, const fs::path& dir_in,
                         const std::string& how)
{
  const bool models = what == "models";
  if (what != "projects" && !models && what != "cache") {
    return make_error(Code::InvalidArgument,
                      std::format("no such folder: {}", what));
  }
  if ((models && how != "move" && how != "keep") ||
      (what == "cache" && how != "move" && how != "keep" &&
       how != "drop")) {
    return make_error(Code::InvalidArgument,
                      std::format("{}: move, keep{}", what,
                                  models ? "" : " or drop"));
  }
  std::error_code ec;
  fs::path dir = fs::absolute(dir_in, ec).lexically_normal();
  if (dir.filename().empty()) {
    dir = dir.parent_path();
  }
  {
    std::lock_guard lk(_paths_mu);
    if (!_moving.is_nil()) {
      return make_error(Code::Busy, msg::kLocationMoving);
    }
  }
  fs::create_directories(dir, ec);
  if (!fs::is_directory(dir, ec) || !writable(dir)) {
    return make_error(Code::Io, msg::kLocationNotWritable,
                      {{"path", dir.string()}});
  }

  if (what == "projects") {
    std::lock_guard lk(_paths_mu);
    _paths.projects = dir;
    VALTZ_TRY(save_locations_());
    post_("locations.changed", JobId{}, {{"what", what}});
    return JobId{};
  }

  // What moves: Valtz's own models folders, or the cache in use.
  std::vector<fs::path> from;
  if (models) {
    from = _store->owned_roots();
    if (same_place(from.front(), dir)) {
      return JobId{};
    }
  } else {
    from = {_cache->root()};
    if (same_place(from.front(), dir)) {
      // Back where it is in use (it was not there at launch).
      std::lock_guard lk(_paths_mu);
      _paths.cache = dir;
      VALTZ_TRY(save_locations_());
      return JobId{};
    }
  }
  for (const auto& f : from) {
    if (nested(f, dir)) {
      return make_error(Code::InvalidArgument, msg::kLocationNested,
                        {{"path", dir.string()}});
    }
  }
  if (others_live_(JobId{}) || (!models && _cache->pinned())) {
    return make_error(Code::Busy, models ? msg::kModelsBusy
                                         : msg::kCacheBusy);
  }

  // KEPT where they are: the new folder takes the downloads, the old
  // ones are still looked in -- at once.
  if (how == "keep") {
    if (models) {
      std::vector<fs::path> kept;
      for (const auto& f : from) {
        if (!same_place(f, dir)) {
          kept.push_back(f);
        }
      }
      _store->set_roots(models::ModelStore::default_roots(dir, kept),
                        1 + kept.size());
      {
        std::lock_guard lk(_paths_mu);
        _paths.models = dir;
        _paths.models_kept = kept;
        VALTZ_TRY(save_locations_());
      }
      VALTZ_LOG_INFO("models", "the models folder is {} now, {} kept",
                     dir.string(), kept.size());
      post_("models.changed", JobId{});
    } else {
      VALTZ_TRY(_cache->set_root(dir));
      std::lock_guard lk(_paths_mu);
      _paths.cache = dir;
      _caches_left.push_back(from.front());
      VALTZ_TRY(save_locations_());
    }
    post_("locations.changed", JobId{}, {{"what", what}});
    return JobId{};
  }

  // Room for what is copied across drives (what is renamed takes none),
  // and a gigabyte to spare.
  if (how == "move") {
    std::uint64_t need = 0;
    for (const auto& f : from) {
      const fs::path src = models ? f : f / "objects";
      if (fs::is_directory(src, ec) && !same_volume(src, dir)) {
        need += tree_size(src).bytes;
      }
    }
    const auto space = fs::space(dir, ec);
    if (!ec && need > 0 &&
        need + (std::uint64_t{1} << 30) > space.available) {
      return make_error(Code::Io, msg::kLocationNoRoom,
                        {{"path", dir.string()},
                         {"free", models::size_text(space.available)},
                         {"need", models::size_text(need)}});
    }
  }

  JobRecord rec;
  rec.id = JobId::make();
  rec.op = "relocate";
  rec.purpose = "relocate";
  rec.title = models ? "Move the models"
              : how == "drop" ? "Delete the cache" : "Move the cache";
  rec.state = JobState::Running;
  rec.created_ms = project::now_ms();
  _jobs.add(rec);
  {
    std::lock_guard lk(_paths_mu);
    _moving = rec.id;
    _moving_what = what;
    _moving_to = dir;
    _moving_stop = false;
  }
  post_("job.queued", rec.id, {{"title", rec.title}, {"op", rec.op},
                               {"purpose", rec.purpose}, {"what", what},
                               {"to", dir.string()}, {"how", how}});
  post_("job.started", rec.id);
  post_("locations.changed", JobId{}, {{"what", what}});
  std::lock_guard lk(_bg_mu);
  _bg.emplace_back([this, id = rec.id, what, from, dir, how] {
    relocate_(id, what, from, dir, how);
  });
  return rec.id;
}

void
Controller::relocate_(JobId id, std::string what, std::vector<fs::path> from,
                      fs::path to, std::string how)
{
  const bool models = what == "models";
  // What moves of each: a models folder whole; a cache's entries.
  auto src_of = [models](const fs::path& r) {
    return models ? r : r / "objects";
  };
  const fs::path dst = models ? to : to / "objects";
  std::error_code ec;
  // Another drive's: copied, counted. This one's: renamed, at once. A
  // models folder not there (its drive not connected) stays one KEPT,
  // looked in when it is back.
  std::vector<fs::path> across, along, kept;
  std::vector<std::uint64_t> sizes;
  std::uint64_t total = 0;
  if (how != "drop") {
    for (const auto& r : from) {
      if (!fs::is_directory(src_of(r), ec)) {
        if (models && !fs::is_directory(r, ec)) {
          kept.push_back(r);
        }
        continue;
      }
      if (!_cfg.move_by_copy && same_volume(src_of(r), dst)) {
        along.push_back(r);
      } else {
        across.push_back(r);
        sizes.push_back(tree_size(src_of(r)).bytes);
        total += sizes.back();
      }
    }
  }
  using Clock = std::chrono::steady_clock;
  auto told = Clock::now() - std::chrono::seconds(1);
  auto tell = [&](const char* phase, std::uint64_t done, bool now) {
    if (!now && Clock::now() - told < std::chrono::milliseconds(250)) {
      return;
    }
    told = Clock::now();
    const double f = total ? static_cast<double>(done) /
                                 static_cast<double>(total)
                           : 0.0;
    _jobs.update(id, [f](JobRecord& r) {
      r.progress = static_cast<float>(f);
    });
    post_("job.progress", id,
          {{"progress", f}, {"phase", phase},
           {"done", static_cast<std::int64_t>(done)},
           {"total", static_cast<std::int64_t>(total)}, {"what", what}});
  };
  auto ended = [&](JobState st, const std::string& why) {
    {
      std::lock_guard lk(_paths_mu);
      _moving = JobId{};
      _moving_what.clear();
      _moving_to.clear();
    }
    finish_job_(id, st, st == JobState::Finished ? to.string() : why);
    if (st == JobState::Finished) {
      post_("job.finished", id, {{"what", what}, {"path", to.string()},
                                 {"how", how}});
    } else if (st == JobState::Cancelled) {
      post_("job.cancelled", id, {{"what", what}});
    } else {
      Json d = message_fields(models ? msg::kModelsMoveFailed
                                     : msg::kCacheMoveFailed,
                              {{"path", to.string()}, {"why", why}});
      d["code"] = "io";
      d["what"] = what;
      post_("job.failed", id, d);
    }
    post_("locations.changed", JobId{}, {{"what", what}});
  };

  // The copy, undone if it does not go on: what it wrote goes, and the
  // folders it made.
  std::set<fs::path> before;
  for (const auto& e : fs::directory_iterator(dst, ec)) {
    before.insert(e.path());
  }
  std::vector<fs::path> made;
  auto undo = [&] {
    std::error_code ec2;
    for (const auto& f : made) {
      fs::remove(f, ec2);
    }
    std::vector<fs::path> added;
    for (const auto& e : fs::directory_iterator(dst, ec2)) {
      if (!before.contains(e.path())) {
        added.push_back(e.path());
      }
    }
    for (const auto& a : added) {
      remove_empty_dirs(a);
    }
  };
  auto stop = [this] {
    return _moving_stop.load() || _shutting_down.load();
  };

  std::uint64_t done = 0;
  for (std::size_t i = 0; i < across.size(); ++i) {
    const std::uint64_t base = done;
    tell("copying", done, true);
    Status st = copy_tree(src_of(across[i]), dst,
                          [&](std::uint64_t n) {
                            if (stop()) {
                              return false;
                            }
                            tell("copying", base + n, false);
                            return true;
                          },
                          &made);
    if (!st.ok()) {
      undo();
      const bool stopped = st.error().code == Code::Cancelled;
      VALTZ_LOG_WARN("locations", "{} not moved to {}: {}", what,
                     to.string(), st.error().message);
      ended(stopped ? JobState::Cancelled : JobState::Failed,
            st.error().message);
      return;
    }
    done = base + sizes[i];
  }

  // Nothing left that could read the old files: the folders switch now.
  // (Stopping Valtz meanwhile: nothing runs any more.)
  tell("waiting", done, true);
  while (!_shutting_down.load() &&
         (others_live_(id) || (!models && _cache->pinned()))) {
    if (_moving_stop.load()) {
      undo();
      ended(JobState::Cancelled, "stopped");
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }
  if (_moving_stop.load() && !_shutting_down.load()) {
    undo();
    ended(JobState::Cancelled, "stopped");
    return;
  }

  // This drive's, renamed. One that fails is KEPT where it is instead:
  // every model still found.
  std::string failed;
  for (const auto& r : along) {
    if (Status st = move_tree(src_of(r), dst); !st.ok()) {
      VALTZ_LOG_WARN("locations", "{} not moved whole: {}", r.string(),
                     st.error().message);
      failed = st.error().message;
      kept.push_back(r);
    }
  }

  if (models) {
    for (const auto& r : from) {
      if (std::ranges::find(kept, r) == kept.end()) {
        _store->rebase(r, to);
        rebase_marks(to, r, to);
      }
    }
    _store->set_roots(models::ModelStore::default_roots(to, kept),
                      1 + kept.size());
    {
      std::lock_guard lk(_paths_mu);
      _paths.models = to;
      _paths.models_kept = kept;
      if (Status st = save_locations_(); !st.ok()) {
        VALTZ_LOG_ERROR("locations", "{}", st.error().message);
      }
    }
    post_("models.changed", JobId{});
  } else {
    // A pin taken since the wait (a thumbnail's): a moment later.
    Status st = _cache->set_root(to);
    for (int i = 0; !st.ok() && st.error().code == Code::Busy && i < 40;
         ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      st = _cache->set_root(to);
    }
    if (!st.ok()) {
      undo();
      ended(JobState::Failed, st.error().message);
      return;
    }
    std::lock_guard lk(_paths_mu);
    _paths.cache = to;
    _caches_left.push_back(from.front());
    if (Status s2 = save_locations_(); !s2.ok()) {
      VALTZ_LOG_ERROR("locations", "{}", s2.error().message);
    }
  }

  // The old files, now nothing reads them.
  tell("deleting", done, true);
  for (const auto& r : from) {
    if (std::ranges::find(kept, r) != kept.end()) {
      continue;
    }
    const fs::path src = src_of(r);
    if (std::ranges::find(across, r) != across.end() || how == "drop") {
      std::vector<fs::path> entries;
      for (const auto& e : fs::directory_iterator(src, ec)) {
        entries.push_back(e.path());
      }
      for (const auto& e : entries) {
        fs::remove_all(e, ec);
      }
    }
    remove_empty_dirs(models ? r : src);
  }
  VALTZ_LOG_INFO("locations", "{} {} {}", what,
                 how == "drop" ? "dropped; now in" : "moved to", to.string());
  if (!failed.empty()) {
    ended(JobState::Failed, failed);
    return;
  }
  ended(JobState::Finished, "");
}

}
