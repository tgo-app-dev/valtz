// The fleet's network (fleet.h, DESIGN §11): Bonjour and TLS with a
// pre-shared key through Network.framework.
//
// Every connection's state lives on ONE serial queue; the controller's
// calls reach it asynchronously (submit, answer, self_changed) or read a
// snapshot under a lock (status, members), so neither side waits on the
// other. A connection sends one thing at a time from its OUTBOX -- a
// message, or a file a chunk at a time -- so a job's events, its output
// and its end arrive in the order they were sent.

#import <Foundation/Foundation.h>
#import <Network/Network.h>
#import <Security/CipherSuite.h>
#import <Security/Security.h>

#include "valtz/fleet/fleet.h"

#include "valtz/base/hash.h"
#include "valtz/base/log.h"
#include "valtz/project/records.h"

#include <cstdio>
#include <deque>
#include <format>
#include <map>
#include <mutex>
#include <set>

namespace fs = std::filesystem;

namespace valtz::fleet {

std::string
default_member_name()
{
  @autoreleasepool {
    NSString* n = [[NSHost currentHost] localizedName];
    if (n.length > 0) {
      return n.UTF8String;
    }
    n = [[NSProcessInfo processInfo] hostName];
    return n.length > 0 ? std::string(n.UTF8String) : std::string("Mac");
  }
}

namespace {

constexpr std::size_t kChunk = 256 * 1024;
constexpr const char kIdentity[] = "valtz-fleet";
// An answer asked of the person ("ask"): declined after this long.
constexpr double kAskSeconds = 60;
// How often a member tells the others its state.
constexpr double kBeatSeconds = 5;
// A connection that has not said hello by then is let go: a handshake
// with another secret is refused by the other side, but this one is not
// always told -- it waits on in its handshake.
constexpr double kHelloSeconds = 10;

std::vector<std::uint8_t>
unhex(const std::string& s)
{
  std::vector<std::uint8_t> out;
  auto v = [](char c) -> int {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
  };
  for (std::size_t i = 0; i + 1 < s.size(); i += 2) {
    const int a = v(s[i]), b = v(s[i + 1]);
    if (a < 0 || b < 0) {
      return {};
    }
    out.push_back(static_cast<std::uint8_t>(a * 16 + b));
  }
  return out;
}

dispatch_data_t
data_of(const std::vector<std::uint8_t>& v)
{
  return dispatch_data_create(v.data(), v.size(), nullptr,
                              DISPATCH_DATA_DESTRUCTOR_DEFAULT);
}

std::string
txt_value(nw_txt_record_t txt, const char* key)
{
  __block std::string out;
  if (!txt) {
    return out;
  }
  nw_txt_record_access_key(
      txt, key,
      ^bool(const char*, nw_txt_record_find_key_t found,
            const uint8_t* value, size_t len) {
        if (found == nw_txt_record_find_key_non_empty_value && value) {
          out.assign(reinterpret_cast<const char*>(value), len);
        }
        return true;
      });
  return out;
}

Json
event_json(const engine::JobEvent& e)
{
  return {{"kind", engine::to_str(e.kind)},
          {"progress", e.progress},
          {"step", e.step},
          {"steps", e.steps},
          {"text", e.text},
          {"data", e.data},
          {"error", static_cast<int>(e.error)}};
}

engine::JobEvent
event_from(JobId job, const Json& j)
{
  engine::JobEvent e;
  e.job = job;
  const auto k = jget<std::string>(j, "kind", "");
  using K = engine::JobEventKind;
  for (K kind : {K::Started, K::Progress, K::Preview, K::Text, K::Output,
                 K::Finished, K::Failed, K::Cancelled}) {
    if (k == engine::to_str(kind)) {
      e.kind = kind;
    }
  }
  e.progress = jget(j, "progress", -1.0f);
  e.step = jget(j, "step", 0);
  e.steps = jget(j, "steps", 0);
  e.text = jget<std::string>(j, "text", "");
  e.data = jget(j, "data", Json::object());
  e.error = static_cast<Code>(jget(j, "error", 0));
  return e;
}

// A file name the other side sent: its last part, nothing else.
std::string
safe_name(const std::string& s)
{
  std::string n = fs::path(s).filename().string();
  if (n.empty() || n == "." || n == "..") {
    n = "output";
  }
  return n;
}

// One thing a connection sends: a message, or a file.
struct Outgoing {
  Json     message;      // or a file's header ("t": "file")
  fs::path file;         // set: its bytes follow, chunked
  std::FILE* f = nullptr;
  std::int64_t sid = 0;
};

struct Link {
  std::uint64_t   key = 0;
  nw_connection_t conn;
  std::string     id;     // the member's, once it said hello
  std::string     name;
  std::string     state = "connecting";  // connected | refused | lost
  std::string     browse;  // the service it was found as
  bool            outgoing = false;
  Deframer        in;
  Json            self = Json::object();
  bool            accepting = false;
  bool            serving = false;
  std::deque<Outgoing> outbox;
  bool            sending = false;
};

// A file coming in: an input of a job served, or an output of one sent.
struct Incoming {
  std::FILE*   f = nullptr;
  fs::path     tmp;
  fs::path     final;
  JobId        job;
  bool         output = false;
  ContentHash  hash;
  Json         info;
  Json         data;
};

// A job this Mac sent.
struct OutJob {
  Offer           offer;
  std::string     member;
  fs::path        out_dir;
  engine::JobSink sink;
  std::map<std::string, fs::path> files;  // hash -> where it is here
  int             outputs = 0;
};

// A job this Mac serves.
struct InJob {
  Offer             offer;
  std::uint64_t     link = 0;
  std::string       from;   // the member's name
  std::set<std::string> missing;
  bool              started = false;
  bool              asked = false;
  fs::path          out_dir;
};

// A member found on the network.
struct Found {
  nw_endpoint_t endpoint;
  std::string   fleet;
  std::string   id;
  std::string   name;
  bool          refused = false;
};

class FleetImpl final : public Fleet {
public:
  FleetImpl(fs::path config_path, fs::path work, Host& host, Config c)
      : _config_path(std::move(config_path)), _work(std::move(work)),
        _host(host), _config(c), _qc(std::move(c))
  {
    _q = dispatch_queue_create("com.tgous.valtz.fleet",
                               DISPATCH_QUEUE_SERIAL);
    _alive = std::make_shared<bool>(true);
    std::error_code ec;
    fs::create_directories(_work / "blobs", ec);
    fs::create_directories(_work / "jobs", ec);
    dispatch_async(_q, ^{ rebuild_(); });
    // The heartbeat: this member's state, to every member.
    _beat = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, _q);
    dispatch_source_set_timer(
        _beat, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC),
        static_cast<std::uint64_t>(kBeatSeconds * NSEC_PER_SEC),
        NSEC_PER_SEC / 4);
    auto alive = _alive;
    dispatch_source_set_event_handler(_beat, ^{
      if (*alive) {
        connect_found_();
        tell_all_();
      }
    });
    dispatch_resume(_beat);
  }

  ~FleetImpl() override { stop(); }

  Config
  config() const override
  {
    std::lock_guard lk(_mu);
    return _config;
  }

  Status
  set_config(const Config& c) override
  {
    VALTZ_TRY(save_config(_config_path, c));
    {
      std::lock_guard lk(_mu);
      _config = c;
    }
    dispatch_async(_q, ^{ rebuild_(); });
    return ok_status();
  }

  Json
  status() const override
  {
    std::lock_guard lk(_mu);
    Json s = _snapshot;
    s["config"] = to_json(_config);
    return s;
  }

  std::vector<Json>
  members() const override
  {
    std::lock_guard lk(_mu);
    std::vector<Json> out;
    for (const auto& m : jget(_snapshot, "members", Json::array())) {
      if (jget<std::string>(m, "state", "") == "connected") {
        out.push_back(m);
      }
    }
    return out;
  }

  void
  browse(bool on) override
  {
    dispatch_async(_q, ^{
      _browse_wanted = on;
      browser_();
    });
  }

  Status
  submit(const std::string& member, const Offer& offer,
         const fs::path& out_dir, engine::JobSink sink) override
  {
    auto job = std::make_shared<OutJob>();
    job->offer = offer;
    job->member = member;
    job->out_dir = out_dir;
    job->sink = std::move(sink);
    for (const auto& in : offer.inputs) {
      job->files[in.hash.hex()] = in.path;
    }
    dispatch_async(_q, ^{ send_offer_(job); });
    return ok_status();
  }

  void
  cancel(JobId id) override
  {
    dispatch_async(_q, ^{
      auto it = _out.find(id);
      if (it == _out.end()) {
        return;
      }
      if (Link* l = member_link_(it->second->member)) {
        queue_(*l, {{"t", "cancel"}, {"job", id.str()}});
      }
    });
  }

  void
  answer(JobId id, bool accept) override
  {
    dispatch_async(_q, ^{ answered_(id, accept); });
  }

  void
  self_changed() override
  {
    dispatch_async(_q, ^{ tell_all_(); });
  }

  void
  connect(const std::string& host, int port) override
  {
    const std::string h = host;
    const std::string p = std::to_string(port);
    dispatch_async(_q, ^{
      nw_endpoint_t ep = nw_endpoint_create_host(h.c_str(), p.c_str());
      open_(ep, "");
    });
  }

  void
  stop() override
  {
    if (!_alive || !*_alive) {
      return;
    }
    dispatch_sync(_q, ^{
      *_alive = false;
      dispatch_source_cancel(_beat);
      teardown_("stopped");
      if (_browser) {
        nw_browser_cancel(_browser);
        _browser = nil;
      }
    });
  }

private:
  // ---- the network, as the configuration says ------------------------

  nw_parameters_t
  params_()
  {
    const auto key = unhex(_qc.key);
    dispatch_data_t psk = data_of(key);
    dispatch_data_t ident = dispatch_data_create(
        kIdentity, sizeof kIdentity - 1, nullptr,
        DISPATCH_DATA_DESTRUCTOR_DEFAULT);
    nw_parameters_t p = nw_parameters_create_secure_tcp(
        ^(nw_protocol_options_t tls) {
          sec_protocol_options_t sec =
              nw_tls_copy_sec_protocol_options(tls);
          sec_protocol_options_add_pre_shared_key(sec, psk, ident);
          sec_protocol_options_append_tls_ciphersuite(
              sec, static_cast<tls_ciphersuite_t>(
                       TLS_PSK_WITH_AES_128_GCM_SHA256));
          sec_protocol_options_set_min_tls_protocol_version(
              sec, tls_protocol_version_TLSv12);
          sec_protocol_options_set_max_tls_protocol_version(
              sec, tls_protocol_version_TLSv12);
          // Every connection proves the key afresh: a session resumed
          // from the process's cache would skip it (one that knew the
          // secret before, or another member of this process).
          sec_protocol_options_set_tls_resumption_enabled(sec, false);
          sec_protocol_options_set_tls_tickets_enabled(sec, false);
        },
        ^(nw_protocol_options_t tcp) {
          nw_tcp_options_set_no_delay(tcp, true);
          nw_tcp_options_set_enable_keepalive(tcp, true);
          nw_tcp_options_set_keepalive_idle_time(tcp, 10);
        });
    nw_parameters_set_include_peer_to_peer(p, false);
    return p;
  }

  // Everything closed: jobs sent told why, jobs served cancelled.
  void
  teardown_(const std::string& why)
  {
    if (_listener) {
      nw_listener_cancel(_listener);
      _listener = nil;
    }
    _port = 0;
    for (auto& [k, l] : _links) {
      nw_connection_cancel(l->conn);
    }
    _links.clear();
    for (auto& [id, j] : _out) {
      fail_out_(*j, why);
    }
    _out.clear();
    for (auto& [id, j] : _in) {
      if (j->started) {
        _host.fleet_cancel(id);
      }
    }
    _in.clear();
    for (auto& [k, in] : _incoming) {
      if (in.f) {
        std::fclose(in.f);
      }
    }
    _incoming.clear();
    for (auto& [k, f] : _found) {
      f.refused = false;
    }
  }

  void
  rebuild_()
  {
    if (!*_alive) {
      return;
    }
    teardown_("left");
    {
      std::lock_guard lk(_mu);
      _qc = _config;
    }
    if (_qc.joined()) {
      _params = params_();
      if (_qc.discoverable) {
        listen_();
      }
    } else {
      _params = nil;
    }
    browser_();
    connect_found_();
    publish_();
  }

  void
  listen_()
  {
    nw_listener_t l = nw_listener_create(_params);
    if (!l) {
      VALTZ_LOG_WARN("fleet", "cannot listen");
      return;
    }
    nw_advertise_descriptor_t ad =
        nw_advertise_descriptor_create_bonjour_service(
            _qc.member_name.c_str(), kService, nullptr);
    nw_txt_record_t txt = nw_txt_record_create_dictionary();
    auto set = [&](const char* k, const std::string& v) {
      nw_txt_record_set_key(txt, k,
                            reinterpret_cast<const uint8_t*>(v.data()),
                            v.size());
    };
    set("fleet", _qc.fleet);
    set("id", _qc.member_id);
    set("v", std::to_string(kProtocol));
    nw_advertise_descriptor_set_txt_record_object(ad, txt);
    nw_listener_set_advertise_descriptor(l, ad);
    nw_listener_set_queue(l, _q);
    auto alive = _alive;
    nw_listener_set_state_changed_handler(
        l, ^(nw_listener_state_t s, nw_error_t e) {
          if (!*alive) {
            return;
          }
          if (s == nw_listener_state_ready) {
            _port = nw_listener_get_port(l);
            VALTZ_LOG_INFO("fleet", "{} listening on port {} as {}",
                           _qc.fleet, _port, _qc.member_name);
            publish_();
          } else if (s == nw_listener_state_failed) {
            VALTZ_LOG_WARN("fleet", "the listener failed: {}",
                           e ? nw_error_get_error_code(e) : 0);
          }
        });
    nw_listener_set_new_connection_handler(l, ^(nw_connection_t c) {
      if (*alive) {
        adopt_(c, false, "");
      }
    });
    nw_listener_start(l);
    _listener = l;
  }

  // Looking for fleets: a member always, for its own; anyone while the
  // Fleet page is open.
  void
  browser_()
  {
    const bool want = _qc.joined() || _browse_wanted;
    if (!want) {
      if (_browser) {
        nw_browser_cancel(_browser);
        _browser = nil;
        _found.clear();
        publish_();
      }
      return;
    }
    if (_browser) {
      return;
    }
    nw_browse_descriptor_t d =
        nw_browse_descriptor_create_bonjour_service(kService, nullptr);
    nw_browse_descriptor_set_include_txt_record(d, true);
    nw_parameters_t p = nw_parameters_create();
    nw_parameters_set_include_peer_to_peer(p, false);
    nw_browser_t b = nw_browser_create(d, p);
    nw_browser_set_queue(b, _q);
    auto alive = _alive;
    nw_browser_set_browse_results_changed_handler(
        b, ^(nw_browse_result_t was, nw_browse_result_t now, bool) {
          if (*alive) {
            found_(was, now);
          }
        });
    nw_browser_set_state_changed_handler(
        b, ^(nw_browser_state_t s, nw_error_t e) {
          if (s == nw_browser_state_failed && *alive) {
            VALTZ_LOG_WARN("fleet", "browsing failed: {}",
                           e ? nw_error_get_error_code(e) : 0);
          }
        });
    nw_browser_start(b);
    _browser = b;
  }

  void
  found_(nw_browse_result_t was, nw_browse_result_t now)
  {
    const auto changes = nw_browse_result_get_changes(was, now);
    nw_browse_result_t r = now ? now : was;
    nw_endpoint_t ep = nw_browse_result_copy_endpoint(r);
    const char* svc = nw_endpoint_get_bonjour_service_name(ep);
    const std::string key = svc ? svc : "";
    if (changes & nw_browse_result_change_result_removed) {
      _found.erase(key);
    } else {
      nw_txt_record_t txt = nw_browse_result_copy_txt_record_object(r);
      Found& f = _found[key];
      f.endpoint = ep;
      f.fleet = txt_value(txt, "fleet");
      f.id = txt_value(txt, "id");
      f.name = key;
    }
    connect_found_();
    publish_();
  }

  // Each member of this fleet found and not yet reached: connected to --
  // by the one of the two with the smaller id when both are
  // discoverable, so a pair keeps one connection.
  void
  connect_found_()
  {
    if (!_qc.joined()) {
      return;
    }
    for (auto& [key, f] : _found) {
      if (f.fleet != _qc.fleet || f.id.empty() ||
          f.id == _qc.member_id || f.refused || reached_(f.id)) {
        continue;
      }
      if (_qc.discoverable && _qc.member_id > f.id) {
        continue;
      }
      open_(f.endpoint, key);
    }
  }

  bool
  reached_(const std::string& id)
  {
    for (auto& [k, l] : _links) {
      if (l->id == id || (!l->browse.empty() && l->state == "connecting" &&
                          _found.contains(l->browse) &&
                          _found[l->browse].id == id)) {
        return true;
      }
    }
    return false;
  }

  void
  open_(nw_endpoint_t ep, const std::string& browse)
  {
    if (!_params) {
      return;
    }
    nw_connection_t c = nw_connection_create(ep, _params);
    adopt_(c, true, browse);
  }

  void
  adopt_(nw_connection_t c, bool outgoing, const std::string& browse)
  {
    auto l = std::make_unique<Link>();
    l->key = ++_next_link;
    l->conn = c;
    l->outgoing = outgoing;
    l->browse = browse;
    const std::uint64_t key = l->key;
    _links[key] = std::move(l);
    nw_connection_set_queue(c, _q);
    auto alive = _alive;
    nw_connection_set_state_changed_handler(
        c, ^(nw_connection_state_t s, nw_error_t e) {
          if (*alive) {
            link_state_(key, s, e);
          }
        });
    nw_connection_start(c);
    dispatch_after(
        dispatch_time(DISPATCH_TIME_NOW,
                      static_cast<std::int64_t>(kHelloSeconds * NSEC_PER_SEC)),
        _q, ^{
          if (*alive) {
            unanswered_(key);
          }
        });
  }

  // Still no hello: reached, but not let in -- most likely another
  // secret.
  void
  unanswered_(std::uint64_t key)
  {
    auto it = _links.find(key);
    if (it == _links.end() || it->second->state == "connected") {
      return;
    }
    Link& l = *it->second;
    if (l.outgoing && !l.browse.empty() && _found.contains(l.browse)) {
      _found[l.browse].refused = true;
      VALTZ_LOG_INFO("fleet", "{} did not take the fleet's key", l.browse);
    }
    drop_(key, "refused");
  }

  void
  link_state_(std::uint64_t key, nw_connection_state_t s, nw_error_t e)
  {
    auto it = _links.find(key);
    if (it == _links.end()) {
      return;
    }
    Link& l = *it->second;
    if (s == nw_connection_state_ready) {
      queue_(l, hello_());
      receive_(key);
      return;
    }
    if (s == nw_connection_state_failed ||
        s == nw_connection_state_cancelled ||
        (s == nw_connection_state_waiting && e)) {
      // A handshake refused: the other side holds another secret.
      const bool tls = e && nw_error_get_error_domain(e) ==
                                nw_error_domain_tls;
      if (tls && !l.browse.empty() && _found.contains(l.browse)) {
        _found[l.browse].refused = true;
      }
      VALTZ_LOG_INFO("fleet", "{} {}{}",
                     l.name.empty() ? l.browse : l.name,
                     tls ? "refused the fleet's key" : "gone",
                     e ? std::format(" ({})", nw_error_get_error_code(e))
                       : "");
      drop_(key, tls ? "refused" : "lost");
    }
  }

  void
  drop_(std::uint64_t key, const std::string& why)
  {
    auto it = _links.find(key);
    if (it == _links.end()) {
      return;
    }
    std::unique_ptr<Link> l = std::move(it->second);
    _links.erase(it);
    nw_connection_cancel(l->conn);
    for (auto& o : l->outbox) {
      if (o.f) {
        std::fclose(o.f);
      }
    }
    // Its jobs: those sent to it fail, those from it stop.
    if (!l->id.empty()) {
      for (auto j = _out.begin(); j != _out.end();) {
        if (j->second->member == l->id) {
          fail_out_(*j->second, why);
          j = _out.erase(j);
        } else {
          ++j;
        }
      }
    }
    for (auto j = _in.begin(); j != _in.end();) {
      if (j->second->link == key) {
        if (j->second->started) {
          _host.fleet_cancel(j->first);
        }
        j = _in.erase(j);
      } else {
        ++j;
      }
    }
    for (auto i = _incoming.begin(); i != _incoming.end();) {
      if (i->first.first == key) {
        if (i->second.f) {
          std::fclose(i->second.f);
        }
        i = _incoming.erase(i);
      } else {
        ++i;
      }
    }
    publish_();
    _host.fleet_note("fleet.changed", Json::object());
  }

  void
  receive_(std::uint64_t key)
  {
    auto it = _links.find(key);
    if (it == _links.end()) {
      return;
    }
    auto alive = _alive;
    nw_connection_receive(
        it->second->conn, 1, 1 << 20,
        ^(dispatch_data_t content, nw_content_context_t, bool complete,
          nw_error_t e) {
          if (!*alive) {
            return;
          }
          auto lt = _links.find(key);
          if (lt == _links.end()) {
            return;
          }
          if (content) {
            Link* l = lt->second.get();
            dispatch_data_apply(
                content, ^bool(dispatch_data_t, size_t, const void* buf,
                               size_t n) {
                  l->in.feed({static_cast<const std::uint8_t*>(buf), n});
                  return true;
                });
            while (auto m = l->in.next()) {
              if (!m->ok()) {
                VALTZ_LOG_WARN("fleet", "a message that does not read");
                drop_(key, "lost");
                return;
              }
              handle_(key, **m);
              if (!_links.contains(key)) {
                return;
              }
            }
          }
          if (e || complete) {
            // Ended by the other side's TLS alert, before it said hello:
            // it holds another secret.
            Link& l = *lt->second;
            const bool tls = e && nw_error_get_error_domain(e) ==
                                      nw_error_domain_tls;
            if (tls && l.state != "connected" && !l.browse.empty() &&
                _found.contains(l.browse)) {
              _found[l.browse].refused = true;
              VALTZ_LOG_INFO("fleet", "{} refused the fleet's key", l.browse);
            }
            drop_(key, tls ? "refused" : "lost");
            return;
          }
          receive_(key);
        });
  }

  // ---- sending -------------------------------------------------------

  void
  queue_(Link& l, Json message)
  {
    Outgoing o;
    o.message = std::move(message);
    l.outbox.push_back(std::move(o));
    pump_(l.key);
  }

  void
  queue_file_(Link& l, Json header, const fs::path& file)
  {
    Outgoing o;
    o.sid = ++_next_stream;
    header["sid"] = o.sid;
    o.message = std::move(header);
    o.file = file;
    l.outbox.push_back(std::move(o));
    pump_(l.key);
  }

  void
  pump_(std::uint64_t key)
  {
    auto it = _links.find(key);
    if (it == _links.end()) {
      return;
    }
    Link& l = *it->second;
    if (l.sending || l.outbox.empty()) {
      return;
    }
    Outgoing& o = l.outbox.front();
    Json m;
    bool last = true;
    if (o.file.empty()) {
      m = std::move(o.message);
    } else if (!o.f) {
      // A file's header, then its bytes.
      o.f = std::fopen(o.file.c_str(), "rb");
      m = o.message;
      last = !o.f;
      if (!o.f) {
        m["missing"] = true;
      }
    } else {
      std::vector<std::uint8_t> buf(kChunk);
      const std::size_t n = std::fread(buf.data(), 1, buf.size(), o.f);
      buf.resize(n);
      last = n < kChunk;
      m = {{"t", "chunk"}, {"sid", o.sid}, {"data", Json::binary(buf)},
           {"end", last}};
      if (last) {
        std::fclose(o.f);
        o.f = nullptr;
      } else {
        last = false;
      }
    }
    if (last) {
      l.outbox.pop_front();
    }
    l.sending = true;
    auto alive = _alive;
    nw_connection_send(l.conn, data_of(frame(m)),
                       NW_CONNECTION_DEFAULT_MESSAGE_CONTEXT, true,
                       ^(nw_error_t e) {
                         if (!*alive) {
                           return;
                         }
                         auto lt = _links.find(key);
                         if (lt == _links.end()) {
                           return;
                         }
                         lt->second->sending = false;
                         if (e) {
                           drop_(key, "lost");
                           return;
                         }
                         pump_(key);
                       });
  }

  Json
  self_()
  {
    Json s = _host.fleet_self();
    s["serving"] = serving_now_();
    s["accepting"] = _qc.accepting_at(std::time(nullptr));
    s["accept"] = _qc.accept;
    return s;
  }

  Json
  hello_()
  {
    return {{"t", "hello"},
            {"protocol", kProtocol},
            {"id", _qc.member_id},
            {"name", _qc.member_name},
            {"fleet", _qc.fleet},
            {"self", self_()}};
  }

  void
  tell_all_()
  {
    if (_links.empty()) {
      return;
    }
    Json m = {{"t", "status"}, {"self", self_()}};
    for (auto& [k, l] : _links) {
      if (l->state == "connected") {
        queue_(*l, m);
      }
    }
  }

  bool
  serving_now_() const
  {
    for (const auto& [id, j] : _in) {
      if (j->started) {
        return true;
      }
    }
    return false;
  }

  Link*
  member_link_(const std::string& member)
  {
    for (auto& [k, l] : _links) {
      if (l->state == "connected" && l->id == member) {
        return l.get();
      }
    }
    return nullptr;
  }

  // ---- messages ------------------------------------------------------

  void
  handle_(std::uint64_t key, const Json& m)
  {
    Link& l = *_links[key];
    const auto t = jget<std::string>(m, "t", "");
    if (t == "hello") {
      hello_from_(l, m);
      return;
    }
    if (l.state != "connected") {
      return;
    }
    if (t == "status") {
      // Told the app only when it changed: a heartbeat says the same.
      Json self = jget(m, "self", Json::object());
      if (self != l.self) {
        l.self = std::move(self);
        publish_();
        _host.fleet_note("fleet.changed", Json::object());
      }
    } else if (t == "offer") {
      offered_(l, m);
    } else if (t == "accept" || t == "decline" || t == "event" ||
               t == "done") {
      reply_(l, t, m);
    } else if (t == "need") {
      needed_(l, m);
    } else if (t == "file") {
      file_(l, m);
    } else if (t == "chunk") {
      chunk_(l, m);
    } else if (t == "cancel") {
      cancelled_(l, m);
    }
  }

  void
  hello_from_(Link& l, const Json& m)
  {
    const auto id = jget<std::string>(m, "id", "");
    if (jget(m, "protocol", 0) != kProtocol ||
        jget<std::string>(m, "fleet", "") != _qc.fleet || id.empty() ||
        id == _qc.member_id) {
      drop_(l.key, "refused");
      return;
    }
    // A pair keeps one connection: a second one goes.
    for (auto& [k, other] : _links) {
      if (k != l.key && other->id == id && other->state == "connected") {
        drop_(l.key, "duplicate");
        return;
      }
    }
    l.id = id;
    l.name = jget<std::string>(m, "name", "");
    l.self = jget(m, "self", Json::object());
    l.state = "connected";
    VALTZ_LOG_INFO("fleet", "{} joined us in {}", l.name, _qc.fleet);
    publish_();
    _host.fleet_note("fleet.changed", Json::object());
  }

  // -- serving --

  void
  decline_(Link& l, JobId job, const std::string& reason)
  {
    VALTZ_LOG_INFO("fleet", "declined {}'s job: {}", l.name, reason);
    queue_(l, {{"t", "decline"}, {"job", job.str()}, {"reason", reason}});
  }

  void
  offered_(Link& l, const Json& m)
  {
    auto o = offer_from_json(jget(m, "offer", Json::object()));
    if (!o.ok()) {
      return;
    }
    Offer offer = std::move(*o);
    if (!_qc.accepting_at(std::time(nullptr))) {
      decline_(l, offer.id, _qc.accept == "never" ? "not-accepting"
                                                      : "schedule");
      return;
    }
    const Json me = _host.fleet_self();
    if (serving_now_() || !_in.empty() || jget(me, "busy", false)) {
      decline_(l, offer.id, "busy");
      return;
    }
    auto ops = jget(me, "ops", Json::array());
    const bool runs_op = std::ranges::any_of(ops, [&](const Json& x) {
      return x.is_string() && x.get<std::string>() == offer.op;
    });
    auto have = jget(me, "installed", Json::array());
    const bool has_model =
        offer.model.empty()
            ? !jget<std::string>(me, "assistant", "").empty()
            : std::ranges::any_of(have, [&](const Json& x) {
                return x.is_string() && x.get<std::string>() == offer.model;
              });
    if (!runs_op || !has_model) {
      decline_(l, offer.id, "cannot-run");
      return;
    }
    auto job = std::make_shared<InJob>();
    job->link = l.key;
    job->from = l.name;
    for (auto& in : offer.inputs) {
      in.path = _work / "blobs" / in.path;
    }
    job->offer = std::move(offer);
    const JobId id = job->offer.id;
    _in[id] = job;
    if (_qc.accept == "ask") {
      job->asked = true;
      _host.fleet_note("fleet.ask", {{"job", id.str()},
                                     {"from", l.name},
                                     {"op", job->offer.op},
                                     {"model", job->offer.model},
                                     {"title", job->offer.title}});
      auto alive = _alive;
      dispatch_after(
          dispatch_time(DISPATCH_TIME_NOW,
                        static_cast<std::int64_t>(kAskSeconds * NSEC_PER_SEC)),
          _q, ^{
            if (*alive) {
              answered_(id, false, "no-answer");
            }
          });
      publish_();
      return;
    }
    accept_(*job);
  }

  void
  answered_(JobId id, bool yes, const std::string& reason = "declined")
  {
    auto it = _in.find(id);
    if (it == _in.end() || !it->second->asked) {
      return;
    }
    InJob& j = *it->second;
    j.asked = false;
    auto lt = _links.find(j.link);
    if (lt == _links.end()) {
      _in.erase(it);
      return;
    }
    _host.fleet_note("fleet.ask", {{"job", id.str()}, {"done", true}});
    if (!yes) {
      decline_(*lt->second, id, reason);
      _in.erase(it);
      publish_();
      return;
    }
    accept_(j);
  }

  void
  accept_(InJob& j)
  {
    auto lt = _links.find(j.link);
    if (lt == _links.end()) {
      return;
    }
    Link& l = *lt->second;
    queue_(l, {{"t", "accept"}, {"job", j.offer.id.str()}});
    // What it does not hold, asked for by hash.
    Json need = Json::array();
    for (const auto& in : j.offer.inputs) {
      std::error_code ec;
      if (!fs::is_regular_file(in.path, ec)) {
        if (j.missing.insert(in.hash.hex()).second) {
          need.push_back(in.hash.hex());
        }
      }
    }
    if (!need.empty()) {
      queue_(l, {{"t", "need"}, {"job", j.offer.id.str()},
                 {"hashes", need}});
      publish_();
      return;
    }
    start_(j);
  }

  void
  start_(InJob& j)
  {
    j.started = true;
    j.out_dir = _work / "jobs" / j.offer.id.str();
    std::error_code ec;
    fs::create_directories(j.out_dir, ec);
    const JobId id = j.offer.id;
    VALTZ_LOG_INFO("fleet", "serving {}'s job: {} ({})", j.from,
                   j.offer.title, j.offer.op);
    publish_();
    tell_all_();
    auto alive = _alive;
    dispatch_queue_t q = _q;
    Status st = _host.fleet_serve(
        j.offer, j.from, j.out_dir,
        [this, alive, q, id](const engine::JobEvent& ev) {
          if (ev.kind == engine::JobEventKind::Preview || !*alive) {
            return;
          }
          engine::JobEvent copy = ev;
          dispatch_async(q, ^{
            if (*alive) {
              served_event_(id, copy);
            }
          });
        });
    if (!st.ok()) {
      engine::JobEvent ev;
      ev.job = id;
      ev.kind = engine::JobEventKind::Failed;
      ev.error = st.error().code;
      ev.text = st.error().message;
      served_event_(id, ev);
    }
  }

  void
  served_event_(JobId id, const engine::JobEvent& ev)
  {
    auto it = _in.find(id);
    if (it == _in.end()) {
      return;
    }
    InJob& j = *it->second;
    auto lt = _links.find(j.link);
    if (lt == _links.end()) {
      return;
    }
    Link& l = *lt->second;
    if (ev.kind == engine::JobEventKind::Output && !ev.output.empty()) {
      queue_file_(l,
                  {{"t", "file"}, {"job", id.str()}, {"output", true},
                   {"name", safe_name(ev.output.filename().string())},
                   {"info", ev.output_info}, {"data", ev.data}},
                  ev.output);
      return;
    }
    if (!ev.terminal()) {
      queue_(l, {{"t", "event"}, {"job", id.str()},
                 {"event", event_json(ev)}});
      return;
    }
    queue_(l, {{"t", "done"}, {"job", id.str()}, {"event", event_json(ev)}});
    VALTZ_LOG_INFO("fleet", "{}'s job {}", j.from, engine::to_str(ev.kind));
    // Its files go once they are sent: the outbox holds them open.
    const fs::path dir = j.out_dir;
    _in.erase(it);
    queue_cleanup_(l, dir);
    publish_();
    tell_all_();
  }

  // The served job's folder removed once what is queued before it is
  // sent.
  void
  queue_cleanup_(Link& l, const fs::path& dir)
  {
    _cleanup[l.key].push_back(dir);
    auto alive = _alive;
    const std::uint64_t key = l.key;
    // Checked after each send: the folder goes when the outbox is empty.
    dispatch_async(_q, ^{
      if (*alive) {
        sweep_(key);
      }
    });
  }

  void
  sweep_(std::uint64_t key)
  {
    auto lt = _links.find(key);
    if (lt != _links.end() && !lt->second->outbox.empty()) {
      auto alive = _alive;
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC / 2), _q,
                     ^{
                       if (*alive) {
                         sweep_(key);
                       }
                     });
      return;
    }
    for (const auto& d : _cleanup[key]) {
      std::error_code ec;
      fs::remove_all(d, ec);
    }
    _cleanup.erase(key);
    trim_blobs_();
  }

  // The inputs received are kept, by content, for the next job that
  // needs them -- the oldest dropped past 4 GB.
  void
  trim_blobs_()
  {
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    std::uintmax_t total = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(_work / "blobs", ec)) {
      if (e.is_regular_file(ec)) {
        total += e.file_size(ec);
        files.emplace_back(e.last_write_time(ec), e.path());
      }
    }
    std::ranges::sort(files);
    for (const auto& [t, p] : files) {
      if (total <= (4ull << 30)) {
        break;
      }
      total -= fs::file_size(p, ec);
      fs::remove(p, ec);
    }
  }

  void
  cancelled_(Link& l, const Json& m)
  {
    auto id = JobId::parse(jget<std::string>(m, "job", ""));
    if (!id) {
      return;
    }
    auto it = _in.find(*id);
    if (it == _in.end() || it->second->link != l.key) {
      return;
    }
    if (it->second->started) {
      _host.fleet_cancel(*id);
      return;
    }
    engine::JobEvent ev;
    ev.job = *id;
    ev.kind = engine::JobEventKind::Cancelled;
    queue_(l, {{"t", "done"}, {"job", id->str()}, {"event", event_json(ev)}});
    _in.erase(it);
    publish_();
  }

  // -- files --

  void
  needed_(Link& l, const Json& m)
  {
    auto id = JobId::parse(jget<std::string>(m, "job", ""));
    if (!id) {
      return;
    }
    auto it = _out.find(*id);
    if (it == _out.end()) {
      return;
    }
    for (const auto& h : jget(m, "hashes", Json::array())) {
      const auto hex = h.is_string() ? h.get<std::string>() : "";
      auto f = it->second->files.find(hex);
      if (f == it->second->files.end()) {
        continue;
      }
      queue_file_(l, {{"t", "file"}, {"job", id->str()}, {"hash", hex}},
                  f->second);
    }
  }

  void
  file_(Link& l, const Json& m)
  {
    const auto sid = jget<std::int64_t>(m, "sid", 0);
    auto id = JobId::parse(jget<std::string>(m, "job", ""));
    if (!id) {
      return;
    }
    Incoming in;
    in.job = *id;
    in.output = jget(m, "output", false);
    if (in.output) {
      auto it = _out.find(*id);
      if (it == _out.end()) {
        return;
      }
      OutJob& j = *it->second;
      in.final = j.out_dir / std::format("{}-fleet{}-{}", id->str(),
                                         j.outputs++,
                                         safe_name(jget<std::string>(
                                             m, "name", "")));
      in.info = jget(m, "info", Json::object());
      in.data = jget(m, "data", Json::object());
    } else {
      auto it = _in.find(*id);
      auto h = ContentHash::parse(jget<std::string>(m, "hash", ""));
      if (it == _in.end() || !h) {
        return;
      }
      in.hash = *h;
      for (const auto& x : it->second->offer.inputs) {
        if (x.hash == *h) {
          in.final = x.path;
        }
      }
      if (in.final.empty()) {
        return;
      }
    }
    if (jget(m, "missing", false)) {
      VALTZ_LOG_WARN("fleet", "{} could not send a file", l.name);
      if (!in.output) {
        engine::JobEvent ev;
        ev.job = *id;
        ev.kind = engine::JobEventKind::Failed;
        ev.error = Code::NotFound;
        ev.text = "an input was not sent";
        queue_(l, {{"t", "done"}, {"job", id->str()},
                   {"event", event_json(ev)}});
        _in.erase(*id);
        publish_();
      }
      return;
    }
    in.tmp = in.final.string() + ".part";
    in.f = std::fopen(in.tmp.c_str(), "wb");
    if (!in.f) {
      VALTZ_LOG_WARN("fleet", "cannot write {}", in.tmp.string());
      return;
    }
    _incoming[{l.key, sid}] = std::move(in);
  }

  void
  chunk_(Link& l, const Json& m)
  {
    const auto sid = jget<std::int64_t>(m, "sid", 0);
    auto it = _incoming.find({l.key, sid});
    if (it == _incoming.end()) {
      return;
    }
    Incoming& in = it->second;
    const Json& d = m["data"];
    if (d.is_binary()) {
      const auto& b = d.get_binary();
      std::fwrite(b.data(), 1, b.size(), in.f);
    }
    if (!jget(m, "end", false)) {
      return;
    }
    std::fclose(in.f);
    in.f = nullptr;
    Incoming done = std::move(in);
    _incoming.erase(it);
    std::error_code ec;
    if (done.output) {
      fs::rename(done.tmp, done.final, ec);
      auto jt = _out.find(done.job);
      if (jt == _out.end()) {
        return;
      }
      engine::JobEvent ev;
      ev.job = done.job;
      ev.kind = engine::JobEventKind::Output;
      ev.output = done.final;
      ev.output_info = jget(Json{{"i", done.info}}, "i", media::MediaInfo{});
      ev.data = done.data;
      jt->second->sink(ev);
      return;
    }
    // An input: kept only as what it says it is.
    auto h = hash_file(done.tmp);
    if (!h.ok() || *h != done.hash) {
      VALTZ_LOG_WARN("fleet", "an input from {} did not match its hash",
                     l.name);
      fs::remove(done.tmp, ec);
      return;
    }
    fs::rename(done.tmp, done.final, ec);
    auto jt = _in.find(done.job);
    if (jt == _in.end()) {
      return;
    }
    jt->second->missing.erase(done.hash.hex());
    if (jt->second->missing.empty() && !jt->second->started) {
      start_(*jt->second);
    }
  }

  // -- jobs sent --

  void
  send_offer_(std::shared_ptr<OutJob> job)
  {
    Link* l = member_link_(job->member);
    if (!l) {
      fail_out_(*job, "lost");
      return;
    }
    _out[job->offer.id] = job;
    queue_(*l, {{"t", "offer"}, {"offer", to_json(job->offer)}});
    VALTZ_LOG_INFO("fleet", "offered {} to {}", job->offer.title, l->name);
  }

  void
  reply_(Link& l, const std::string& t, const Json& m)
  {
    auto id = JobId::parse(jget<std::string>(m, "job", ""));
    if (!id) {
      return;
    }
    auto it = _out.find(*id);
    if (it == _out.end() || it->second->member != l.id) {
      return;
    }
    OutJob& j = *it->second;
    if (t == "accept") {
      VALTZ_LOG_INFO("fleet", "{} took {}", l.name, j.offer.title);
      return;
    }
    if (t == "decline") {
      engine::JobEvent ev;
      ev.job = *id;
      ev.kind = engine::JobEventKind::Failed;
      ev.error = Code::Busy;
      const auto reason = jget<std::string>(m, "reason", "");
      ev.text = std::format("{} declined the job ({})", l.name, reason);
      ev.data = {{"fleet", "declined"}, {"reason", reason},
                 {"member", l.name}};
      auto keep = it->second;
      _out.erase(it);
      keep->sink(ev);
      return;
    }
    engine::JobEvent ev = event_from(*id, jget(m, "event", Json::object()));
    if (t == "done") {
      auto keep = it->second;
      _out.erase(it);
      keep->sink(ev);
      return;
    }
    j.sink(ev);
  }

  void
  fail_out_(OutJob& j, const std::string& why)
  {
    engine::JobEvent ev;
    ev.job = j.offer.id;
    ev.kind = engine::JobEventKind::Failed;
    ev.error = Code::Network;
    ev.text = std::format("the fleet member was {}", why);
    ev.data = {{"fleet", why}};
    j.sink(ev);
  }

  // ---- what the app reads --------------------------------------------

  void
  publish_()
  {
    Json members = Json::array();
    std::set<std::string> listed;
    for (const auto& [k, l] : _links) {
      if (l->id.empty() || listed.contains(l->id)) {
        continue;
      }
      listed.insert(l->id);
      members.push_back({{"id", l->id}, {"name", l->name},
                         {"state", l->state}, {"self", l->self}});
    }
    // Found, of this fleet, and not reached.
    std::map<std::string, int> fleets;
    for (const auto& [key, f] : _found) {
      if (f.id.empty() || f.id == _qc.member_id) {
        continue;
      }
      ++fleets[f.fleet];
      if (_qc.joined() && f.fleet == _qc.fleet &&
          !listed.contains(f.id)) {
        listed.insert(f.id);
        members.push_back({{"id", f.id}, {"name", f.name},
                           {"state", f.refused ? "refused" : "found"},
                           {"self", Json::object()}});
      }
    }
    Json fl = Json::array();
    for (const auto& [name, n] : fleets) {
      fl.push_back({{"fleet", name}, {"members", n}});
    }
    Json serving = Json();
    Json asking = Json::array();
    for (const auto& [id, j] : _in) {
      if (j->started) {
        serving = {{"job", id.str()}, {"from", j->from},
                   {"title", j->offer.title}, {"op", j->offer.op},
                   {"model", j->offer.model}};
      } else if (j->asked) {
        asking.push_back({{"job", id.str()}, {"from", j->from},
                          {"title", j->offer.title}, {"op", j->offer.op},
                          {"model", j->offer.model}});
      }
    }
    Json sent = Json::array();
    for (const auto& [id, j] : _out) {
      sent.push_back({{"job", id.str()}, {"member", j->member},
                      {"title", j->offer.title}});
    }
    std::lock_guard lk(_mu);
    _snapshot = {{"port", _port},
                 {"browsing", _browser != nil},
                 {"members", members},
                 {"fleets", fl},
                 {"serving", serving},
                 {"asking", asking},
                 {"sent", sent}};
  }

  fs::path _config_path;
  fs::path _work;
  Host&    _host;
  mutable std::mutex _mu;
  Config   _config;  // the controller's, under _mu
  Json     _snapshot = Json::object();
  std::shared_ptr<bool> _alive;

  // On the queue alone.
  Config            _qc;  // _config, as the network was last made for
  dispatch_queue_t  _q;
  dispatch_source_t _beat;
  nw_parameters_t   _params;
  nw_listener_t     _listener;
  nw_browser_t      _browser;
  int               _port = 0;
  bool              _browse_wanted = false;
  std::uint64_t     _next_link = 0;
  std::int64_t      _next_stream = 0;
  std::map<std::uint64_t, std::unique_ptr<Link>> _links;
  std::map<std::string, Found> _found;
  std::map<JobId, std::shared_ptr<OutJob>> _out;
  std::map<JobId, std::shared_ptr<InJob>>  _in;
  std::map<std::pair<std::uint64_t, std::int64_t>, Incoming> _incoming;
  std::map<std::uint64_t, std::vector<fs::path>> _cleanup;
};

}  // namespace

std::unique_ptr<Fleet>
make_fleet(fs::path config_path, fs::path work_dir, Host& host)
{
  auto c = load_config(config_path);
  Config cfg = c.ok() ? *c : Config{};
  if (!c.ok()) {
    VALTZ_LOG_WARN("fleet", "{} not read: {}", config_path.string(),
                   c.error().message);
  }
  if (cfg.member_id.empty()) {
    cfg.member_id = Uuid::v7().str();
  }
  if (cfg.member_name.empty()) {
    cfg.member_name = default_member_name();
  }
  // Kept: its member id is its own from now on.
  (void)save_config(config_path, cfg);
  return std::make_unique<FleetImpl>(std::move(config_path),
                                     std::move(work_dir), host,
                                     std::move(cfg));
}

}
