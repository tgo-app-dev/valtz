// MODELS COPIED BETWEEN THE FLEET'S MEMBERS (DESIGN §11d): this Mac's
// model share -- the file server it runs while in a fleet, what it
// serves -- and a copy's plan from a member: the Thunderbolt cables to
// it (fleet/thunderbolt.h), else its network addresses. The copy itself
// is a download job of the engine's (`fleet_source`), so Capabilities
// shows, pauses and cancels it as any download.

#include "valtz/controller/controller.h"

#include "valtz-vpipe/far-wire.h"

#include "valtz/base/hash.h"
#include "valtz/base/log.h"
#include "valtz/models/model-check.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <set>
#include <fstream>
#include <thread>

namespace valtz {

namespace fs = std::filesystem;

namespace {

bool
in_list(const Json& list, std::string_view what)
{
  return std::ranges::any_of(
      list.is_array() ? list : Json::array(), [&](const Json& x) {
        return x.is_string() && x.get_ref<const std::string&>() == what;
      });
}

}

void
Controller::share_follow_()
{
  const bool joined = _fleet && _fleet->config().joined();
  // MEMORY LENT (DESIGN §11e): a member's expert cache's far tier, sized
  // as each session starts -- nothing taken until then.
  const bool lends = joined && _fleet->config().lend != "off";
  if (!lends && _lender) {
    _lender.reset();
    VALTZ_LOG_INFO("fleet", "memory no longer lent");
  } else if (lends && !_lender) {
    fleet::FarLender::Options o;
    o.size = [this] {
      return _fleet ? fleet::lend_bytes_for(_fleet->config().lend) : 0;
    };
    o.key = [this] {
      const std::string k = _fleet ? _fleet->config().key : std::string();
      return far_wire::from_hex(k.data(), k.size());
    };
    auto l = fleet::FarLender::start(std::move(o));
    if (l.ok()) {
      _lender = std::move(*l);
      VALTZ_LOG_INFO("fleet", "memory lent on port {} (now {} GB)",
                     _lender->port(),
                     fleet::lend_bytes_for(_fleet->config().lend) >> 30);
    } else {
      VALTZ_LOG_WARN("fleet", "no memory lent: {}", l.error().message);
    }
  }
  if (!joined) {
    if (_share) {
      _share.reset();
      VALTZ_LOG_INFO("fleet", "models no longer shared: out of a fleet");
    }
    return;
  }
  if (_share) {
    return;
  }
  auto s = fleet::ShareServer::start(
      [this] { return _fleet ? _fleet->config().key : std::string(); },
      [this](const std::string& model) { return share_list_(model); },
      [this](const std::string& bundle, std::uint64_t bytes) {
        return weights_dir_(bundle, bytes);
      });
  if (!s.ok()) {
    VALTZ_LOG_WARN("fleet", "models not shared: {}", s.error().message);
    return;
  }
  _share = std::move(*s);
}

Result<fleet::ShareList>
Controller::share_list_(const std::string& model) const
{
  if (model.starts_with("job:")) {
    std::lock_guard lk(_share_jobs_mu);
    if (auto it = _share_jobs.find(model); it != _share_jobs.end()) {
      return it->second;
    }
    return make_error(Code::NotFound, "that job's weights are not served "
                                      "now");
  }
  // "<id>@<bits>bit": its compressed copy (DESIGN §8b).
  std::string id = model;
  int want_bits = 0;
  for (const int bits : {8, 4}) {
    const auto tail = std::format("@{}bit", bits);
    if (model.ends_with(tail)) {
      id = model.substr(0, model.size() - tail.size());
      want_bits = bits;
    }
  }
  const auto* e = _catalog.find(id);
  if (!e) {
    return make_error(Code::NotFound, std::format("{} is not a model of "
                                                  "this Valtz", model));
  }
  const fleet::Config fc = _fleet ? _fleet->config() : fleet::Config{};
  if (fc.withheld_models.contains(e->id)) {
    return make_error(Code::NotFound, std::format("{} is not shared",
                                                  e->name));
  }
  const auto info = _store->info(*e);
  if (info.state != models::InstallState::Installed) {
    return make_error(Code::NotFound, std::format("{} is not here",
                                                  e->name));
  }
  // Its compressed copy, in use or set aside -- its mark left out (the
  // copy marks its own).
  if (want_bits > 0) {
    if (info.compressed_bits == want_bits) {
      return fleet::list_model_files(info.dir, e->file);
    }
    if (info.other_bits == want_bits && !info.other_dir.empty()) {
      return fleet::list_model_files(info.other_dir, e->file);
    }
    return make_error(Code::NotFound, std::format(
        "{} has no {}-bit copy here", e->name, want_bits));
  }
  // Its own weights: a compressed copy in use, its original beside it.
  fs::path root = info.dir;
  if (info.compressed_bits > 0) {
    if (info.other_dir.empty() || info.other_bits != 0) {
      return make_error(Code::NotFound, std::format(
          "{} is here only as a compressed copy", e->name));
    }
    root = info.other_dir;
  }
  return fleet::list_model_files(root, e->file);
}

Result<fleet::CopyPlan>
Controller::copy_plan_(const models::ModelEntry& e,
                       const std::string& member, const fs::path& dest,
                       int bits, Json* shared)
{
  if (!_fleet || !_fleet->config().joined()) {
    return make_error(Code::NotFound, msg::kFleetCopyAway,
                      {{"member", member}, {"model", e.name}});
  }
  Json self, found;
  for (const auto& m : _fleet->members()) {
    if (jget<std::string>(m, "id", "") == member ||
        jget<std::string>(m, "name", "") == member) {
      found = m;
      self = jget(m, "self", Json::object());
    }
  }
  const auto name = jget<std::string>(found, "name", member);
  if (!self.is_object() || self.empty()) {
    return make_error(Code::NotFound, msg::kFleetCopyAway,
                      {{"member", name}, {"model", e.name}});
  }
  const int port = jget(self, "share_port", 0);
  if (port <= 0 ||
      (bits == 0 && !in_list(jget(self, "shares", Json()), e.id))) {
    return make_error(Code::NotFound, msg::kFleetCopyNotShared,
                      {{"member", name}, {"model", e.name}});
  }
  if (bits > 0) {
    // Its compressed copy, as the member says it has one (DESIGN §8b).
    Json found_copy;
    const Json copies = jget(jget(self, "shares_compressed",
                                  Json::object()),
                             e.id.c_str(), Json::array());
    for (const auto& c : copies.is_array() ? copies : Json::array()) {
      if (jget(c, "bits", 0) == bits) {
        found_copy = c;
      }
    }
    if (!found_copy.is_object()) {
      return make_error(Code::NotFound, msg::kFleetCopyNoCompressed,
                        {{"member", name}, {"model", e.name},
                         {"bits", std::to_string(bits)}});
    }
    if (shared) {
      *shared = found_copy;
    }
  }
  fleet::CopyPlan plan;
  plan.port = port;
  plan.key = _fleet->config().key;
  plan.model = bits > 0 ? std::format("{}@{}bit", e.id, bits) : e.id;
  plan.dest = dest;
  // Each cable once (a cable bridged reads as its port and its bridge),
  // fastest first; then the member's addresses.
  std::set<std::pair<std::string, std::string>> seen;
  auto links = tb_links_(true)[jget<std::string>(found, "id", member)];
  std::ranges::stable_sort(links, [](const auto& a, const auto& b) {
    return a.gbps > b.gbps;
  });
  for (const auto& l : links) {
    if (seen.insert({l.local_interface, l.remote_address}).second) {
      plan.routes.push_back({l.remote_address, l.local_interface, l.gbps});
    }
  }
  for (const auto& a : jget(self, "addresses", Json::array())) {
    if (a.is_string()) {
      plan.routes.push_back({a.get<std::string>(), "", 0});
    }
  }
  if (plan.routes.empty()) {
    return make_error(Code::NotFound, msg::kFleetCopyAway,
                      {{"member", name}, {"model", e.name}});
  }
  return plan;
}

std::map<std::string, Json>
Controller::fleet_copies_() const
{
  std::map<std::string, Json> out;
  if (!_fleet || !_fleet->config().joined()) {
    return out;
  }
  // The cables as last probed (probed again in the background when
  // stale): a row says which member is a cable away.
  const auto links = const_cast<Controller*>(this)->tb_links_(false);
  for (const auto& m : _fleet->members()) {
    const Json self = jget(m, "self", Json::object());
    if (!self.is_object() || jget(self, "share_port", 0) <= 0) {
      continue;
    }
    const auto id = jget<std::string>(m, "id", "");
    int gbps = 0, cables = 0;
    std::set<std::string> routes;
    if (auto it = links.find(id); it != links.end()) {
      for (const auto& l : it->second) {
        if (routes.insert(l.local_interface + "/" + l.remote_address)
                .second) {
          ++cables;
          gbps += l.gbps;
        }
      }
    }
    const Json from = {{"member", id},
                       {"name", jget<std::string>(m, "name", id)},
                       {"thunderbolt", cables > 0},
                       {"cables", cables},
                       {"gbps", gbps}};
    const auto add = [&](const std::string& model, const Json& item) {
      auto& list = out[model];
      if (!list.is_array()) {
        list = Json::array();
      }
      list.push_back(item);
    };
    for (const auto& model : jget(self, "shares", Json::array())) {
      if (model.is_string()) {
        add(model.get<std::string>(), from);
      }
    }
    // Its compressed copies (DESIGN §8b), each an item of its own.
    const Json squeezed = jget(self, "shares_compressed", Json::object());
    if (squeezed.is_object()) {
      for (auto it = squeezed.begin(); it != squeezed.end(); ++it) {
        if (!it.value().is_array()) {
          continue;
        }
        for (const auto& c : it.value()) {
          const int bits = jget(c, "bits", 0);
          if (bits != 8 && bits != 4) {
            continue;
          }
          Json item = from;
          item["bits"] = bits;
          item["bytes"] = jget<std::uint64_t>(c, "bytes", 0);
          add(it.key(), item);
        }
      }
    }
  }
  return out;
}

// ---- a job's own weights, sent with it ---------------------------------

namespace {

// The keys of a job's params that name a weight file of one's own.
const std::vector<std::string>&
weight_keys()
{
  static const std::vector<std::string> keys = [] {
    std::vector<std::string> k = {"lora_file", "dit_file", "vae_file",
                                  "encoder_weights", "vae_weights"};
    for (int i = 2; i <= engine::kLoraSlots; ++i) {
      k.push_back(std::format("lora{}_file", i));
    }
    return k;
  }();
  return keys;
}

}  // namespace

Result<Json>
Controller::own_weights_(const Json& params, JobId job,
                         std::uint64_t* bytes)
{
  if (!_share || !_fleet) {
    return make_error(Code::Unsupported, "this Mac shares no files");
  }
  fleet::ShareList list;
  Json map = Json::object();
  Hasher h;
  h.update(_fleet->config().member_id);
  std::uint64_t total = 0;
  std::error_code ec;
  // Each file: its place in the bundle, and what tells it apart -- where
  // it is, its size, when it was written (a file changed is another).
  const auto add = [&](const std::string& rel, const fs::path& from) {
    const std::uint64_t n = fs::file_size(from, ec);
    const auto when = fs::last_write_time(from, ec)
                          .time_since_epoch()
                          .count();
    list.files.push_back({rel, n, from});
    total += n;
    h.update(std::format("{}\n{}\n{}\n{}\n", rel, from.string(), n,
                         static_cast<long long>(when)));
  };
  for (const auto& k : weight_keys()) {
    const auto path = jget<std::string>(params, k.c_str(), "");
    if (path.empty()) {
      continue;
    }
    const fs::path p(path);
    if (fs::is_regular_file(p, ec)) {
      const std::string rel = k + "/" + p.filename().string();
      add(rel, p);
      map[k] = rel;
      continue;
    }
    if (!fs::is_directory(p, ec)) {
      return make_error(Code::NotFound, std::format("{} is not there",
                                                    path));
    }
    auto files = fleet::list_model_files(p);
    if (!files.ok()) {
      return files.error();
    }
    for (const auto& f : files->files) {
      add(k + "/" + f.path, p / fs::path(f.path));
    }
    map[k] = k;
  }
  if (list.files.empty()) {
    return make_error(Code::NotFound, "no weight files of its own");
  }
  {
    std::lock_guard lk(_share_jobs_mu);
    _share_jobs["job:" + job.str()] = std::move(list);
  }
  *bytes = total;
  return Json{{"job", job.str()},
              {"bundle", h.finish().hex().substr(0, 32)},
              {"bytes", total},
              {"map", std::move(map)},
              {"sender", _fleet->config().member_id},
              {"addresses", fleet::share_addresses()},
              {"port", _share->port()}};
}

void
Controller::unshare_job_(JobId job)
{
  std::lock_guard lk(_share_jobs_mu);
  _share_jobs.erase("job:" + job.str());
}

void
Controller::cancel_weights_(JobId job)
{
  std::lock_guard lk(_weights_mu);
  if (auto it = _weight_stops.find(job); it != _weight_stops.end()) {
    *it->second = true;
  }
}

Result<fs::path>
Controller::weights_dir_(const std::string& bundle, std::uint64_t bytes)
{
  if (_fleet_files.empty()) {
    return make_error(Code::Unsupported, "this Mac takes no weights");
  }
  const fs::path root = _fleet_files / "weights";
  const fs::path dir = root / bundle;
  std::error_code ec;
  fs::create_directories(root, ec);
  // Weights kept a week since last used; older ones go first.
  const auto now = fs::file_time_type::clock::now();
  for (const auto& d : fs::directory_iterator(root, ec)) {
    const auto t = fs::last_write_time(d.path(), ec);
    if (d.path() != dir && !ec && now - t > std::chrono::hours(24 * 7)) {
      fs::remove_all(d.path(), ec);
    }
  }
  if (!fs::exists(dir / ".valtz-whole", ec)) {
    const auto sp = fs::space(root, ec);
    if (ec || sp.available < bytes + bytes / 10 + (2ull << 30)) {
      return make_error(Code::Busy, std::format(
          "no room for {} of weights", models::size_text(bytes)));
    }
  }
  fs::create_directories(dir, ec);
  fs::last_write_time(dir, now, ec);
  return dir;
}

Status
Controller::serve_weights_(const fleet::Offer& o, const std::string& from,
                           const fs::path& out_dir, engine::JobSink sink)
{
  const Json w = jget(o.params, "weights_from", Json::object());
  const auto bundle = jget<std::string>(w, "bundle", "");
  const Json map = jget(w, "map", Json::object());
  const bool named = !bundle.empty() && bundle.size() <= 64 &&
                     bundle.find_first_not_of("0123456789abcdef") ==
                         std::string::npos;
  if (_fleet_files.empty() || !named || !map.is_object()) {
    return make_error(Code::InvalidArgument,
                      "a fleet job's weights, not as this Valtz reads them");
  }
  // Pushed here by the sender before it offered the job.
  const fs::path dir = _fleet_files / "weights" / bundle;
  std::error_code ec;
  if (!fs::exists(dir / ".valtz-whole", ec)) {
    return make_error(Code::NotFound, "its weights did not come");
  }
  fs::last_write_time(dir, fs::file_time_type::clock::now(), ec);
  // The job as it runs here: its weights' paths in the copy.
  fleet::Offer here = o;
  here.params.erase("weights_from");
  for (auto it = map.begin(); it != map.end(); ++it) {
    const auto rel = it.value().is_string() ? it.value().get<std::string>()
                                            : std::string();
    const fs::path relp(rel);
    if (rel.empty() || relp.is_absolute() ||
        relp.lexically_normal().string().starts_with("..")) {
      return make_error(Code::InvalidArgument,
                        "a fleet job's weights outside their folder");
    }
    here.params[it.key()] = (dir / relp).string();
  }
  VALTZ_LOG_INFO("fleet", "{}: its weights here ({})", o.title,
                 dir.string());
  return fleet_serve_(here, from, out_dir, std::move(sink), true);
}

Status
Controller::push_weights_(const Json& member, const fleet::Offer& offer,
                          std::function<void(Status)> then,
                          engine::JobSink sink)
{
  const Json self = jget(member, "self", Json::object());
  const auto id = jget<std::string>(member, "id", "");
  const Json w = jget(offer.params, "weights_from", Json::object());
  std::vector<fleet::ShareFile> files;
  {
    std::lock_guard lk(_share_jobs_mu);
    auto it = _share_jobs.find("job:" + offer.id.str());
    if (it == _share_jobs.end()) {
      return make_error(Code::NotFound, "the job's weights are not here");
    }
    files = it->second.files;
  }
  fleet::CopyPlan plan;
  plan.port = jget(self, "share_port", 0);
  plan.key = _fleet ? _fleet->config().key : std::string();
  plan.model = jget<std::string>(w, "bundle", "");
  if (plan.port <= 0) {
    return make_error(Code::Unsupported, "the member takes no weights");
  }
  auto stop = std::make_shared<std::atomic<bool>>(false);
  std::lock_guard lk(_weights_mu);
  _weight_stops[offer.id] = stop;
  _weight_threads.emplace_back([this, plan, files, self, id, stop,
                                job = offer.id, title = offer.title,
                                then = std::move(then),
                                sink = std::move(sink)]() mutable {
    // Each cable to the member, fastest first; else its addresses.
    auto links = tb_links_(true)[id];
    std::ranges::stable_sort(links, [](const auto& a, const auto& b) {
      return a.gbps > b.gbps;
    });
    std::set<std::pair<std::string, std::string>> seen;
    for (const auto& l : links) {
      if (seen.insert({l.local_interface, l.remote_address}).second) {
        plan.routes.push_back({l.remote_address, l.local_interface,
                               l.gbps});
      }
    }
    for (const auto& a : jget(self, "addresses", Json::array())) {
      if (a.is_string()) {
        plan.routes.push_back({a.get<std::string>(), "", 0});
      }
    }
    const auto progress = [&](std::uint64_t got, std::uint64_t total) {
      if (stop->load() || withdrawn_(job)) {
        return false;
      }
      engine::JobEvent ev;
      ev.job = job;
      ev.kind = engine::JobEventKind::Progress;
      ev.progress = total > 0 ? static_cast<float>(
                                    static_cast<double>(got) / total)
                              : -1.0f;
      ev.data = {{"phase", std::string(engine::kPhaseDownload)},
                 {"done", got},
                 {"total", total},
                 {"label", "weights"}};
      sink(ev);
      return true;
    };
    auto got = plan.routes.empty()
                   ? Result<fleet::CopyResult>(make_error(
                         Code::Network, "no way to the member"))
                   : fleet::push_files(plan, files, progress);
    {
      std::lock_guard l(_weights_mu);
      _weight_stops.erase(job);
    }
    if (got.ok()) {
      VALTZ_LOG_INFO("fleet", "{}: its weights sent -- {} of {} in "
                     "{:.1f} s ({})", title, models::size_text(got->bytes),
                     models::size_text(got->total), got->seconds,
                     got->routes);
      then(ok_status());
    } else {
      then(got.error());
    }
  });
  return ok_status();
}

}
