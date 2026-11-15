#ifndef VALTZ_VPIPE_PLUGIN_TP_RDMA_H
#define VALTZ_VPIPE_PLUGIN_TP_RDMA_H

// valtz-tp's partial sums over RDMA on Thunderbolt (macOS 26.2 and later,
// `rdma_ctl enable` from recoveryOS on both Macs; DESIGN §11c). Each link
// a device -- `rdma_en1`, a Thunderbolt port -- its peer the device on
// the far end of the same cable.
//
// WHAT APPLE'S VERBS CARRY (measured 2026-11-12, macOS 27.2): unreliable
// connected queue pairs alone (no RC), and an RDMA write lands in a
// posted RECEIVE's buffer, as a send does -- its address and key unread,
// its arrival a receive's completion, no immediate data -- so data moves
// two-sided: the receiver posts receives straight into the destination
// (no copy) and the sender sends. UC drops a send with no receive posted,
// so a sender sends a piece only on the receiver's CREDIT for it, on a
// queue pair of its own. A queue pair holds about 4 MB of receives posted
// at once (4,096,000 bytes: past it, ENOMEM) -- and of sends -- a memory
// region 16,384,000 bytes, a message 16 MiB: pieces of 1 MiB, three in
// flight each way.
//
// At setup each link says HELLO on its credits' queue pair, its place in
// the list: a cable down, or two crossed, is named then.
//
// Nothing is retried: a lost piece leaves its receive unfilled -- the
// exchange waits, the next piece's length checked against the credit --
// and the run fails at the stall. Thunderbolt's own link does not lose
// them in practice.
//
// MEMORY handed to an exchange is registered with the device on first
// sight and kept until it is FORGOTTEN (forget) or the path goes: the host
// hands the same few exchange buffers, each at its address until it calls
// TpChannel::forget on it, before freeing or replacing it
// (tensor-parallel.h). A registration kept past that would have the
// device read and write the memory that was freed, not what replaced it
// at the same address.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace valtz::plugin {

// Whether this Mac's verbs (librdma, weakly linked) are there.
bool rdma_available();

// The RDMA devices whose port is up, by name.
std::vector<std::string> rdma_active_devices();

class RdmaPath {
public:
  // A queue pair pair on each device -- the ranks' devices paired in the
  // order given -- set up over `fd`, a connected, proved TCP link the
  // peer sets up its own over. `gid_index`: the GID the routes use (1,
  // the port's link-local). Null, with `err`, when it cannot be.
  // `weights`, one a device (its cable's speed), share every partial
  // among them -- both ranks given the same, so each stripe is received
  // where it is sent; empty or another count: shared evenly.
  static std::unique_ptr<RdmaPath> open(
      const std::vector<std::string>& devices, int fd, int gid_index,
      int wait_ms, std::string* err, std::vector<int> weights = {});
  ~RdmaPath();
  RdmaPath(const RdmaPath&) = delete;
  RdmaPath& operator=(const RdmaPath&) = delete;

  // TpChannel::exchange's: `bytes` of `partial` striped across the links,
  // the peer's into `others`. `stopped`, polled while it spins: the run
  // stopped, the exchange given up.
  bool exchange(const void* partial, void* others, std::size_t bytes,
                std::uint64_t seq, const std::function<bool()>& stopped,
                std::string* err);

  // [base, base + bytes) is about to be freed: every registration of
  // memory in it let go. Never during an exchange (TpChannel's contract);
  // after one that succeeded, nothing of it is still posted.
  void forget(const void* base, std::size_t bytes);

  struct Impl;

private:
  explicit RdmaPath(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> _impl;
};

}

#endif
