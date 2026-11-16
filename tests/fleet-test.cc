// The fleet (fleet/fleet.h, DESIGN §11): its configuration, schedule,
// key and framing; and, with VALTZ_TEST_FLEET=1, two members on this Mac
// over the loopback -- a job sent, its input asked for by hash, its
// events and output back; a member that declines; another secret kept
// out; the members' arithmetic added up.

#include "testing.h"

#include "valtz/base/hash.h"
#include "valtz/fleet/fleet.h"
#include "valtz/fleet/model-share.h"
#include "valtz/fleet/thunderbolt.h"
#include "valtz/project/records.h"

#include <atomic>
#include <cmath>
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

// Where this Mac's jobs go (Config::offload): a model's own policy, else
// its task's, else local first -- and what it shares (Config::shares):
// all but what is withheld; both kept in its file, an unknown policy
// read as none.
TEST(fleet, offload_policies_and_sharing_are_kept)
{
  Config c;
  CHECK(c.offload("generate-video", "minimax-h3-fl2va") == "local-first");
  c.offload_tasks["generate-video"] = kOffloadFleetFirst;
  c.offload_models["minimax-h3-ref2va"] = kOffloadAlways;
  CHECK(c.offload("generate-video", "minimax-h3-fl2va") == "fleet-first");
  CHECK(c.offload("generate-video", "minimax-h3-ref2va") == "always");
  CHECK(c.offload("generate-image", "minimax-h3-ref2va") == "always");
  CHECK(c.offload("chat", "") == "local-first");
  CHECK(c.shares("chat", ""));
  c.withheld_tasks.insert("chat");
  c.withheld_models.insert("krea2-turbo");
  CHECK(!c.shares("chat", ""));
  CHECK(!c.shares("generate-image", "krea2-turbo"));
  CHECK(c.shares("generate-image", "z-image-turbo"));
  CHECK(is_offload_policy("never") && !is_offload_policy("sometimes"));

  const fs::path d = temp_dir("offload");
  const fs::path f = d / "fleet.json";
  REQUIRE_OK(save_config(f, c));
  auto back = load_config(f);
  REQUIRE_OK(back);
  CHECK(back->offload_tasks == c.offload_tasks);
  CHECK(back->offload_models == c.offload_models);
  CHECK(back->withheld_tasks == c.withheld_tasks);
  CHECK(back->withheld_models == c.withheld_models);
  const Json j = to_json(*back);
  CHECK(jget<std::string>(jget(jget(j, "offload", Json::object()), "tasks",
                               Json::object()),
                          "generate-video", "") == "fleet-first");
  // Written by hand: a policy that is none is dropped, not kept.
  std::ofstream(f) << R"({"offload": {"tasks": {"chat": "sometimes",
                     "generate-image": "never"}}})";
  auto hand = load_config(f);
  REQUIRE_OK(hand);
  CHECK(hand->offload_tasks.size() == 1);
  CHECK(hand->offload("generate-image", "") == "never");
  fs::remove_all(d);
}

// THUNDERBOLT between members (thunderbolt.h, DESIGN §11c): a bridge's
// ports and the addresses it learned on each, as ifconfig prints them;
// the links an echo's answers make -- the ports facing each other, and
// their RDMA devices -- through a bridge on both Macs, through unbridged
// ports cable by cable, crossed; and the pair kept with the rest.
TEST(fleet, thunderbolt_cables_are_found_port_to_port)
{
  // As the M5 Pro printed it, a cable in en1 to the other Mac's en6.
  const std::string ifconfig =
      "bridge0: flags=8863<UP,BROADCAST,SMART,RUNNING,SIMPLEX,MULTICAST> "
      "mtu 1500\n"
      "\tether 02:00:00:00:00:00\n"
      "\tinet 169.254.245.114 netmask 0xffff0000 broadcast 169.254.255.255\n"
      "\tConfiguration:\n"
      "\t\tid 0:0:0:0:0:0 priority 0 hellotime 0 fwddelay 0\n"
      "\tmember: en1 flags=3<LEARNING,DISCOVER>\n"
      "\t        ifmaxaddr 0 port 10 priority 0 path cost 0\n"
      "\tmember: en6 flags=3<LEARNING,DISCOVER>\n"
      "\t        ifmaxaddr 0 port 11 priority 0 path cost 0\n"
      "\tmember: en2 flags=3<LEARNING,DISCOVER>\n"
      "\t        ifmaxaddr 0 port 12 priority 0 path cost 0\n"
      "\tAddress cache:\n"
      "\t\t36:5c:16:8d:fe:0 Vlan1 en1 1194 flags=0<>\n"
      "\tnd6 options=201<PERFORMNUD,DAD>\n"
      "\tmedia: autoselect\n"
      "\tstatus: active\n";
  const BridgeTable t = parse_bridge(ifconfig);
  CHECK((t.members == std::vector<std::string>{"en1", "en6", "en2"}));
  CHECK(t.learned.size() == 1);
  CHECK(t.learned.at("36:5c:16:8d:fe:00") == "en1");
  CHECK(normal_mac("36:5C:16:8d:fe:0") == "36:5c:16:8d:fe:00");
  CHECK(normal_mac("bridge0").empty());
  CHECK(normal_mac("1:2:3:4:5").empty());

  // Both bridged, one cable: the bridge's addresses answer, its ports
  // face each other by what the bridge learned, RDMA on both.
  TbState pro;
  pro.ports = {{"en1", "36:d6:9f:8a:76:c0", true, "", "rdma_en1", "", "", 0,
                ""},
               {"en2", "36:d6:9f:8a:76:c8", false, "", "", "", "", 0, ""},
               {"en6", "36:d6:9f:8a:76:c4", false, "", "", "", "", 0, ""}};
  pro.bridge = "bridge0";
  pro.bridge_address = "169.254.245.114";
  pro.bridged = {"en1", "en6", "en2"};
  pro.rdma = true;
  TbState ultra;
  ultra.ports = {{"en1", "36:5c:16:8d:fe:04", false, "", "", "", "", 0, ""},
                 {"en6", "36:5c:16:8d:fe:00", true, "", "rdma_en6", "", "", 0,
                  ""}};
  ultra.bridge = "bridge0";
  ultra.bridge_address = "169.254.64.133";
  ultra.bridged = {"en1", "en6"};
  ultra.rdma = true;
  CHECK(pro.linked() && ultra.linked());
  auto links = tb_links(pro, ultra, {{"bridge0", "169.254.64.133", 0.21}},
                        t.learned);
  REQUIRE(links.size() == 1);
  CHECK(links[0].local_interface == "bridge0");
  CHECK(links[0].local_address == "169.254.245.114");
  CHECK(links[0].remote_interface == "bridge0");
  CHECK(links[0].local_port == "en1");
  CHECK(links[0].remote_port == "en6");
  CHECK(links[0].local_rdma == "rdma_en1");
  CHECK(links[0].remote_rdma == "rdma_en6");
  CHECK(links[0].rdma());
  // Learned nowhere yet: the lone ports with a cable.
  links = tb_links(pro, ultra, {{"bridge0", "169.254.64.133", 0.2}}, {});
  REQUIRE(links.size() == 1);
  CHECK(links[0].local_port == "en1" && links[0].remote_port == "en6");
  // RDMA down at one end: the cable, over TCP.
  TbState no_rdma = ultra;
  no_rdma.ports[1].rdma.clear();
  links = tb_links(pro, no_rdma, {{"bridge0", "169.254.64.133", 0.2}}, {});
  REQUIRE(links.size() == 1);
  CHECK(!links[0].rdma());

  // Two Mac Studios, two cables, no bridge: each port its own address.
  // Crossed -- a's en1 to b's en3, a's en2 to b's en1 -- and the echoes
  // say so; a port answering for two addresses is one link.
  TbState a, b;
  a.ports = {{"en1", "aa:00:00:00:00:01", true, "169.254.1.1", "rdma_en1",
              "", "", 0, ""},
             {"en2", "aa:00:00:00:00:02", true, "169.254.1.2", "rdma_en2",
              "", "", 0, ""}};
  b.ports = {{"en1", "bb:00:00:00:00:01", true, "169.254.2.1", "rdma_en1",
              "", "", 0, ""},
             {"en3", "bb:00:00:00:00:03", true, "169.254.2.3", "rdma_en3",
              "", "", 0, ""}};
  links = tb_links(a, b,
                   {{"en1", "169.254.2.3", 0.1},
                    {"en2", "169.254.2.1", 0.1},
                    {"en1", "169.254.2.1", 0.3}},
                   {});
  REQUIRE(links.size() == 2);
  CHECK(links[0].local_port == "en1" && links[0].remote_port == "en3");
  CHECK(links[0].local_rdma == "rdma_en1");
  CHECK(links[0].remote_rdma == "rdma_en3");
  CHECK(links[0].local_address == "169.254.1.1");
  CHECK(links[1].local_port == "en2" && links[1].remote_port == "en1");
  CHECK(links[1].remote_rdma == "rdma_en1");
  // An address that is none of the member's Thunderbolt: no link.
  CHECK(tb_links(a, b, {{"en1", "10.0.0.7", 0.1}}, {}).empty());

  // Two cables between two Mac Studios, BOTH Macs' ports in their
  // Thunderbolt Bridges (as macOS sets them up), one a TB5 cable at 80
  // Gb/s and one at 20: the bridges answer as one route, and each port's
  // Thunderbolt domains -- its own bus's, the Mac's at its other end --
  // pair them cable by cable (as cabled 2026-11-12: here en3 to
  // there en3 at 80, here en9 to there en2 at 20), the faster first,
  // each with its RDMA devices and speed.
  TbState m4, m3;
  m4.ports = {{"en3", "36:aa:00:00:00:03", true, "", "rdma_en3",
               "D-00E2", "D-9924", 80, ""},
              {"en9", "36:aa:00:00:00:09", true, "", "rdma_en9",
               "D-47CD", "D-B904", 20, ""},
              {"en4", "36:aa:00:00:00:04", false, "", "", "D-9AE3", "",
               0, ""}};
  m4.bridge = "bridge0";
  m4.bridge_address = "169.254.63.26";
  m4.bridged = {"en3", "en9", "en4"};
  m3.ports = {{"en2", "36:bb:00:00:00:02", true, "", "rdma_en2",
               "D-B904", "D-47CD", 20, ""},
              {"en3", "36:bb:00:00:00:03", true, "", "rdma_en3",
               "D-9924", "D-00E2", 80, ""}};
  m3.bridge = "bridge0";
  m3.bridge_address = "169.254.191.72";
  m3.bridged = {"en2", "en3"};
  auto cl = tb_links(m4, m3, {{"bridge0", "169.254.191.72", 0.3}}, {});
  REQUIRE(cl.size() == 2);
  CHECK(cl[0].local_port == "en3" && cl[0].remote_port == "en3");
  CHECK(cl[0].local_rdma == "rdma_en3");
  CHECK(cl[0].remote_rdma == "rdma_en3");
  CHECK(cl[0].gbps == 80);
  CHECK(cl[1].local_port == "en9" && cl[1].remote_port == "en2");
  CHECK(cl[1].gbps == 20);
  for (const auto& l : cl) {
    CHECK(l.local_interface == "bridge0");
    CHECK(l.remote_address == "169.254.191.72");
  }
  // A member that tells no domains (before them): the bridge's one link,
  // as before.
  TbState old_m3 = m3;
  for (auto& p : old_m3.ports) {
    p.domain.clear();
    p.peer.clear();
  }
  cl = tb_links(m4, old_m3, {{"bridge0", "169.254.191.72", 0.3}}, {});
  CHECK(cl.size() == 1);
  CHECK(tb_state_from_json(to_json(m4)) == m4);

  // The 20 Gb/s cable's ports taken out of the Thunderbolt Bridges, each
  // put by macOS in a bridge of its own (bridge1, as on 2026-11-13): that
  // cable answers through those bridges' addresses, bound to bridge1.
  TbState s4 = m4, s3 = m3;
  s4.bridged = {"en3", "en4"};
  s4.ports[1].address = "169.254.57.246";
  s4.ports[1].via = "bridge1";
  s3.bridged = {"en3"};
  s3.ports[0].address = "169.254.199.65";
  s3.ports[0].via = "bridge1";
  cl = tb_links(s4, s3, {{"bridge0", "169.254.191.72", 0.3},
                         {"bridge1", "169.254.199.65", 0.4}}, {});
  REQUIRE(cl.size() == 2);
  CHECK(cl[0].local_interface == "bridge0" && cl[0].gbps == 80);
  CHECK(cl[1].local_interface == "bridge1");
  CHECK(cl[1].local_address == "169.254.57.246");
  CHECK(cl[1].remote_address == "169.254.199.65");
  CHECK(cl[1].local_port == "en9" && cl[1].remote_port == "en2");
  CHECK(cl[1].local_rdma == "rdma_en9" && cl[1].gbps == 20);
  CHECK(tb_state_from_json(to_json(s4)) == s4);

  // A cable whose port at the member's end shares a bridge of its own
  // with ANOTHER port (as on 2026-11-13: the other Mac's en5 and en6 in
  // bridge3, a cable in en6 alone): both ports hold that bridge's
  // address, and the echo answering it pairs the cable to en6 -- it was
  // read as en5's, which had none, and the cable was dropped.
  TbState f4 = s4, f3 = s3;
  f4.ports[2] = {"en4", "36:aa:00:00:00:04", true, "169.254.44.189",
                 "rdma_en4", "D-9AE3", "D-6E06", 80, "bridge3"};
  f3.ports.push_back({"en5", "36:bb:00:00:00:05", false, "169.254.57.69",
                      "", "D-6E05", "", 0, "bridge3"});
  f3.ports.push_back({"en6", "36:bb:00:00:00:06", true, "169.254.57.69",
                      "rdma_en6", "D-6E06", "D-9AE3", 80, "bridge3"});
  cl = tb_links(f4, f3, {{"bridge0", "169.254.191.72", 0.3},
                         {"bridge1", "169.254.199.65", 0.4},
                         {"bridge3", "169.254.57.69", 0.5}}, {});
  REQUIRE(cl.size() == 3);
  bool en4 = false;
  for (const auto& l : cl) {
    if (l.local_port == "en4") {
      en4 = true;
      CHECK(l.remote_port == "en6");
      CHECK(l.remote_interface == "bridge3");
      CHECK(l.local_interface == "bridge3");
      CHECK(l.local_rdma == "rdma_en4" && l.remote_rdma == "rdma_en6");
    }
  }
  CHECK(en4);

  // What a member tells the others comes back as it was.
  const TbState back = tb_state_from_json(to_json(pro));
  CHECK(back == pro);
  const TbLink lb = tb_link_from_json(to_json(links[1]));
  CHECK(lb.remote_port == "en1" && lb.local_rdma == "rdma_en2");
  CHECK(tb_link_from_json(to_json(tb_links(m4, m3,
                              {{"bridge0", "169.254.191.72", 0.3}},
                              {})[0])).gbps == 80);

  // Each rank makes the split's key from its own fleet key and the job:
  // the same on both, another job's another.
  const std::string k = derive_key("Studio", "orchard-lantern-42");
  CHECK(split_key(k, "job-1").size() == 64);
  CHECK(split_key(k, "job-1") == split_key(k, "job-1"));
  CHECK(split_key(k, "job-1") != split_key(k, "job-2"));
  CHECK(split_key("zz", "job-1").empty());

  // Each Mac's share by its GPU: its cores, a GPU before M5's a quarter
  // each -- said, or read from its chip's name.
  CHECK(std::abs(gpu_share(20, true, {{"gpu_cores", 80},
                                      {"matrix_cores", true}}) -
                 0.2) < 1e-9);
  CHECK(std::abs(gpu_share(20, true, {{"gpu_cores", 40},
                                      {"matrix_cores", false}}) -
                 20.0 / 30.0) < 1e-9);
  CHECK(std::abs(gpu_share(76, false, {{"gpu_cores", 20},
                                       {"chip", "Apple M5 Pro"}}) -
                 19.0 / 39.0) < 1e-9);
  CHECK(std::abs(gpu_share(20, true, {{"gpu_cores", 60},
                                      {"chip", "Apple M2 Ultra"}}) -
                 20.0 / 35.0) < 1e-9);

  // The pair kept with the configuration.
  Config c;
  c.fleet = "Studio";
  c.key = k;
  c.pair_member = "m-2";
  c.pair_name = "studio";
  c.pair_rdma = false;
  c.pair_share = 0.25;
  const fs::path d = temp_dir("pair");
  REQUIRE_OK(save_config(d / "fleet.json", c));
  auto kept = load_config(d / "fleet.json");
  REQUIRE_OK(kept);
  CHECK(kept->pair_member == "m-2" && kept->pair_name == "studio");
  CHECK(!kept->pair_rdma);
  CHECK(std::abs(kept->pair_share - 0.25) < 1e-9);
  CHECK(jget<std::string>(jget(to_json(*kept), "pair", Json::object()),
                          "name", "") == "studio");
  fs::remove_all(d);
}

// A SPLIT'S ROWS (fleet/thunderbolt.h, DESIGN §11c): the heads in whole
// groups -- Krea 2's four query heads a key head -- and the feed-forward's
// rows a whole number a head where they are, else whole grains.
TEST(fleet, a_splits_rows_take_whole_groups)
{
  using fleet::split_rows;
  // H3: 56 heads, 256 rows a head.
  CHECK((split_rows(56, 14336, 0.2, 1, 1) == std::pair<int, int>{11, 2816}));
  // What does not divide evenly goes to the faster Mac: the slower one's
  // share rounded down, the faster one's up -- the two agree.
  CHECK((split_rows(56, 14336, 0.2, 4, 1) == std::pair<int, int>{8, 2048}));
  CHECK((split_rows(56, 14336, 0.8, 4, 1) ==
         std::pair<int, int>{48, 12288}));
  // An M5 Ultra beside an M5 Max (2:1): 40 heads to 16, not 36 to 20.
  CHECK((split_rows(56, 14336, 2.0 / 3, 4, 1) ==
         std::pair<int, int>{40, 10240}));
  CHECK((split_rows(56, 14336, 1.0 / 3, 4, 1) ==
         std::pair<int, int>{16, 4096}));
  // Exact stays exact.
  CHECK((split_rows(56, 14336, 0.75, 1, 1) ==
         std::pair<int, int>{42, 10752}));
  // Krea 2: 48 heads in groups of 4, 16384 rows (341.3 a head) in 256s.
  const auto k = split_rows(48, 16384, 0.3, 4, 256);
  CHECK(k.first % 4 == 0 && k.first == 12);
  CHECK(k.second % 256 == 0 && k.second == 4096);
  const auto k7 = split_rows(48, 16384, 0.7, 4, 256);
  CHECK(k7.first == 36 && k7.second == 12288);
  // Fibo 2.0: 9536 rows, 149 groups of 64.
  const auto f = split_rows(28, 9536, 0.5, 1, 64);
  CHECK(f.first == 14 && f.second % 64 == 0 && f.second == 4800);
  // Never all to one Mac.
  const auto z = split_rows(30, 10240, 0.99, 1, 256);
  CHECK(z.first == 29 && z.second > 0 && z.second < 10240 &&
        z.second % 256 == 0);
  const auto m = split_rows(24, 8192, 0.01, 4, 256);
  CHECK(m.first == 4 && m.second >= 256);
}

// A MODEL COPIED FROM A MEMBER (fleet/model-share.h, DESIGN §11d), over
// the loopback: its files listed and fetched in pieces by several
// streams, byte for byte; a Mac without the fleet's key answered
// nothing; a copy again keeping what is whole and fetching what is not;
// stopped part way, the files not whole left as `.part`; a member
// sharing nothing of a model saying so.
TEST(fleet, a_model_is_copied_from_a_member)
{
  const fs::path root = temp_dir("share");
  const fs::path there = root / "there" / "Org" / "Model";
  fs::create_directories(there / "text_encoder");
  // A file of several pieces, one of none, small ones beside them.
  std::vector<char> big(150u << 20);
  for (std::size_t i = 0; i < big.size(); ++i) {
    big[i] = static_cast<char>((i * 2654435761u) >> 13);
  }
  std::ofstream(there / "model.safetensors", std::ios::binary)
      .write(big.data(), static_cast<std::streamsize>(big.size()));
  std::ofstream(there / "config.json") << "{\"a\": 1}";
  std::ofstream(there / "empty.txt");
  std::ofstream(there / "text_encoder" / "w.safetensors") << "weights";
  std::ofstream(there / "left.safetensors.part") << "a download's";
  std::ofstream(there / "valtz-compressed.json") << "{}";
  auto listed = list_model_files(there);
  REQUIRE_OK(listed);
  REQUIRE(listed->files.size() == 4);
  CHECK(listed->files[0].path == "config.json");
  CHECK(listed->files[3].path == "text_encoder/w.safetensors");
  // A model that is ONE FILE of its repo (an adapter in a folder of its
  // own: Pika H3 Flex's fl2va/adapter.safetensors) goes with what its
  // download brings that is no weights -- its sampling manifest beside
  // it, the repo's LICENSE at the top -- and not the other models' files
  // there (fl2va's other adapter, ref2va's folder, the top's weights).
  {
    const fs::path repo = root / "there" / "Pika" / "Flex";
    fs::create_directories(repo / "fl2va");
    fs::create_directories(repo / "ref2va");
    std::ofstream(repo / "fl2va" / "adapter.safetensors") << "fl2va";
    std::ofstream(repo / "fl2va" / "other.safetensors") << "another";
    std::ofstream(repo / "fl2va" / "manifest.json") << "{\"sampling\": 1}";
    std::ofstream(repo / "ref2va" / "manifest.json") << "{}";
    std::ofstream(repo / "LICENSE") << "license";
    std::ofstream(repo / "top.safetensors") << "weights";
    auto one = list_model_files(repo, "fl2va/adapter.safetensors");
    REQUIRE_OK(one);
    std::set<std::string> paths;
    for (const auto& f : one->files) {
      paths.insert(f.path);
    }
    CHECK(one->files.front().path == "fl2va/adapter.safetensors");
    CHECK(paths == std::set<std::string>({"fl2va/adapter.safetensors",
                                          "fl2va/manifest.json",
                                          "LICENSE"}));
  }

  const std::string key = derive_key("Studio", "orchard-lantern-42");
  std::string served_key = key;
  auto server = ShareServer::start(
      [&] { return served_key; },
      [&](const std::string& model) -> Result<ShareList> {
        if (model != "org-model") {
          return make_error(Code::NotFound, "not shared");
        }
        return list_model_files(there);
      });
  REQUIRE_OK(server);
  CopyPlan plan;
  plan.routes = {{"127.0.0.1", "", 0}};
  plan.port = (*server)->port();
  plan.key = key;
  plan.model = "org-model";
  plan.dest = root / "here" / "Org" / "Model";
  plan.streams = 3;
  const auto same = [&](const fs::path& rel) {
    std::ifstream a(there / rel, std::ios::binary);
    std::ifstream b(plan.dest / rel, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(a), {}) ==
           std::string(std::istreambuf_iterator<char>(b), {});
  };
  std::uint64_t last = 0;
  auto got = copy_model(plan, [&](std::uint64_t done, std::uint64_t total) {
    CHECK(done <= total);
    last = done;
    return true;
  });
  REQUIRE_OK(got);
  CHECK(got->files == 4);
  CHECK(got->bytes == big.size() + 8 + 7);
  CHECK(last == got->total);
  for (const char* f : {"model.safetensors", "config.json", "empty.txt",
                        "text_encoder/w.safetensors"}) {
    CHECK(same(f));
  }
  CHECK(!fs::exists(plan.dest / "left.safetensors.part"));
  CHECK(!fs::exists(plan.dest / "valtz-compressed.json"));
  CHECK(!fs::exists(plan.dest / "model.safetensors.part"));

  // Again: what is whole kept, nothing fetched; a file changed fetched.
  fs::remove(plan.dest / "config.json");
  got = copy_model(plan, {});
  REQUIRE_OK(got);
  CHECK(got->bytes == 8);
  CHECK(same("config.json"));

  // Stopped at its first report: the big file left as its `.part`.
  fs::remove(plan.dest / "model.safetensors");
  got = copy_model(plan, [](std::uint64_t, std::uint64_t) { return false; });
  REQUIRE(!got.ok());
  CHECK(got.code() == Code::Cancelled);
  CHECK(fs::exists(plan.dest / "model.safetensors.part") ||
        fs::exists(plan.dest / "model.safetensors"));

  // Another fleet's key: refused before anything is asked.
  CopyPlan stranger = plan;
  stranger.key = derive_key("Studio", "another-secret-9");
  got = copy_model(stranger, {});
  REQUIRE(!got.ok());
  CHECK(got.error().message.find("key") != std::string::npos);
  // A model it does not share.
  CopyPlan other = plan;
  other.model = "org-other";
  got = copy_model(other, {});
  REQUIRE(!got.ok());
  CHECK(got.code() == Code::NotFound);
  // Out of the fleet (no key): nothing answered.
  served_key.clear();
  got = copy_model(plan, {});
  REQUIRE(!got.ok());
  served_key = key;

  // A job's own weights PUSHED to a member (the sender connects: a member
  // serving detached may not), from wherever they are, into the bundle's
  // folder there; again, nothing sent; no room there, Busy.
  const fs::path bundles = root / "bundles";
  bool roomy = true;
  auto member = ShareServer::start(
      [&] { return key; },
      [](const std::string&) -> Result<ShareList> {
        return make_error(Code::NotFound, "none");
      },
      [&](const std::string& bundle,
          std::uint64_t) -> Result<fs::path> {
        if (!roomy) {
          return make_error(Code::Busy, "no room");
        }
        return bundles / bundle;
      });
  REQUIRE_OK(member);
  CopyPlan push;
  push.routes = {{"127.0.0.1", "", 0}};
  push.port = (*member)->port();
  push.key = key;
  push.model = "0123abcd";
  push.streams = 2;
  const std::vector<ShareFile> weights = {
      {"lora_file/style.safetensors", big.size(),
       there / "model.safetensors"},
      {"vae_file/config.json", 8, there / "config.json"}};
  got = push_files(push, weights, {});
  REQUIRE_OK(got);
  CHECK(got->bytes == big.size() + 8);
  CHECK(fs::exists(bundles / "0123abcd" / ".valtz-whole"));
  CHECK(fs::file_size(bundles / "0123abcd" / "lora_file" /
                      "style.safetensors") == big.size());
  got = push_files(push, weights, {});
  REQUIRE_OK(got);
  CHECK(got->bytes == 0);
  roomy = false;
  push.model = "4567ef";
  got = push_files(push, weights, {});
  REQUIRE(!got.ok());
  CHECK(got.code() == Code::Busy);
  // Not a bundle's name: refused.
  push.model = "../x";
  CHECK(!push_files(push, weights, {}).ok());
  fs::remove_all(root);
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

  Json
  fleet_load() override
  {
    std::lock_guard lk(load_mu);
    return load;
  }

  void
  set_load(double gpu, double ane)
  {
    std::lock_guard lk(load_mu);
    load = {{"gpu_tflops", gpu}, {"ane_tflops", ane}};
  }

  std::string       served_from;
  std::atomic<bool> received = false;
  std::mutex        load_mu;
  Json              load = Json::object();
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
  // Heard over the link it has: not taking jobs (the connections are
  // remade only for a new name, fleet, key or discoverability).
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

// The FLEET'S ARITHMETIC (the status bar's Fleet meter): each member
// tells its own TFLOP/s as they move, and each adds up its own and the
// others' -- with the highest the sum reached kept when it falls.
TEST(fleet, the_members_arithmetic_is_added_up)
{
  if (!std::getenv("VALTZ_TEST_FLEET")) {
    SKIP("set VALTZ_TEST_FLEET=1 (two members on this Mac's loopback)");
  }
  const fs::path d = temp_dir("load");
  const std::string key = derive_key("Load fleet", "a-long-test-secret");
  auto made = [&](const char* name, bool discoverable) {
    Config c;
    c.member_id = Uuid::v7().str();
    c.member_name = name;
    c.fleet = "Load fleet";
    c.key = key;
    c.discoverable = discoverable;
    (void)save_config(d / (std::string(name) + ".json"), c);
  };
  made("studio", true);
  made("air", false);
  FakeHost hs, ha;
  hs.set_load(12.5, 1.5);
  ha.set_load(3, 0);
  auto studio = make_fleet(d / "studio.json", d / "studio-files", hs);
  auto air = make_fleet(d / "air.json", d / "air-files", ha);
  int port = 0;
  REQUIRE(eventually([&] {
    port = jget(studio->status(), "port", 0);
    return port > 0;
  }, 10));
  // Alone: no member connected, its own figures only.
  REQUIRE(eventually([&] {
    return jget(studio->throughput(), "gpu_tflops", 0.0) == 12.5;
  }, 5));
  CHECK(jget(studio->throughput(), "connected", -1) == 0);
  air->connect("127.0.0.1", port);
  auto sums = [](const Json& t, double gpu, double ane) {
    return jget(t, "connected", 0) == 1 &&
           std::abs(jget(t, "gpu_tflops", 0.0) - gpu) < 1e-6 &&
           std::abs(jget(t, "ane_tflops", 0.0) - ane) < 1e-6;
  };
  REQUIRE(eventually([&] { return sums(air->throughput(), 15.5, 1.5); },
                     10));
  REQUIRE(eventually([&] { return sums(studio->throughput(), 15.5, 1.5); },
                     10));
  const Json t = air->throughput();
  CHECK(std::abs(jget(t, "peak_tflops", 0.0) - 17) < 1e-6);
  const Json ms = jget(t, "members", Json::array());
  REQUIRE(ms.size() == 2);
  CHECK(jget(ms[0], "self", false));
  CHECK(jget<std::string>(ms[0], "name", "") == "air");
  CHECK(jget<std::string>(ms[1], "name", "") == "studio");
  CHECK(jget(ms[1], "gpu_tflops", 0.0) == 12.5);

  // The studio idles: told at once (it moved), the peak kept.
  hs.set_load(0, 0);
  REQUIRE(eventually([&] { return sums(air->throughput(), 3, 0); }, 5));
  CHECK(std::abs(jget(air->throughput(), "peak_tflops", 0.0) - 17) < 1e-6);
  air->stop();
  studio->stop();
  fs::remove_all(d);
}
