// valtz-tp: the transport a DiT split across two Macs exchanges its
// partial sums through (vpipe's generative-models/tensor-parallel.h;
// DESIGN §11c). vpipe hands this rank's partial over at each of a
// block's two reduction points and waits for the peer's; this carries
// both, over TCP, ONE CONNECTION PER LINK -- a Thunderbolt cable each,
// its own addresses -- every exchange striped across them, sent and
// received at once.
//
// The bytes go from the partial's shared Metal buffer to the socket and
// from the socket into vpipe's buffer for the peer's: no copy here.
//
// Trust: each connection proves both ends hold the run's KEY (Valtz's
// fleet key, or one given) before a byte of a partial crosses -- an HMAC
// of each side's nonce -- and the partials then cross in the clear: a
// cable between two Macs, at the link's speed (TLS would cost a core per
// direction at these rates).
//
// params (generate-video's tensor_parallel.params, passed through):
//   role         "listen" | "connect"
//   links        ["host:port", ...]: where each link listens (its own
//                address on that cable) or the peer's to connect to
//   interfaces   ["en1", ...]: connect only -- each link's socket bound
//                to that interface, so a link is the cable named
//   key          hex: the shared key (empty: no check -- loopback tests)
//   wait_ms      how long to wait for the peer to appear and for each
//                exchange (default 30 min: the peer may still be loading)
//   rdma         ["rdma_en1", ...] or "auto": the partials over RDMA on
//                these devices -- a Thunderbolt port each, paired with the
//                peer's in the order given -- the peer asked for RDMA too,
//                as many devices; "auto" the ports that are up, where both
//                ranks have as many (else TCP, said on stderr). The TCP
//                links then prove the key and set the queue pairs up, and
//                carry nothing else (tp-rdma.h).
//   rdma_weights [80, 20]: each RDMA device's share of a partial, its
//                cable's speed in Gb/s -- the same on both ranks -- so a
//                slower cable beside a faster one carries less (an even
//                share waits on it); absent: shared evenly
//   gid_index    the GID RDMA routes by (default 1, the link-local)

#include "stages.h"
#include "tp-rdma.h"
#include "valtz-vpipe/exchange.h"

#include "generative-models/tensor-parallel.h"

#include <CommonCrypto/CommonHMAC.h>

#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace valtz::plugin {

namespace {

using vpipe::FlexData;
using vpipe::genai::TpChannel;
using vpipe::genai::TpShare;
using vpipe::genai::TpTransport;

// What every channel has carried, for the status bar's Fleet popover
// (exchange.h's kTpTrafficSymbol).
std::atomic<std::uint64_t> g_sent{0};
std::atomic<std::uint64_t> g_received{0};
std::atomic<std::uint64_t> g_channels{0};

constexpr char kMagic[4] = {'V', 'T', 'P', '1'};
constexpr std::size_t kNonce = 32;

bool
send_all(int fd, const void* p, std::size_t n)
{
  const auto* c = static_cast<const std::uint8_t*>(p);
  while (n > 0) {
    const ssize_t k = ::send(fd, c, n, 0);
    if (k <= 0) { return false; }
    c += k;
    n -= (std::size_t)k;
  }
  return true;
}

bool
recv_all(int fd, void* p, std::size_t n)
{
  auto* c = static_cast<std::uint8_t*>(p);
  while (n > 0) {
    const ssize_t k = ::recv(fd, c, n, 0);
    if (k <= 0) { return false; }
    c += k;
    n -= (std::size_t)k;
  }
  return true;
}

// Sockets tuned for a link of several GB/s: no Nagle, large buffers, and
// a receive / send timeout so a peer that vanished fails the run.
void
tune(int fd, int wait_ms)
{
  int one = 1, buf = 8 << 20;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  timeval tv{wait_ms / 1000, (wait_ms % 1000) * 1000};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

bool
split_host_port(const std::string& hp, std::string* host, std::string* port)
{
  const auto colon = hp.rfind(':');
  if (colon == std::string::npos || colon == 0 || colon + 1 >= hp.size()) {
    return false;
  }
  *host = hp.substr(0, colon);
  *port = hp.substr(colon + 1);
  return true;
}

std::vector<std::uint8_t>
from_hex(const std::string& h)
{
  std::vector<std::uint8_t> out;
  auto nib = [](char c) -> int {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
  };
  for (std::size_t i = 0; i + 1 < h.size(); i += 2) {
    const int a = nib(h[i]), b = nib(h[i + 1]);
    if (a < 0 || b < 0) { return {}; }
    out.push_back((std::uint8_t)(a * 16 + b));
  }
  return out;
}

// HMAC-SHA256 of `who` (which side proves itself), both nonces and the
// link's index, keyed by the run's key.
std::array<std::uint8_t, CC_SHA256_DIGEST_LENGTH>
proof(const std::vector<std::uint8_t>& key, char who, const std::uint8_t* a,
      const std::uint8_t* b, std::uint32_t link)
{
  std::vector<std::uint8_t> msg = {'v', 'a', 'l', 't', 'z', '-', 't', 'p',
                                   '/', '1', (std::uint8_t)who};
  msg.insert(msg.end(), a, a + kNonce);
  msg.insert(msg.end(), b, b + kNonce);
  for (int i = 0; i < 4; ++i) {
    msg.push_back((std::uint8_t)(link >> (8 * i)));
  }
  std::array<std::uint8_t, CC_SHA256_DIGEST_LENGTH> out{};
  CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), msg.data(), msg.size(),
         out.data());
  return out;
}

void
nonce(std::uint8_t* out)
{
  arc4random_buf(out, kNonce);
}

// One rank's channel to its one peer.
class Channel final : public TpChannel {
public:
  struct Link {
    std::string address;     // host:port
    std::string interface;   // connect: the interface to bind
    int fd = -1;
  };

  Channel(bool listen, std::vector<Link> links, std::vector<std::uint8_t> key,
          int rank, int wait_ms, std::vector<std::string> rdma,
          bool rdma_auto, int gid_index, std::vector<int> rdma_weights)
      : _listen(listen), _links(std::move(links)), _key(std::move(key)),
        _rank(rank), _wait_ms(wait_ms), _rdma_devices(std::move(rdma)),
        _rdma_auto(rdma_auto), _gid_index(gid_index),
        _rdma_weights(std::move(rdma_weights))
  {
    g_channels.fetch_add(1);
    // The peer may not be there yet -- still encoding its prompt, still
    // loading its share -- so the links are made BESIDE the load, and
    // the first exchange waits for them.
    _setup = std::thread([this] { setup_(); });
  }

  ~Channel() override
  {
    // A run torn down before its links were made (stopped while its
    // prompt was read, its peer never come): the setup let go at once,
    // not at its deadline.
    _closing.store(true);
    if (_setup.joinable()) { _setup.join(); }
    {
      std::lock_guard<std::mutex> lk(_mu);
      _quit = true;
      ++_gen;
    }
    _cv.notify_all();
    _rdma.reset();
    for (Link& l : _links) {
      if (l.fd >= 0) { ::shutdown(l.fd, SHUT_RDWR); }
    }
    for (std::thread& t : _workers) {
      if (t.joinable()) { t.join(); }
    }
    for (Link& l : _links) {
      if (l.fd >= 0) { ::close(l.fd); }
    }
    for (int fd : _listening) { ::close(fd); }
    g_channels.fetch_sub(1);
  }

  // An exchange buffer about to be freed: RDMA's registrations of it let
  // go (TCP keeps nothing of what it is handed). Before the links are
  // made nothing was exchanged, so nothing was registered -- and the
  // setup, which makes the RDMA path, is still the setup thread's.
  void
  forget(const void* base, std::size_t bytes) override
  {
    if (_setup.joinable()) { return; }
    if (_rdma) { _rdma->forget(base, bytes); }
  }

  // The run's stop, polled wherever this waits: for the peer to appear,
  // for its partial.
  void
  set_stop(std::function<bool()> stop) override
  {
    std::lock_guard<std::mutex> lk(_stop_mu);
    _stop = std::move(stop);
  }

  // `dtype` unread: with two ranks the sum of the others' partials is
  // the peer's own, carried as bytes (open() refuses more ranks).
  bool
  exchange(const void* partial, void* others, std::size_t bytes,
           vpipe::genai::TpDType dtype, std::uint64_t seq,
           std::string* err) override
  {
    (void)dtype;
    if (_setup.joinable()) { _setup.join(); }
    if (!_setup_err.empty()) {
      if (err != nullptr) { *err = _setup_err; }
      return false;
    }
    if (_rdma) {
      if (!_rdma->exchange(partial, others, bytes, seq,
                           [this] { return stopped_(); }, err)) {
        return false;
      }
      counted_(bytes);
      return true;
    }
    std::unique_lock<std::mutex> lk(_mu);
    _out = static_cast<const std::uint8_t*>(partial);
    _in = static_cast<std::uint8_t*>(others);
    _bytes = bytes;
    _seq = seq;
    _done = 0;
    _fail.clear();
    ++_gen;
    _cv.notify_all();
    // Stopped while the peer's partial is awaited: the links cut, so the
    // workers' sends and receives end and the exchange fails.
    const auto all = [&] { return _done == 2 * (int)_links.size(); };
    bool cut = false;
    while (!_cv_done.wait_for(lk, std::chrono::milliseconds(200), all)) {
      if (!cut && stopped_()) {
        cut = true;
        _fail = "stopped";
        for (Link& l : _links) {
          if (l.fd >= 0) { ::shutdown(l.fd, SHUT_RDWR); }
        }
      }
    }
    if (!_fail.empty()) {
      if (err != nullptr) { *err = _fail; }
      return false;
    }
    counted_(bytes);
    return true;
  }

private:
  // Two ranks: the partial went out whole and the peer's came in whole.
  static void
  counted_(std::size_t bytes)
  {
    g_sent.fetch_add(bytes, std::memory_order_relaxed);
    g_received.fetch_add(bytes, std::memory_order_relaxed);
  }

  // Every link made and proved, then a sender and a receiver a link.
  void
  setup_()
  {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(_wait_ms);
    for (std::size_t i = 0; i < _links.size(); ++i) {
      std::fprintf(stderr, "valtz-tp: rank %d: link %zu: %s %s%s%s\n",
                   _rank, i + 1, _listen ? "listening on" : "connecting to",
                   _links[i].address.c_str(),
                   _links[i].interface.empty() ? "" : " by ",
                   _links[i].interface.c_str());
      std::string e = _listen ? accept_(i, deadline) : connect_(i, deadline);
      if (e.empty()) { e = prove_(i); }
      if (!e.empty()) {
        _setup_err = "link " + std::to_string(i + 1) + " (" +
                     _links[i].address + "): " + e;
        return;
      }
      tune(_links[i].fd, _wait_ms);
    }
    if (std::string e = choose_path_(); !e.empty()) {
      _setup_err = "RDMA: " + e;
      return;
    }
    if (!_rdma_devices.empty()) {
      // Over RDMA the first link sets the queue pairs up; the partials
      // cross on the devices.
      std::string e;
      _rdma = RdmaPath::open(_rdma_devices, _links[0].fd, _gid_index,
                             _wait_ms, &e, _rdma_weights);
      if (!_rdma) { _setup_err = "RDMA: " + e; }
      if (_rdma) {
        std::string names;
        for (std::size_t i = 0; i < _rdma_devices.size(); ++i) {
          names += (names.empty() ? "" : ", ") + _rdma_devices[i];
          if (_rdma_weights.size() == _rdma_devices.size()) {
            names += " (" + std::to_string(_rdma_weights[i]) + ")";
          }
        }
        std::fprintf(stderr, "valtz-tp: rank %d's partials over RDMA on "
                     "%s\n", _rank, names.c_str());
      }
      return;
    }
    for (std::size_t i = 0; i < _links.size(); ++i) {
      _workers.emplace_back([this, i] { work_(i, true); });
      _workers.emplace_back([this, i] { work_(i, false); });
    }
  }

  std::string
  accept_(std::size_t i, std::chrono::steady_clock::time_point deadline)
  {
    std::string host, port;
    if (!split_host_port(_links[i].address, &host, &port)) {
      return "not host:port";
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 ||
        res == nullptr) {
      return "cannot resolve it";
    }
    const int ls = ::socket(res->ai_family, SOCK_STREAM, 0);
    int one = 1;
    ::setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    const bool bound = ls >= 0 &&
                       ::bind(ls, res->ai_addr, res->ai_addrlen) == 0 &&
                       ::listen(ls, 4) == 0;
    ::freeaddrinfo(res);
    if (!bound) {
      if (ls >= 0) { ::close(ls); }
      return std::string("cannot listen: ") + std::strerror(errno);
    }
    _listening.push_back(ls);
    for (;;) {
      const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            deadline - std::chrono::steady_clock::now())
                            .count();
      if (left <= 0) { return "the peer did not connect"; }
      if (_closing.load() || stopped_()) { return "stopped"; }
      pollfd p{ls, POLLIN, 0};
      if (::poll(&p, 1, (int)std::min<long long>(left, 200)) <= 0) {
        continue;
      }
      const int fd = ::accept(ls, nullptr, nullptr);
      if (fd >= 0) {
        // Its listener closed with the link made: a clip's next channel
        // (a 2K clip's refine after its draft) listens on the same port,
        // and a connection queued on this one would wait there unheard.
        ::close(ls);
        std::erase(_listening, ls);
        _links[i].fd = fd;
        return {};
      }
    }
  }

  std::string
  connect_(std::size_t i, std::chrono::steady_clock::time_point deadline)
  {
    std::string host, port;
    if (!split_host_port(_links[i].address, &host, &port)) {
      return "not host:port";
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    // Why each try failed, said the first time and every ten seconds:
    // a peer still loading refuses, a wrong cable or address does not.
    std::string why;
    auto said = std::chrono::steady_clock::time_point{};
    for (;;) {
      addrinfo* res = nullptr;
      if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0 &&
          res != nullptr) {
        const int fd = ::socket(res->ai_family, SOCK_STREAM, 0);
        if (fd >= 0 && !_links[i].interface.empty()) {
          const unsigned idx = ::if_nametoindex(_links[i].interface.c_str());
          ::setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx);
        }
        const bool ok = fd >= 0 && connect_soon_(fd, res, &why);
        ::freeaddrinfo(res);
        if (ok) {
          _links[i].fd = fd;
          return {};
        }
        if (fd >= 0) { ::close(fd); }
      } else {
        why = "cannot resolve it";
      }
      const auto now = std::chrono::steady_clock::now();
      if (now - said >= std::chrono::seconds(10)) {
        std::fprintf(stderr, "valtz-tp: rank %d: link %zu: %s%s%s: %s\n",
                     _rank, i + 1, _links[i].address.c_str(),
                     _links[i].interface.empty() ? "" : " by ",
                     _links[i].interface.c_str(), why.c_str());
        said = now;
      }
      if (now >= deadline) {
        return "the peer is not listening (" + why + ")";
      }
      if (_closing.load() || stopped_()) { return "stopped"; }
      // Refused (the peer not listening yet) comes back at once: a short
      // pause before the next try. A try that timed out has waited.
      if (why != kSynLost) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }
  }

  // A CONNECT GIVEN 100 MS, then another SYN: over a Thunderbolt bridge
  // the first SYN of a link is often lost (a link-local route the reply
  // took elsewhere, an address not learned yet), and the kernel's own
  // retransmit waits a second -- a second at the start of every clip.
  static constexpr const char* kSynLost = "no answer to the connection";

  static bool
  connect_soon_(int fd, const addrinfo* res, std::string* why)
  {
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, 100) <= 0) {
        *why = kSynLost;
        return false;
      }
      int e = 0;
      socklen_t n = sizeof e;
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n);
      errno = e;
      rc = e == 0 ? 0 : -1;
    }
    if (rc != 0) {
      *why = std::strerror(errno);
      return false;
    }
    ::fcntl(fd, F_SETFL, fl);
    why->clear();
    return true;
  }

  // Both ends prove the key: the connecting side's nonce first, the
  // listener's back with its proof, the connector's proof last.
  // WHAT EACH RANK ASKED FOR -- TCP, RDMA on devices named, RDMA where
  // it can -- swapped over the first link, and one rule both apply, so
  // the two take one path. Named devices are required: a peer not asked
  // for RDMA, or with as many devices, is an error. "auto" settles for
  // TCP where either rank has no port up, or their counts differ.
  std::string
  choose_path_()
  {
    enum : std::uint32_t { kTcp = 0, kNamed = 1, kAuto = 2 };
    if (_rdma_auto) {
      _rdma_devices = rdma_available() ? rdma_active_devices()
                                       : std::vector<std::string>();
    }
    const std::uint32_t mine[2] = {
        _rdma_auto ? kAuto : !_rdma_devices.empty() ? kNamed : kTcp,
        (std::uint32_t)_rdma_devices.size()};
    std::uint32_t theirs[2] = {0, 0};
    const int fd = _links[0].fd;
    if (::send(fd, mine, sizeof mine, 0) != (ssize_t)sizeof mine ||
        ::recv(fd, theirs, sizeof theirs, MSG_WAITALL) !=
            (ssize_t)sizeof theirs) {
      return "the link was cut choosing the path";
    }
    const bool named = mine[0] == kNamed || theirs[0] == kNamed;
    if (mine[0] == kTcp && theirs[0] == kTcp) { return {}; }
    if (named) {
      if (mine[0] == kTcp || theirs[0] == kTcp) {
        return mine[0] == kTcp ? "the peer asks for RDMA, this rank not"
                               : "the peer was not asked for RDMA";
      }
      if (mine[1] != theirs[1]) {
        return "this rank has " + std::to_string(mine[1]) +
               " RDMA links, the peer " + std::to_string(theirs[1]);
      }
      return {};
    }
    // "auto" on one side at least, nothing named.
    if (mine[0] == kAuto && theirs[0] == kAuto && mine[1] > 0 &&
        mine[1] == theirs[1]) {
      return {};
    }
    if (mine[0] == kAuto) {
      std::fprintf(stderr, "valtz-tp: rank %d's partials over TCP: %s\n",
                   _rank,
                   theirs[0] != kAuto ? "the peer was not asked for RDMA"
                   : mine[1] == 0     ? "this Mac has no RDMA port up"
                   : theirs[1] == 0   ? "the peer has no RDMA port up"
                                      : "the two have as many RDMA ports "
                                        "up");
    }
    _rdma_devices.clear();
    return {};
  }

  std::string
  prove_(std::size_t i)
  {
    const int fd = _links[i].fd;
    tune(fd, std::min(_wait_ms, 30000));
    const auto link = (std::uint32_t)i;
    std::uint8_t mine[kNonce], theirs[kNonce];
    nonce(mine);
    char magic[4];
    std::uint32_t hdr[3] = {link, (std::uint32_t)_links.size(),
                            (std::uint32_t)_rank};
    std::uint32_t peer[3];
    if (!send_all(fd, kMagic, 4) || !send_all(fd, hdr, sizeof hdr) ||
        !send_all(fd, mine, kNonce) || !recv_all(fd, magic, 4) ||
        !recv_all(fd, peer, sizeof peer) || !recv_all(fd, theirs, kNonce)) {
      return "the handshake was cut";
    }
    if (std::memcmp(magic, kMagic, 4) != 0) { return "not a valtz-tp peer"; }
    if (peer[0] != link || peer[1] != hdr[1]) {
      return "the peer counts its links otherwise";
    }
    if (peer[2] == (std::uint32_t)_rank) { return "the peer is the same rank"; }
    if (_key.empty()) { return {}; }
    // Each side proves it holds the key over both nonces, its own first.
    const char me = _listen ? 'l' : 'c', them = _listen ? 'c' : 'l';
    const auto p = proof(_key, me, mine, theirs, link);
    std::array<std::uint8_t, CC_SHA256_DIGEST_LENGTH> got{};
    if (!send_all(fd, p.data(), p.size()) ||
        !recv_all(fd, got.data(), got.size())) {
      return "the handshake was cut";
    }
    if (got != proof(_key, them, theirs, mine, link)) {
      return "the peer does not hold the key";
    }
    return {};
  }

  // A link's sender (`out`) or receiver: woken for each exchange, its
  // stripe of the partial sent -- or the peer's received -- after a
  // header that keeps the ranks in step.
  void
  work_(std::size_t i, bool out)
  {
    std::uint64_t seen = 0;
    for (;;) {
      const std::uint8_t* src;
      std::uint8_t* dst;
      std::size_t bytes;
      std::uint64_t seq;
      {
        std::unique_lock<std::mutex> lk(_mu);
        _cv.wait(lk, [&] { return _gen != seen; });
        seen = _gen;
        if (_quit) { return; }
        src = _out;
        dst = _in;
        bytes = _bytes;
        seq = _seq;
      }
      const std::size_t n = _links.size();
      const std::size_t per = (bytes + n - 1) / n;
      const std::size_t off = std::min(bytes, per * i);
      const std::size_t len = std::min(per, bytes - off);
      const int fd = _links[i].fd;
      std::string e;
      std::uint64_t hdr[2] = {seq, (std::uint64_t)len};
      if (out) {
        if (!send_all(fd, hdr, sizeof hdr) ||
            (len > 0 && !send_all(fd, src + off, len))) {
          e = "the link to the peer failed";
        }
      } else {
        std::uint64_t got[2];
        if (!recv_all(fd, got, sizeof got)) {
          e = "the peer went away";
        } else if (got[0] != seq || got[1] != len) {
          e = "the ranks are out of step (exchange " +
              std::to_string(got[0]) + " of " + std::to_string(got[1]) +
              " bytes, against " + std::to_string(seq) + " of " +
              std::to_string(len) + ")";
        } else if (len > 0 && !recv_all(fd, dst + off, len)) {
          e = "the peer went away";
        }
      }
      {
        std::lock_guard<std::mutex> lk(_mu);
        if (!e.empty() && _fail.empty()) {
          _fail = "link " + std::to_string(i + 1) + ": " + e;
        }
        ++_done;
      }
      _cv_done.notify_all();
    }
  }

  bool
  stopped_()
  {
    std::lock_guard<std::mutex> lk(_stop_mu);
    return _stop && _stop();
  }

  std::mutex _stop_mu;
  std::function<bool()> _stop;
  std::atomic<bool> _closing{false};
  const bool _listen;
  std::vector<Link> _links;
  const std::vector<std::uint8_t> _key;
  const int _rank;
  const int _wait_ms;
  std::vector<std::string> _rdma_devices;
  const bool _rdma_auto;
  const int _gid_index;
  // Each device's share of a partial (its cable's speed); empty: even.
  std::vector<int> _rdma_weights;
  std::unique_ptr<RdmaPath> _rdma;
  std::vector<int> _listening;
  std::thread _setup;
  std::string _setup_err;
  std::vector<std::thread> _workers;

  std::mutex _mu;
  std::condition_variable _cv, _cv_done;
  std::uint64_t _gen = 0;
  bool _quit = false;
  int _done = 0;
  std::string _fail;
  const std::uint8_t* _out = nullptr;
  std::uint8_t* _in = nullptr;
  std::size_t _bytes = 0;
  std::uint64_t _seq = 0;
};

class Transport final : public TpTransport {
public:
  std::string_view name() const override { return "valtz-tp"; }

  std::shared_ptr<TpChannel>
  open(const FlexData& params, const TpShare& share,
       std::string* err) override
  {
    auto fail = [&](const std::string& why) {
      if (err != nullptr) { *err = why; }
      return std::shared_ptr<TpChannel>();
    };
    if (share.ranks != 2) { return fail("two ranks, one link pair each"); }
    if (!params.is_object()) { return fail("no params"); }
    auto o = params.as_object();
    auto str = [&](const char* k) {
      return o.contains(k) ? std::string(o.at(k).as_string("")) : "";
    };
    const std::string role = str("role");
    if (role != "listen" && role != "connect") {
      return fail("role is neither listen nor connect");
    }
    // Each array held while its view is read: a view of the temporary
    // at() returns dangles past its statement.
    std::vector<Channel::Link> links;
    const FlexData lk = o.contains("links") ? o.at("links") : FlexData();
    if (lk.is_array()) {
      auto a = lk.as_array();
      for (std::size_t i = 0; i < a.size(); ++i) {
        links.push_back({std::string(a.at(i).as_string("")), {}, -1});
      }
    }
    if (links.empty()) { return fail("no links"); }
    const FlexData ifs =
        o.contains("interfaces") ? o.at("interfaces") : FlexData();
    if (ifs.is_array()) {
      auto a = ifs.as_array();
      for (std::size_t i = 0; i < a.size() && i < links.size(); ++i) {
        links[i].interface = std::string(a.at(i).as_string(""));
      }
    }
    const std::string hex = str("key");
    std::vector<std::uint8_t> key = from_hex(hex);
    if (!hex.empty() && key.empty()) { return fail("the key is not hex"); }
    const int wait_ms =
        o.contains("wait_ms") ? (int)o.at("wait_ms").as_int(1800000)
                              : 1800000;
    // RDMA: its devices named, or "auto" -- the ports that are up, found
    // as the links are made.
    std::vector<std::string> rdma;
    bool rdma_auto = false;
    const FlexData rd = o.contains("rdma") ? o.at("rdma") : FlexData();
    if (rd.is_array()) {
      auto a = rd.as_array();
      for (std::size_t i = 0; i < a.size(); ++i) {
        rdma.emplace_back(a.at(i).as_string(""));
      }
      if (!rdma.empty() && !rdma_available()) {
        return fail("this macOS has no RDMA (26.2 and later, enabled with "
                    "rdma_ctl from recoveryOS)");
      }
    } else if (rd.is_string() && rd.as_string("") == "auto") {
      rdma_auto = true;
    } else if (!rd.is_null()) {
      return fail("rdma is a list of devices or \"auto\"");
    }
    // Each RDMA device's share of a partial, its cable's speed (Gb/s):
    // the same on both ranks, or the stripes would not meet.
    std::vector<int> weights;
    const FlexData rw =
        o.contains("rdma_weights") ? o.at("rdma_weights") : FlexData();
    if (rw.is_array()) {
      auto a = rw.as_array();
      for (std::size_t i = 0; i < a.size(); ++i) {
        weights.push_back(static_cast<int>(a.at(i).as_int(0)));
      }
    }
    const int gid = o.contains("gid_index")
                        ? (int)o.at("gid_index").as_int(1)
                        : 1;
    return std::make_shared<Channel>(role == "listen", std::move(links),
                                     std::move(key), share.rank, wait_ms,
                                     std::move(rdma), rdma_auto, gid,
                                     std::move(weights));
  }
};

}

void
register_tp(vpipe::VpipePluginContext*)
{
  // A host without tensor-parallel/1 has none of this: the plugin works
  // on without it.
  if (!vpipe::genai::tp_transports_available()) { return; }
  vpipe::genai::register_tp_transport(std::make_unique<Transport>());
}

}

// exchange.h's kTpTrafficSymbol: what the transport has carried.
extern "C" __attribute__((visibility("default"))) void
valtz_tp_traffic(unsigned long long out[3])
{
  using namespace valtz::plugin;
  out[0] = g_sent.load(std::memory_order_relaxed);
  out[1] = g_received.load(std::memory_order_relaxed);
  out[2] = g_channels.load();
}
static_assert(std::is_same_v<decltype(&valtz_tp_traffic),
                             valtz::exchange::TpTrafficFn>);
