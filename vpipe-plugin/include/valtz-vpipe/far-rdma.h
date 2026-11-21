#ifndef VALTZ_VPIPE_FAR_RDMA_H
#define VALTZ_VPIPE_FAR_RDMA_H

// FAR MEMORY OVER RDMA (DESIGN §11e): the lent memory reached over each
// Thunderbolt cable's RDMA device in place of TCP -- a read a few
// microseconds of setup, not a socket's. Both ends compile this file
// (vpipe-plugin/src/far-rdma.cc): the plugin's valtz-far the USER, the
// core's FarLender the LENDER. Neither includes vpipe here.
//
// What Apple's verbs carry is valtz-tp's (tp-rdma.h): unreliable
// connected queue pairs alone, an RDMA write landing as a send -- so
// every byte moves TWO-SIDED, a receive posted for it first -- a queue
// pair holding about 4 MB of receives (and of sends) at once, a memory
// region 16,384,000 bytes, a page registered three times at most. A link
// is two queue pairs on one device: CONTROL (32-byte messages) and DATA
// (pieces of 1 MiB at most, three in flight each way).
//
// A device registers 100 memory regions of 16,384,000 bytes at most (M5,
// macOS 27.2): 1.6 GB, neither the lent memory nor an expert cache. So
// each link STAGES its pieces -- three receive slots and three send slots
// of 1 MiB, registered once -- and copies them (~50 us a MiB, while the
// next two are on the cable).
//
// A READ: the user posts a receive into its next slot, then asks
// (control) for the piece; the lender copies it from the lent memory into
// a send slot and sends it -- in the order asked, so each lands in the
// receive posted for it -- and the user copies it out. A WRITE: the
// lender keeps its three receive slots posted -- the user's CREDITS --
// and the user sends a piece only on one, its header on control beside
// it -- a whole slot, the header saying how much of it counts: a message
// must fill the receive it lands in ("local length error" otherwise), and
// the lender posts its receives before it knows a write's length; a read
// posts one of the piece's length -- the lender copies it into place,
// posts the slot again and ACKS it, the credit back.
//
// SETUP rides a proved connection of far-wire.h: the user asks kRdma
// with its links -- for each, the lender's device on that cable and its
// own queue pairs -- the lender answers with its own (or why not), both
// bring the pairs up, then a HELLO each way on each link names a cable
// down or two crossed. That connection stays open while the links live:
// closed, the lender lets them go.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace valtz::far_rdma {

// Whether this Mac's verbs (librdma, weakly linked) are there.
bool available();

// A piece's most: what one message carries.
inline constexpr std::size_t kPiece = std::size_t(1) << 20;

class UserLink;

// The user's end of every link, set up over `fd` -- a proved far-wire
// connection to the lender -- each local device paired with the lender's
// `peer_devices` at the same place. Empty with `err` when it cannot be.
std::vector<std::unique_ptr<UserLink>> connect(
    int fd, const std::vector<std::string>& devices,
    const std::vector<std::string>& peer_devices, std::string* err);

// One link of the user's, driven by one thread at a time (forget aside).
class UserLink {
public:
  ~UserLink();
  UserLink(const UserLink&) = delete;
  UserLink& operator=(const UserLink&) = delete;

  const std::string& name() const;

  // Room for another read / write now.
  bool can_read() const;
  bool can_write() const;

  // A piece of at most kPiece bytes at the lent `offset`, read into
  // `dst` / written from `src`; `tag` comes back from poll() when it is
  // done. An error string when it could not be posted.
  std::string read(std::uint64_t offset, void* dst, std::size_t n,
                   std::uint64_t tag);
  std::string write(std::uint64_t offset, const void* src, std::size_t n,
                    std::uint64_t tag);

  // What has completed since, appended to `done`; whether anything did.
  // `err` set when the link failed (everything in flight lost).
  bool poll(std::vector<std::uint64_t>* done, std::string* err);

  // Nothing posted and unanswered.
  bool idle() const;

  // A CALL (kCompute; PROTOTYPE): `req` (at most 32 KB) sent on the link's
  // call queue pair, its answer -- exactly `reply_n` bytes -- into `reply`,
  // waited for here by polling (any thread; one call at a time a link). An
  // error string when it could not be (the link then not used for calls).
  std::string call(const void* req, std::size_t n, void* reply,
                   std::size_t reply_n, int wait_ms);

  struct Impl;

private:
  friend std::vector<std::unique_ptr<UserLink>> connect(
      int, const std::vector<std::string>&, const std::vector<std::string>&,
      std::string*);
  explicit UserLink(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> _impl;
};

// The lender's end of a session's links: set up over `fd` after the
// user's kRdma request (`count` links), each served by a thread of its
// own until this goes -- before `mem` is let go. `read` / `written` count
// the bytes handed back / stored. Null with `err` (the user told why too)
// when it cannot be.
class Lender {
public:
  // A call's request (`n` bytes as they landed) answered into `answer`
  // (its length the caller's; empty: no answer -- the caller's wait then
  // fails). Run on link 0's call thread.
  using Call = std::function<void(const std::uint8_t* req, std::size_t n,
                                  std::vector<std::uint8_t>* answer)>;

  static std::unique_ptr<Lender> serve(int fd, std::uint32_t count,
                                       std::uint8_t* mem,
                                       std::uint64_t bytes,
                                       std::atomic<std::uint64_t>* read,
                                       std::atomic<std::uint64_t>* written,
                                       Call on_call, std::string* err);
  ~Lender();
  Lender(const Lender&) = delete;
  Lender& operator=(const Lender&) = delete;

  struct Impl;

private:
  explicit Lender(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> _impl;
};

// THE LENDER'S FILES (far-wire.h's kOpenFile; PROTOTYPE): its copies of a
// model's files, under `root` ("" none), read for the user off this Mac's
// drive -- uncached -- when a read names one (far_wire::file_offset).
// Process-wide: one lender a process.
void lend_files(const std::string& root);
// `name` (a path under the root, no "..") opened if it is `size` bytes:
// its id, or -1 with `err`.
std::int64_t open_lent_file(const std::string& name, std::uint64_t size,
                            std::string* err);

}

#endif
