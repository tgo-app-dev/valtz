// The fleet (fleet/fleet.h, DESIGN §11): its configuration, schedule,
// key and framing; and, with VALTZ_TEST_FLEET=1, two members on this Mac
// over the loopback -- a job sent, its input asked for by hash, its
// events and output back; a member that declines; another secret kept
// out.

#include "testing.h"

#include "valtz/base/hash.h"
#include "valtz/fleet/fleet.h"
#include "valtz/project/records.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <unistd.h>

using namespace valtz;
using namespace valtz::fleet;
namespace fs = std::filesystem;

namespace {

fs::path
temp_dir(const char* name)
{
  fs::path d = fs::temp_directory_path() /
               std::format("valtz-fleet-{}-{}", name, getpid());
  fs::remove_all(d);
  fs::create_directories(d);
  return d;
}

std::time_t
at(int wday, int hour, int minute)
{
  // A day of a known week: 2026-10-05 was a Monday.
  std::tm tm{};
  tm.tm_year = 2026 - 1900;
  tm.tm_mon = 9;
  tm.tm_mday = 4 + (wday == 0 ? 7 : wday);
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_isdst = -1;
  return std::mktime(&tm);
}

}  // namespace

TEST(fleet, a_schedule_opens_on_its_days_and_hours)
{
  Schedule s;
  CHECK(s.open_at(at(3, 3, 0)));  // off: always
  s.on = true;
  s.days = 0x1f;  // Monday to Friday
  s.from = 9 * 60;
  s.to = 18 * 60;
  CHECK(s.open_at(at(1, 9, 0)));
  CHECK(s.open_at(at(5, 17, 59)));
  CHECK(!s.open_at(at(5, 18, 0)));
  CHECK(!s.open_at(at(1, 8, 59)));
  CHECK(!s.open_at(at(6, 12, 0)));  // Saturday
  // Past midnight: Friday night into Saturday's small hours.
  s.from = 22 * 60;
  s.to = 6 * 60;
  CHECK(s.open_at(at(5, 23, 0)));
  CHECK(s.open_at(at(6, 2, 0)));   // Friday's night
  CHECK(!s.open_at(at(1, 2, 0)));  // Sunday's night: Sunday is off
  CHECK(!s.open_at(at(3, 12, 0)));
  Config c;
  c.accept = "never";
  CHECK(!c.accepting_at(at(3, 12, 0)));
}

TEST(fleet, the_key_is_the_name_and_the_secret)
{
  const auto a = derive_key("Studio", "orchard-lantern-42");
  CHECK(a.size() == 64);
  CHECK(a == derive_key("Studio", "orchard-lantern-42"));
  CHECK(a != derive_key("Studio", "orchard-lantern-43"));
  CHECK(a != derive_key("Studios", "orchard-lantern-42"));
}

TEST(fleet, the_configuration_keeps_its_key_not_its_secret)
{
  const fs::path d = temp_dir("config");
  const fs::path f = d / "fleet.json";
  auto fresh = load_config(f);
  REQUIRE_OK(fresh);
  CHECK(!fresh->member_id.empty());
  CHECK(!fresh->member_name.empty());
  CHECK(!fresh->joined());
  Config c = *fresh;
  c.fleet = "Studio";
  c.key = derive_key("Studio", "orchard-lantern-42");
  c.discoverable = true;
  c.accept = "ask";
  c.schedule.on = true;
  c.schedule.days = 0x60;
  REQUIRE_OK(save_config(f, c));
  std::ifstream in(f);
  const std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  CHECK(text.find("orchard") == std::string::npos);
  CHECK((fs::status(f).permissions() & fs::perms::others_read) ==
        fs::perms::none);
  auto back = load_config(f);
  REQUIRE_OK(back);
  CHECK(back->member_id == c.member_id);
  CHECK(back->key == c.key);
  CHECK(back->joined());
  CHECK(back->discoverable);
  CHECK(back->accept == "ask");
  CHECK(back->schedule.days == 0x60);
  CHECK(!jget(to_json(*back), "key", Json()).is_string());
  CHECK(jget(to_json(*back), "secret_set", false));
  fs::remove_all(d);
}

TEST(fleet, messages_are_framed_and_read_whole)
{
  const Json a = {{"t", "hello"}, {"n", 1}};
  const Json b = {{"t", "chunk"},
                  {"data", Json::binary(std::vector<std::uint8_t>(70000, 7))}};
  std::vector<std::uint8_t> wire = frame(a);
  const auto fb = frame(b);
  wire.insert(wire.end(), fb.begin(), fb.end());
  Deframer d;
  // Fed a few bytes at a time, as a connection reads.
  std::vector<Json> got;
  for (std::size_t i = 0; i < wire.size(); i += 333) {
    d.feed({wire.data() + i, std::min<std::size_t>(333, wire.size() - i)});
    while (auto m = d.next()) {
      REQUIRE_OK(*m);
      got.push_back(**m);
    }
  }
  REQUIRE(got.size() == 2);
  CHECK(got[0] == a);
  CHECK(got[1]["data"].get_binary().size() == 70000);
  Offer o;
  o.id = JobId::make();
  o.op = "generate-image";
  o.model = "krea2-turbo";
  o.params = {{"prompt", "a kite"}};
  const fs::path d2 = temp_dir("offer");
  std::ofstream(d2 / "in.png") << "picture";
  engine::JobInput ji;
  ji.role = "base";
  ji.path = d2 / "in.png";
  ji.hash = *hash_file(ji.path);
  o.inputs.push_back(ji);
  auto r = offer_from_json(to_json(o));
  REQUIRE_OK(r);
  CHECK(r->id == o.id);
  CHECK(r->params == o.params);
  REQUIRE(r->inputs.size() == 1);
  CHECK(r->inputs[0].hash == ji.hash);
  CHECK(r->inputs[0].path == fs::path(ji.hash.hex() + ".png"));
  fs::remove_all(d2);
}

namespace {

// A member's controller, as the fleet sees it: runs "generate-image" by
// writing its input's bytes, reversed, as its output.
class FakeHost final : public Host {
public:
  Json
  fleet_self() override
  {
    return {{"ops", {"generate-image"}},
            {"installed", {"krea2-turbo"}},
            {"runs", {"image/generate/krea2-turbo"}},
            {"features", {"text-to-image"}},
            {"assistant", ""},
            {"busy", false},
            {"machine", {{"chip", "test"}, {"ram_gb", 64}}}};
  }

  Status
  fleet_serve(const Offer& o, const std::string& from, const fs::path& out,
              engine::JobSink sink) override
  {
    served_from = from;
    std::string bytes;
    if (!o.inputs.empty()) {
      std::ifstream in(o.inputs[0].path, std::ios::binary);
      bytes.assign(std::istreambuf_iterator<char>(in), {});
      received = *hash_file(o.inputs[0].path) == o.inputs[0].hash;
    }
    std::thread([o, out, bytes, sink] {
      engine::JobEvent e;
      e.job = o.id;
      e.kind = engine::JobEventKind::Started;
      sink(e);
      e.kind = engine::JobEventKind::Progress;
      e.progress = 0.5f;
      e.data = {{"phase", "denoise"}, {"done", 4}, {"total", 8}};
      sink(e);
      const fs::path made = out / "made.png";
      std::ofstream(made, std::ios::binary)
          << std::string(bytes.rbegin(), bytes.rend());
      e.kind = engine::JobEventKind::Output;
      e.output = made;
      e.data = {{"score", "X:1"}};
      sink(e);
      e = {};
      e.job = o.id;
      e.kind = engine::JobEventKind::Finished;
      sink(e);
    }).detach();
    return ok_status();
  }

  void fleet_cancel(JobId) override {}
  void fleet_note(std::string, Json) override {}

  std::string       served_from;
  std::atomic<bool> received = false;
};

// A job's events, waited for.
struct Collected {
  std::mutex                    mu;
  std::condition_variable       cv;
  std::vector<engine::JobEvent> events;
  bool                          done = false;

  engine::JobSink
  sink()
  {
    return [this](const engine::JobEvent& e) {
      std::lock_guard lk(mu);
      events.push_back(e);
      done = done || e.terminal();
      cv.notify_all();
    };
  }

  bool
  wait(int seconds)
  {
    std::unique_lock lk(mu);
    return cv.wait_for(lk, std::chrono::seconds(seconds),
                       [&] { return done; });
  }
};

bool
eventually(const std::function<bool()>& f, int seconds)
{
  for (int i = 0; i < seconds * 20; ++i) {
    if (f()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

}  // namespace

TEST(fleet, a_job_goes_to_a_member_and_comes_back)
{
  if (!std::getenv("VALTZ_TEST_FLEET")) {
    SKIP("set VALTZ_TEST_FLEET=1 (two members on this Mac's loopback)");
  }
  const fs::path d = temp_dir("net");
  const std::string key = derive_key("Test fleet", "a-long-test-secret");
  auto made = [&](const char* name, bool discoverable) {
    Config c;
    c.member_id = Uuid::v7().str();
    c.member_name = name;
    c.fleet = "Test fleet";
    c.key = key;
    c.discoverable = discoverable;
    (void)save_config(d / (std::string(name) + ".json"), c);
  };
  made("server", true);
  made("client", false);
  FakeHost hs, hc;
  auto server = make_fleet(d / "server.json", d / "server-files", hs);
  auto client = make_fleet(d / "client.json", d / "client-files", hc);
  int port = 0;
  REQUIRE(eventually([&] {
    port = jget(server->status(), "port", 0);
    return port > 0;
  }, 10));
  client->connect("127.0.0.1", port);
  REQUIRE(eventually([&] { return client->members().size() == 1; }, 10));
  const Json member = client->members().front();
  CHECK(jget<std::string>(member, "name", "") == "server");
  CHECK(jget(jget(member, "self", Json::object()), "accepting", false));

  // A job with an input: asked for by hash, sent, run there, its output
  // back as a file.
  std::ofstream(d / "input.png", std::ios::binary) << "0123456789";
  Offer o;
  o.id = JobId::make();
  o.op = "generate-image";
  o.model = "krea2-turbo";
  o.title = "a kite";
  engine::JobInput ji;
  ji.role = "base";
  ji.path = d / "input.png";
  ji.hash = *hash_file(ji.path);
  o.inputs.push_back(ji);
  fs::create_directories(d / "out");
  Collected got;
  REQUIRE_OK(client->submit(jget<std::string>(member, "id", ""), o,
                            d / "out", got.sink()));
  REQUIRE(got.wait(20));
  CHECK(hs.received.load());
  CHECK(hs.served_from == "client");
  std::lock_guard lk(got.mu);
  bool progressed = false;
  fs::path output;
  for (const auto& e : got.events) {
    progressed = progressed || (e.kind == engine::JobEventKind::Progress &&
                                jget(e.data, "done", 0) == 4);
    if (e.kind == engine::JobEventKind::Output) {
      output = e.output;
      CHECK(jget<std::string>(e.data, "score", "") == "X:1");
    }
  }
  CHECK(progressed);
  CHECK(got.events.back().kind == engine::JobEventKind::Finished);
  REQUIRE(!output.empty());
  CHECK(output.parent_path() == d / "out");
  std::ifstream back(output, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(back)), {});
  CHECK(bytes == "9876543210");

  // Not taking jobs: declined, said why.
  Config sc = server->config();
  sc.accept = "never";
  REQUIRE_OK(server->set_config(sc));
  REQUIRE(eventually([&] {
    port = jget(server->status(), "port", 0);
    return port > 0;
  }, 10));
  client->connect("127.0.0.1", port);
  // Reached again, and heard to be not taking jobs.
  REQUIRE(eventually([&] {
    const auto ms = client->members();
    return ms.size() == 1 &&
           !jget(jget(ms.front(), "self", Json::object()), "accepting",
                 true);
  }, 15));
  Offer o2 = o;
  o2.id = JobId::make();
  Collected no;
  REQUIRE_OK(client->submit(
      jget<std::string>(client->members().front(), "id", ""), o2,
      d / "out", no.sink()));
  REQUIRE(no.wait(20));
  {
    std::lock_guard lk2(no.mu);
    REQUIRE(!no.events.empty());
    const auto& e = no.events.back();
    CHECK(e.kind == engine::JobEventKind::Failed);
    CHECK(jget<std::string>(e.data, "fleet", "") == "declined");
    CHECK(jget<std::string>(e.data, "reason", "") == "not-accepting");
  }

  // Another secret: kept out.
  Config other;
  other.member_id = Uuid::v7().str();
  other.member_name = "stranger";
  other.fleet = "Test fleet";
  other.key = derive_key("Test fleet", "not-the-secret");
  (void)save_config(d / "stranger.json", other);
  FakeHost hx;
  auto stranger = make_fleet(d / "stranger.json", d / "stranger-files", hx);
  stranger->connect("127.0.0.1", port);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  CHECK(stranger->members().empty());
  stranger->stop();
  client->stop();
  server->stop();
  fs::remove_all(d);
}
