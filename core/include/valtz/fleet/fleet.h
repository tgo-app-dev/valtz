#ifndef VALTZ_FLEET_FLEET_H
#define VALTZ_FLEET_FLEET_H

// THE FLEET (DESIGN §11): Valtz on several Macs of one local network,
// working as one. A Mac is a MEMBER of a fleet by its NAME and a SECRET
// the members share; it may be DISCOVERABLE (advertised over Bonjour, so
// the others reach it and newcomers see the fleet to join), and it may
// take FLEET JOBS -- always, asking each time, or never; at all hours or
// on a SCHEDULE. Each member tells the others what it is: what it can
// run (the engine's ops, the models installed, Auto's picks), whether it
// is busy, whether it takes jobs now.
//
// A job a member cannot run -- no model, no capability -- or cannot run
// NOW, being busy, goes to a member that can and is idle: a REMOTE JOB.
// What crosses is what an engine job is (engine.h: jobs are data): its
// op, its catalog model (none for a chat: the member's own assistant),
// its parameters by value, and its inputs BY CONTENT -- the member asks
// only for the files it does not hold, by hash. It resolves the models
// on its own disk and runs the job on its own engine; its events come
// back (progress, streamed text -- no previews), then its output files.
//
// Trust is the secret: every connection is TLS with a PRE-SHARED KEY
// derived from the fleet's name and secret (derive_key), so a Mac without
// the secret fails the handshake, and nothing else of the fleet -- its
// members' state, a job -- crosses but over TLS. The secret itself is
// kept nowhere: the configuration holds the key made from it.
//
// Nothing reaches the network until this Mac is in a fleet, or looks for
// one to join (browse): macOS asks for the local network once.

#include "valtz/base/id.h"
#include "valtz/base/json.h"
#include "valtz/base/result.h"
#include "valtz/engine/engine.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace valtz::fleet {

// The protocol's version: a member of another refuses the hello.
inline constexpr int kProtocol = 1;
// The Bonjour service a discoverable member advertises.
inline constexpr const char* kService = "_valtz-fleet._tcp";

// When a member takes fleet jobs: on `days` (bit 0 Monday .. bit 6
// Sunday), from minute `from` to minute `to` of the day, local time --
// `to` before `from` runs past midnight (the morning belonging to the
// day before); equal, all day.
struct Schedule {
  bool on = false;
  int  days = 0x7f;
  int  from = 9 * 60;
  int  to = 18 * 60;
  bool open_at(std::time_t) const;
};

struct Config {
  std::string member_id;    // made once, kept
  std::string member_name;  // what the others call this Mac
  std::string fleet;        // "" in none
  std::string key;          // hex, derive_key(fleet, secret)
  bool        discoverable = false;
  std::string accept = "always";  // always | ask | never
  Schedule    schedule;

  bool joined() const { return !fleet.empty() && !key.empty(); }
  // Takes fleet jobs at `t`: its policy, its schedule.
  bool accepting_at(std::time_t t) const;
};

// The pre-shared key: HMAC-SHA256 of the fleet's name, keyed by its
// secret, as hex. The same name and secret make the same key on any Mac.
std::string derive_key(std::string_view fleet, std::string_view secret);

// Kept as JSON beside the rest of Valtz's settings (or where
// $VALTZ_FLEET_CONFIG says), its key with it, readable by its owner
// alone; none yet: a new member id, this Mac's name.
Result<Config> load_config(const std::filesystem::path&);
Status save_config(const std::filesystem::path&, const Config&);
// Without its key ("secret_set" says whether there is one).
Json to_json(const Config&);
Json to_json(const Schedule&);
Schedule schedule_from_json(const Json&);
// This Mac's name, as Sharing shows it ("Studio", "Wei's MacBook Air").
std::string default_member_name();

// FRAMING. A message is a CBOR map, sent as its length (4 bytes, big
// endian) and its bytes; a file crosses as messages of binary chunks.
std::vector<std::uint8_t> frame(const Json& message);
class Deframer {
public:
  void feed(std::span<const std::uint8_t>);
  // The next whole message, if one has come; an error for a frame past
  // `kMaxFrame` or one that does not decode.
  std::optional<Result<Json>> next();
  static constexpr std::size_t kMaxFrame = 64u << 20;

private:
  std::vector<std::uint8_t> _buf;
  std::size_t               _at = 0;
};

// A job offered to a member: what it makes and from what.
struct Offer {
  JobId                         id;
  std::string                   op;
  std::string                   model;  // catalog id; "" a chat
  std::string                   title;
  Json                          params = Json::object();
  // Paths are the sender's (or, served, where they were received); the
  // hash and size are what crosses.
  std::vector<engine::JobInput> inputs;
};
Json to_json(const Offer&);
Result<Offer> offer_from_json(const Json&);

// What serving needs of the controller it runs in.
class Host {
public:
  virtual ~Host() = default;
  // This member as the others see it: {"ops", "installed", "runs",
  // "features", "assistant", "busy", "machine", "engine", "version"}.
  virtual Json fleet_self() = 0;
  // Run a job a member sent, its inputs at hand, its output files into
  // `out_dir`; its events onto `sink`, a terminal one last.
  virtual Status fleet_serve(const Offer&, const std::string& from,
                             const std::filesystem::path& out_dir,
                             engine::JobSink sink) = 0;
  virtual void fleet_cancel(JobId) = 0;
  // Something to tell the app: "fleet.changed" (members, state),
  // "fleet.ask" (a job waiting for an answer: answer()).
  virtual void fleet_note(std::string kind, Json data) = 0;
};

class Fleet {
public:
  virtual ~Fleet() = default;
  virtual Config config() const = 0;
  // Saved, and the network made to follow: a new fleet, a new secret,
  // discoverable or not.
  virtual Status set_config(const Config&) = 0;
  // {"config", "port", "browsing", "members": [{"id", "name", "state",
  // "self"}], "fleets": [{"fleet", "members"}], "serving", "asking"}.
  virtual Json status() const = 0;
  // The members connected now, each as it says it is ("self", "id",
  // "name").
  virtual std::vector<Json> members() const = 0;
  // Look for fleets on the network (the Fleet page open), or stop: a
  // member looks for its own anyway.
  virtual void browse(bool on) = 0;
  // A remote job: `offer` to `member`; its events onto `sink` -- an
  // output written into `out_dir` arrives as an Output event, and a
  // refusal is a Failed event with data {"fleet": "declined"}.
  virtual Status submit(const std::string& member, const Offer&,
                        const std::filesystem::path& out_dir,
                        engine::JobSink sink) = 0;
  virtual void cancel(JobId) = 0;
  // The answer to a "fleet.ask".
  virtual void answer(JobId, bool accept) = 0;
  // This member's own state changed (busy, models): told the others.
  virtual void self_changed() = 0;
  // A member at an address, without Bonjour (tests; a network that does
  // not carry it).
  virtual void connect(const std::string& host, int port) = 0;
  virtual void stop() = 0;
};

std::unique_ptr<Fleet> make_fleet(std::filesystem::path config_path,
                                  std::filesystem::path work_dir, Host&);

}

#endif
