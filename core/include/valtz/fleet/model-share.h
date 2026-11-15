#ifndef VALTZ_FLEET_MODEL_SHARE_H
#define VALTZ_FLEET_MODEL_SHARE_H

// A MODEL COPIED FROM A MEMBER (DESIGN §11d). A member of a fleet that has
// a model's files may hand them to another, which keeps them as its own
// download -- in the folder a download would have put them, so its
// Capabilities row is the downloaded model's, paused and resumed and
// cancelled as a download is.
//
// WHY NOT OVER THE FLEET'S CONNECTION: that is TLS, a core a direction
// at these rates and one stream; a model is tens or hundreds of GB, and
// two Macs on a Thunderbolt cable carry 8-9 GB/s each way. So each member
// in a fleet runs a FILE SERVER of its own -- plain TCP, its port told
// the others with the rest of what it is -- and the copy opens several
// streams on each Thunderbolt route to it, each bound to its port, taking
// 64 MB pieces from one queue: a faster cable takes more of them, and no
// stream waits on a slower one. Without a cable, its network address.
//
// TRUST is the fleet's key, as for everything else: each connection
// proves it both ways (HMAC-SHA256 over two fresh nonces) before anything
// is asked, so a Mac without the secret is answered nothing. The bytes
// then cross UNENCRYPTED -- a model's weights, public downloads, over a
// cable between the two Macs or the local network; a member serves only
// the files of a model it shares (installed, not withheld), by the path
// it listed, never a path asked for.
//
// A copy writes each file as `<name>.part`, its whole size made at once,
// renamed when every piece of it has come: stopped part way, the folder
// reads as a partial download, and a copy again keeps the files already
// whole (a file part way is fetched again whole).

#include "valtz/base/json.h"
#include "valtz/base/result.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace valtz::fleet {

// The protocol's version: the other end refuses another.
inline constexpr int kShareProtocol = 1;

// One file of a model as a member lists it: its path under the model's
// folder ('/'-separated, relative) and its size.
struct ShareFile {
  std::string   path;
  std::uint64_t size = 0;
  // Where it is read from, when not `root / path` (a job's weights, from
  // wherever Custom names them).
  std::filesystem::path from;
};

// What a member serves of a model: where its files are, and which.
struct ShareList {
  std::filesystem::path  root;
  std::vector<ShareFile> files;
};

// A model's files under `root` as a member lists them: `file` where the
// model is one file of its folder -- with the small files beside it and
// at the repo's top that are no weights (an adapter's sampling manifest,
// LICENSE, NOTICE) -- else every file under it; never a download's
// leftovers (".part") or a compressed copy's mark.
Result<ShareList> list_model_files(const std::filesystem::path& root,
                                   const std::string& file = {});

// This Mac's IPv4 addresses, up, not the loopback -- link-local last:
// where a member without a Thunderbolt cable to it reaches its share.
Json share_addresses();

// The member's side: listening on every address at a port of its own,
// serving whoever proves the key `key()` gives at that moment ("" none:
// every connection refused), what `resolve` lists for a model it is asked.
class ShareServer {
public:
  using Key = std::function<std::string()>;
  using Resolve = std::function<Result<ShareList>(const std::string&)>;
  // A BUNDLE pushed here (push_files): the folder it goes into, given its
  // name and bytes -- or why not (Busy: no room for it). None: nothing
  // taken.
  using Receive = std::function<Result<std::filesystem::path>(
      const std::string& bundle, std::uint64_t bytes)>;
  static Result<std::unique_ptr<ShareServer>> start(Key key,
                                                    Resolve resolve,
                                                    Receive receive = {});
  virtual ~ShareServer() = default;
  virtual int port() const = 0;
  // Bytes served since it started (logs, tests).
  virtual std::uint64_t served() const = 0;
};

// A way to a member: its address there and the interface the streams are
// bound to ("" the system's route), the cable's speed (Gb/s, 0 unknown).
struct ShareRoute {
  std::string address;
  std::string interface;
  int         gbps = 0;
};

// A copy: the member's routes -- each Thunderbolt cable's, fastest first;
// else its network addresses, the first that answers used -- its port,
// the key (hex), the model, and the folder its files go into.
struct CopyPlan {
  std::vector<ShareRoute> routes;
  int                     port = 0;
  std::string             key;
  std::string             model;
  std::filesystem::path   dest;
  // Streams a Thunderbolt route (and the one network route) opens.
  int streams = 4;
};
Json to_json(const CopyPlan&);
CopyPlan copy_plan_from_json(const Json&);

// What a copy did: its bytes (those already whole not counted), the
// files, the seconds, how many streams on which routes.
struct CopyResult {
  std::uint64_t bytes = 0;
  std::uint64_t total = 0;
  std::size_t   files = 0;
  double        seconds = 0;
  std::string   routes;  // "bridge0 x4, bridge1 x4"
};

// Files PUSHED to a member's share as `bundle` (`plan.model` its name,
// `plan.dest` unused): what it lacks of them -- those whole there kept --
// over each of its routes as copy_model reads. `files`' paths are their
// places in the bundle, `from` where each is read here. Busy where the
// member has no room for them.
Result<CopyResult> push_files(
    const CopyPlan& plan, const std::vector<ShareFile>& files,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress);

// The model's files copied into `plan.dest`. `progress(done, total)` is
// called as pieces land (a few times a second); false stops the copy:
// Cancelled, the files part way left as `.part`.
Result<CopyResult> copy_model(
    const CopyPlan& plan,
    const std::function<bool(std::uint64_t, std::uint64_t)>& progress);

}

#endif
