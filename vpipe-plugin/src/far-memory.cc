// valtz-far: another Mac's memory lent to a Mixture of Experts' expert
// cache (vpipe's generative-models/far-memory.h; DESIGN §11e). The Mac
// lending it runs the core's FarLender (fleet/far-lend.h); this is the
// other end, over valtz-vpipe/far-wire.h.
//
// SEVERAL CONNECTIONS A CABLE, every call striped across all of them: an
// expert is a few MB in a few pieces, and one connection waits a round
// trip a request, so a read is cut in CHUNKS taken from one queue by a
// worker a connection -- a faster cable takes more of them, and no chunk
// waits behind a slower one. Reads go before writes: a step waits on its
// reads, never on what the cache writes behind it.
//
// params (text-chat's far_memory.params, passed through):
//   links       ["host:port", ...]: the lender's address on each cable
//   interfaces  ["en2", ...]: each link's sockets bound to that interface,
//               so a link is the cable named
//   streams     connections a link (default 8)
//   chunk_kb    a chunk's size (default 1024)
//   key         hex: the fleet's key (empty: no proof -- loopback tests)
//   wait_ms     how long to wait for each connection (default 10000)
//   warm_us     KEEPS EACH CABLE WARM: a small read this often on a
//   warm_kb     connection of its own (default 4 KB every 100 us; 0 us
//               none), while real reads came in the last second. Idle
//               for a few hundred microseconds the path between the two
//               Macs is slow to wake -- an 18 MB read after 20 ms of
//               nothing took 3.9 ms against 1.5 ms back to back (M5
//               Ultra and M5 Max, three TB5 cables), and a decode's reads
//               come between its layers' GPU work. Warm: 1.6 ms. Not CPU
//               load on either Mac (spinners did nothing), nor a read
//               every 200 us or more.
//   rdma        ["rdma_en1", ...]: this Mac's RDMA devices, a cable each,
//   rdma_peer   ["rdma_en6", ...]: the lender's on the same cables -- the
//               reads and writes over RDMA (far-rdma.h), a link a device,
//               in place of the connections; where it cannot be set up,
//               said, the connections as above

#include "stages.h"
#include "valtz-vpipe/far-rdma.h"
#include "valtz-vpipe/far-wire.h"

#include "generative-models/far-memory.h"

#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <time.h>

namespace valtz::plugin {

namespace {

namespace fw = valtz::far_wire;
using vpipe::FlexData;
using vpipe::genai::FarCompute;
using vpipe::genai::FarExpertCall;
using vpipe::genai::FarExpertForm;
using vpipe::genai::FarMemory;
using vpipe::genai::kFarComputeExtension;
using vpipe::genai::FarFiles;
using vpipe::genai::kFarFilesExtension;
using vpipe::genai::FarMemoryTransport;
using vpipe::genai::FarPiece;

bool
send_all(int fd, const void* p, std::size_t n)
{
  const auto* c = static_cast<const std::uint8_t*>(p);
  while (n > 0) {
    const ssize_t k = ::send(fd, c, n, 0);
    if (k <= 0) {
      if (k < 0 && errno == EINTR) { continue; }
      return false;
    }
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
    if (k <= 0) {
      if (k < 0 && errno == EINTR) { continue; }
      return false;
    }
    c += k;
    n -= (std::size_t)k;
  }
  return true;
}

// A connection to `hp` through `ifname` (or any), its hello and proof
// made; -1 with `err`. `lent` the bytes the lender says it lends.
int
dial(const std::string& hp, const std::string& ifname,
     const std::vector<std::uint8_t>& key, int wait_ms, std::uint64_t user,
     std::uint64_t* lent, std::string* err)
{
  const std::size_t colon = hp.rfind(':');
  if (colon == std::string::npos) {
    *err = hp + ": not host:port";
    return -1;
  }
  const std::string host = hp.substr(0, colon), port = hp.substr(colon + 1);
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* res = nullptr;
  if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 ||
      res == nullptr) {
    *err = hp + ": cannot resolve it";
    return -1;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(wait_ms);
  int fd = -1;
  std::string why;
  while (fd < 0 && std::chrono::steady_clock::now() < deadline) {
    fd = ::socket(res->ai_family, SOCK_STREAM, 0);
    if (fd < 0) { break; }
    if (!ifname.empty()) {
      const unsigned idx = ::if_nametoindex(ifname.c_str());
      ::setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx);
    }
    // A Thunderbolt bridge often loses a link's first SYN: 100 ms, then
    // another (as valtz-tp's links).
    const int fl = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc != 0 && errno == EINPROGRESS) {
      pollfd p{fd, POLLOUT, 0};
      if (::poll(&p, 1, 100) > 0) {
        int e = 0;
        socklen_t n = sizeof e;
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &n);
        rc = e == 0 ? 0 : -1;
        if (e != 0) { why = std::strerror(e); }
      } else {
        why = "no answer";
      }
    } else if (rc != 0) {
      why = std::strerror(errno);
    }
    if (rc != 0) {
      ::close(fd);
      fd = -1;
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }
    ::fcntl(fd, F_SETFL, fl);
  }
  ::freeaddrinfo(res);
  if (fd < 0) {
    *err = hp + ": " + (why.empty() ? "cannot connect" : why);
    return -1;
  }
  int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  int buf = 8 << 20;
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
  timeval tv{30, 0};   // a lender gone quiet fails the call, not hangs it
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
  fw::Hello mine{}, theirs{};
  std::memcpy(mine.magic, fw::kMagic, 4);
  mine.version = fw::kVersion;
  arc4random_buf(mine.nonce, fw::kNonce);
  mine.user = user;
  if (!send_all(fd, &mine, sizeof mine) ||
      !recv_all(fd, &theirs, sizeof theirs)) {
    ::close(fd);
    *err = hp + ": the hello was cut";
    return -1;
  }
  if (std::memcmp(theirs.magic, fw::kMagic, 4) != 0 ||
      theirs.version != fw::kVersion) {
    ::close(fd);
    *err = hp + ": not a Valtz memory lender of this version";
    return -1;
  }
  if (theirs.keyed != 0) {
    const fw::Proof p = fw::proof(key, 'u', mine.nonce, theirs.nonce);
    fw::Proof got{};
    if (key.empty() || !send_all(fd, p.data(), p.size()) ||
        !recv_all(fd, got.data(), got.size()) ||
        got != fw::proof(key, 'l', theirs.nonce, mine.nonce)) {
      ::close(fd);
      *err = hp + ": the lender and this Mac do not share the key";
      return -1;
    }
  }
  // What it lends this user: 0, lent to another Mac or none to spare.
  std::uint64_t got = 0;
  if (!recv_all(fd, &got, sizeof got) || got == 0) {
    ::close(fd);
    *err = hp + ": the lender has no memory for this Mac now";
    return -1;
  }
  *lent = got;
  return fd;
}

class Lent final : public FarMemory, public FarCompute, public FarFiles {
public:
  struct Link {
    std::string address, interface;
  };

  ~Lent() override
  {
    {
      std::lock_guard<std::mutex> lk(_mu);
      _quit = true;
    }
    _stopping.store(true);
    _cv.notify_all();
    if (_warm.joinable()) { _warm.join(); }
    for (int fd : _warm_fds) { ::close(fd); }
    for (int fd : _fds) { ::shutdown(fd, SHUT_RDWR); }
    for (std::thread& t : _workers) {
      if (t.joinable()) { t.join(); }
    }
    for (int fd : _fds) { ::close(fd); }
    _rdma.clear();
    if (_rdma_fd >= 0) { ::close(_rdma_fd); }
    if (!_call_us.empty()) {
      // what running experts there cost, a call from send to answer
      std::vector<float> v = _call_us;
      std::sort(v.begin(), v.end());
      double sum = 0;
      for (float x : v) { sum += x; }
      auto q = [&](double f) {
        return v[std::min(v.size() - 1, (std::size_t)(f * v.size()))];
      };
      std::fprintf(stderr, "valtz-far: %zu expert calls (%llu experts), "
                           "%.0f us mean, p50 %.0f, p90 %.0f, p99 %.0f\n",
                   v.size(), (unsigned long long)_call_n, sum / v.size(),
                   q(0.5), q(0.9), q(0.99));
    }
  }

  // Over RDMA: one connection to set the links up and hold the session,
  // a worker a link. Null with `err` when it cannot be.
  static std::shared_ptr<Lent>
  open_rdma(const std::vector<Link>& links,
            const std::vector<std::string>& devices,
            const std::vector<std::string>& peers,
            const std::vector<std::uint8_t>& key, int wait_ms, int warm_us,
            int warm_kb, std::string* err)
  {
    std::shared_ptr<Lent> m(new Lent());
    m->_chunk = far_rdma::kPiece;
    m->_links = devices.size();
    std::uint64_t user = 0;
    while (user == 0) { arc4random_buf(&user, sizeof user); }
    std::uint64_t lent = 0;
    m->_rdma_fd = dial(links[0].address, links[0].interface, key, wait_ms,
                       user, &lent, err);
    if (m->_rdma_fd < 0) { return nullptr; }
    m->_rdma = far_rdma::connect(m->_rdma_fd, devices, peers, err);
    if (m->_rdma.empty()) { return nullptr; }
    m->_bytes = lent;
    m->_where = links[0].address;
    m->_wait_ms = std::max(wait_ms, 1000);
    const std::size_t warm = std::min<std::size_t>(
        (std::size_t)warm_kb << 10, far_rdma::kPiece);
    for (auto& l : m->_rdma) {
      m->_workers.emplace_back([p = m.get(), l = l.get(), warm_us, warm] {
        p->rwork_(l, warm_us, warm);
      });
    }
    return m;
  }

  static std::shared_ptr<Lent>
  open(const std::vector<Link>& links, int streams, std::size_t chunk,
       const std::vector<std::uint8_t>& key, int wait_ms, int warm_us,
       int warm_kb, std::string* err)
  {
    std::shared_ptr<Lent> m(new Lent());
    m->_chunk = chunk;
    m->_links = links.size();
    // Every connection made at once: a link's several in parallel.
    // The warm connections after the workers': one a link.
    const std::size_t nw = warm_us > 0 ? links.size() : 0;
    const std::size_t n = links.size() * (std::size_t)streams + nw;
    std::vector<int> fds(n, -1);
    std::vector<std::uint64_t> lent(n, 0);
    std::vector<std::string> errs(n);
    // One user: every connection of this far memory's life.
    std::uint64_t user = 0;
    while (user == 0) { arc4random_buf(&user, sizeof user); }
    std::vector<std::thread> ts;
    for (std::size_t i = 0; i < n; ++i) {
      ts.emplace_back([&, i] {
        const Link& l = links[i % links.size()];
        fds[i] = dial(l.address, l.interface, key, wait_ms, user, &lent[i],
                      &errs[i]);
      });
    }
    for (std::thread& t : ts) { t.join(); }
    std::string e;
    for (std::size_t i = 0; i < n; ++i) {
      if (fds[i] < 0 && e.empty()) { e = errs[i]; }
      if (fds[i] >= 0 && lent[i] != lent[0]) {
        e = "the links reach different lenders";
      }
    }
    for (std::size_t i = 0; i < n; ++i) {
      if (fds[i] < 0) { continue; }
      (i + nw >= n ? m->_warm_fds : m->_fds).push_back(fds[i]);
    }
    if (!e.empty() || m->_fds.empty()) {
      *err = e.empty() ? "no connection" : e;
      return nullptr;    // the destructor closes what was made
    }
    m->_bytes = lent[0];
    m->_where = links[0].address;
    for (std::size_t i = 0; i < m->_fds.size(); ++i) {
      m->_workers.emplace_back([p = m.get(), fd = m->_fds[i]] {
        p->work_(fd);
      });
    }
    if (!m->_warm_fds.empty()) {
      m->_warm = std::thread([p = m.get(), warm_us, warm_kb] {
        p->warm_(warm_us, (std::size_t)warm_kb << 10);
      });
    }
    return m;
  }

  std::uint64_t bytes() const override { return _bytes; }

  std::string describe() const override
  {
    if (!_rdma.empty()) {
      std::string names;
      for (const auto& l : _rdma) {
        names += (names.empty() ? "" : ", ") + l->name();
      }
      return std::format("{:.1f} GB lent at {} over RDMA ({})",
                         (double)_bytes / (double)(1ull << 30), _where,
                         names);
    }
    return std::format("{:.1f} GB lent at {} over {} link{}, {} connections",
                       (double)_bytes / (double)(1ull << 30), _where, _links,
                       _links == 1 ? "" : "s", _fds.size());
  }


  bool read(const FarPiece* p, std::size_t n, std::string* err) override
  {
    return call_(fw::kRead, p, n, err);
  }

  bool write(const FarPiece* p, std::size_t n, std::string* err) override
  {
    return call_(fw::kWrite, p, n, err);
  }

  // RUN THERE (far-compute/2; PROTOTYPE): over RDMA, the session's own
  // connection -- idle once the links are up -- carries the calls, one at
  // a time (VALTZ_FAR_COMPUTE=0: not offered).
  void* query_extension(std::string_view id) noexcept override
  {
    // ITS DISK TOO (far-files/1; PROTOTYPE): over RDMA, the lender's copies
    // of the model's files (VALTZ_FAR_FILES=0: not offered).
    if (id == kFarFilesExtension) {
      const char* v = std::getenv("VALTZ_FAR_FILES");
      if (_rdma.empty() || _call_tcp || (v != nullptr && std::atoi(v) == 0)) {
        return nullptr;
      }
      return static_cast<FarFiles*>(this);
    }
    if (id != kFarComputeExtension || _rdma_fd < 0) { return nullptr; }
    const char* v = std::getenv("VALTZ_FAR_COMPUTE");
    if (v != nullptr && std::atoi(v) == 0) { return nullptr; }
    return static_cast<FarCompute*>(this);
  }

  int open(std::string_view name, std::uint64_t size,
           std::string* err) override
  {
    if (name.empty() || name.size() > 4000) {
      if (err != nullptr) { *err = "a bad file name"; }
      return -1;
    }
    std::vector<std::uint8_t> buf(sizeof(fw::Request) + sizeof size +
                                  name.size());
    const fw::Request r{fw::kOpenFile, 0, 0, sizeof size + name.size()};
    std::memcpy(buf.data(), &r, sizeof r);
    std::memcpy(buf.data() + sizeof r, &size, sizeof size);
    std::memcpy(buf.data() + sizeof r + sizeof size, name.data(),
                name.size());
    std::uint64_t ans[2] = {};
    std::lock_guard<std::mutex> lk(_cmu);
    const std::string e =
        _rdma[0]->call(buf.data(), buf.size(), ans, sizeof ans, _wait_ms);
    if (!e.empty() || ans[0] != fw::kOk) {
      if (err != nullptr) {
        *err = !e.empty() ? e : "the lender has no copy of " +
                                    std::string(name);
      }
      return -1;
    }
    return (int)ans[1];
  }

  bool read(int id, std::uint64_t offset, void* local, std::size_t bytes,
            std::string* err) override
  {
    if (id < 0 || offset >= (std::uint64_t(1) << fw::kFilePosBits)) {
      if (err != nullptr) { *err = "a bad file read"; }
      return false;
    }
    const FarPiece p{fw::file_offset((std::uint64_t)id, offset), local,
                     bytes};
    return call_(fw::kRead, &p, 1, err);
  }

  bool experts(const FarExpertForm& form, const void* x,
               const FarExpertCall* calls, std::size_t n, void* out,
               std::string* err) override
  {
    static_assert(sizeof(FarExpertForm) == sizeof(fw::ComputeForm));
    static_assert(sizeof(FarExpertCall) == sizeof(fw::ComputeCall));
    if (n == 0 || n > fw::kComputeMaxCalls || form.hidden == 0 ||
        form.hidden > (1u << 16)) {
      if (err != nullptr) { *err = "a bad far compute call"; }
      return false;
    }
    const std::size_t xb = (std::size_t)form.hidden * 2;
    const std::size_t ob = n * xb;   // the answer: a row a call
    const std::size_t body =
        sizeof(fw::ComputeForm) + n * sizeof(fw::ComputeCall) + xb;
    thread_local std::vector<std::uint8_t> buf;
    buf.resize(sizeof(fw::Request) + body);
    fw::Request r{fw::kCompute, 0, n, body};
    std::uint8_t* at = buf.data();
    std::memcpy(at, &r, sizeof r);
    at += sizeof r;
    std::memcpy(at, &form, sizeof form);
    at += sizeof form;
    std::memcpy(at, calls, n * sizeof(fw::ComputeCall));
    at += n * sizeof(fw::ComputeCall);
    std::memcpy(at, x, xb);
    {
      std::lock_guard<std::mutex> lk(_mu);
      _last_call = std::chrono::steady_clock::now();   // the cables kept warm
    }
    std::lock_guard<std::mutex> lk(_cmu);
    const auto t0 = std::chrono::steady_clock::now();
    struct Timed {
      Lent* m;
      std::chrono::steady_clock::time_point t0;
      std::size_t n;
      ~Timed()
      {
        const double us = std::chrono::duration<double, std::micro>(
                              std::chrono::steady_clock::now() - t0).count();
        m->_call_us.push_back((float)us);
        m->_call_n += n;
      }
    } timed{this, t0, n};
    std::uint64_t st = 0;
    if (!_rdma.empty() && !_call_tcp) {
      // over the first link's call queue pair: the same request, the
      // answer its status and the rows
      thread_local std::vector<std::uint8_t> ans;
      ans.resize(sizeof st + ob);
      const std::string e =
          _rdma[0]->call(buf.data(), buf.size(), ans.data(), ans.size(),
                         _wait_ms);
      if (!e.empty()) {
        if (err != nullptr) { *err = e; }
        return false;
      }
      std::memcpy(&st, ans.data(), sizeof st);
      if (st == fw::kOk) { std::memcpy(out, ans.data() + sizeof st, ob); }
    } else if (!send_all(_rdma_fd, buf.data(), buf.size()) ||
               !recv_all(_rdma_fd, &st, sizeof st)) {
      if (err != nullptr) { *err = "the lender stopped answering"; }
      return false;
    }
    if (st != fw::kOk) {
      if (err != nullptr) {
        *err = st == fw::kNoCompute ? "the lender runs no experts"
               : st == fw::kOutOfRange ? "an expert past the lent memory"
                                       : "the lender refused the call";
      }
      return false;
    }
    if ((_rdma.empty() || _call_tcp) && !recv_all(_rdma_fd, out, ob)) {
      if (err != nullptr) { *err = "the lender stopped answering"; }
      return false;
    }
    return true;
  }

private:
  Lent() = default;

  struct Call {
    int left = 0;
    std::string err;
    std::condition_variable cv;
  };
  struct Chunk {
    std::uint32_t op;
    std::uint64_t offset;
    std::uint8_t* local;
    std::size_t bytes;
    Call* call;
  };

  // Each cable kept warm: a 4 KB read on its warm connection every
  // `us`, until the far memory closes or a read fails.
  void
  warm_(int us, std::size_t bytes)
  {
    std::vector<std::uint8_t> v(bytes);
    std::uint8_t* buf = v.data();
    for (;;) {
      {
        std::unique_lock<std::mutex> lk(_mu);
        if (_cv.wait_for(lk, std::chrono::microseconds(us),
                         [&] { return _quit; })) {
          return;
        }
        // Only while reads come: none for a second, wait for the next.
        _cv.wait(lk, [&] {
          return _quit || std::chrono::steady_clock::now() - _last_call <
                              std::chrono::seconds(1);
        });
        if (_quit) { return; }
      }
      for (int fd : _warm_fds) {
        const fw::Request r{fw::kRead, 0, 0, bytes};
        std::uint64_t st = 1;
        if (!send_all(fd, &r, sizeof r) || !recv_all(fd, &st, sizeof st) ||
            st != fw::kOk || !recv_all(fd, buf, bytes)) {
          return;   // a warm link lost costs speed, not the far memory
        }
      }
    }
  }

  bool
  call_(std::uint32_t op, const FarPiece* p, std::size_t n, std::string* err)
  {
    Call c;
    std::unique_lock<std::mutex> lk(_mu);
    _last_call = std::chrono::steady_clock::now();
    if (_dead) {
      if (err != nullptr) { *err = _why; }
      return false;
    }
    std::deque<Chunk>& q = op == fw::kRead ? _reads : _writes;
    for (std::size_t i = 0; i < n; ++i) {
      if (!fw::is_file(p[i].offset) &&
          (p[i].offset > _bytes || p[i].bytes > _bytes - p[i].offset)) {
        if (err != nullptr) { *err = "past the memory lent"; }
        // what was queued of this call is still the workers'
        _cv_done.wait(lk, [&] { return c.left == 0; });
        return false;
      }
      auto* at = static_cast<std::uint8_t*>(p[i].local);
      for (std::size_t o = 0; o < p[i].bytes; o += _chunk) {
        q.push_back({op, p[i].offset + o, at + o,
                     std::min(_chunk, p[i].bytes - o), &c});
        ++c.left;
        _queued.fetch_add(1, std::memory_order_relaxed);
      }
    }
    _cv.notify_all();
    _cv_done.wait(lk, [&] { return c.left == 0; });
    if (!c.err.empty()) {
      if (err != nullptr) { *err = c.err; }
      return false;
    }
    return true;
  }

  void
  work_(int fd)
  {
    // A decode step waits on these: woken at once, on a performance core.
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::unique_lock<std::mutex> lk(_mu);
    for (;;) {
      _cv.wait(lk, [&] {
        return _quit || !_reads.empty() || !_writes.empty();
      });
      if (_quit) { return; }
      std::deque<Chunk>& q = !_reads.empty() ? _reads : _writes;
      const Chunk ch = q.front();
      q.pop_front();
      _queued.fetch_sub(1, std::memory_order_relaxed);
      const bool dead = _dead;
      lk.unlock();
      std::string e;
      if (dead) {
        e = _why;
      } else {
        const fw::Request r{ch.op, 0, ch.offset, ch.bytes};
        std::uint64_t st = 0;
        bool ok = send_all(fd, &r, sizeof r);
        if (ok && ch.op == fw::kWrite) {
          ok = send_all(fd, ch.local, ch.bytes);
        }
        ok = ok && recv_all(fd, &st, sizeof st);
        if (ok && st == fw::kOk && ch.op == fw::kRead) {
          ok = recv_all(fd, ch.local, ch.bytes);
        }
        if (!ok) {
          e = "the link to the lender failed";
        } else if (st != fw::kOk) {
          e = std::format("the lender refused a {} ({})",
                          ch.op == fw::kRead ? "read" : "write", st);
        }
      }
      lk.lock();
      if (!e.empty()) {
        if (ch.call->err.empty()) { ch.call->err = e; }
        // One connection gone takes the rest with it: a call cut half
        // way leaves a far slot that is not what the cache believes.
        if (!_dead) {
          _dead = true;
          _why = e;
        }
      }
      if (--ch.call->left == 0) { _cv_done.notify_all(); }
    }
  }

  // A chunk done, `e` its error: its call told when it was the last.
  // Under _mu.
  void
  finish_(const Chunk& ch, const std::string& e)
  {
    if (!e.empty()) {
      if (ch.call->err.empty()) { ch.call->err = e; }
      if (!_dead) {
        _dead = true;
        _why = e;
      }
    }
    if (--ch.call->left == 0) { _cv_done.notify_all(); }
  }

  // An RDMA link's worker: chunks taken while it has room -- reads first
  // -- posted, its queue polled, spinning while any is out. Idle while
  // calls come, a small read every `warm_us` keeps its cable awake.
  void
  rwork_(far_rdma::UserLink* l, int warm_us, std::size_t warm)
  {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    using Clock = std::chrono::steady_clock;
    constexpr std::uint64_t kWarmTag = 0;
    // STUDY (VALTZ_FAR_USER_WAIT=sleep<N>): a pass that moved nothing
    // sleeps N us instead of polling again at once. Each worker says at
    // its end how much of its life it kept a core busy.
    static const int nap_us = [] {
      const char* v = std::getenv("VALTZ_FAR_USER_WAIT");
      if (v == nullptr || std::strncmp(v, "sleep", 5) != 0) { return 0; }
      return std::max(1, std::atoi(v + 5));
    }();
    struct Busy {
      std::string name;
      Clock::time_point t0 = Clock::now();
      double cpu0 = cpu();
      static double cpu()
      {
        timespec ts{};
        clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
        return ts.tv_sec + ts.tv_nsec * 1e-9;
      }
      ~Busy()
      {
        const double wall =
            std::chrono::duration<double>(Clock::now() - t0).count();
        const double c = cpu() - cpu0;
        std::fprintf(stderr,
                     "valtz-far: worker %s busy %.1f s of %.1f s (%.0f%%)"
                     "%s\n",
                     name.c_str(), c, wall, wall > 0 ? 100 * c / wall : 0.0,
                     nap_us > 0 ? " (napping)" : "");
      }
    } busy{l->name()};
    std::vector<std::uint8_t> scratch(std::max<std::size_t>(warm, 1));
    std::map<std::uint64_t, Chunk> out;
    std::uint64_t next = 1;
    bool warming = false;
    auto moved = Clock::now();
    std::vector<Chunk> take;
    std::vector<std::uint64_t> done;
    for (;;) {
      take.clear();
      const bool room = l->can_read() || l->can_write();
      if (l->idle() && !warming) {
        std::unique_lock<std::mutex> lk(_mu);
        auto has = [&] {
          return _quit || !_reads.empty() || !_writes.empty();
        };
        if (!has()) {
          const bool warm_now =
              warm_us > 0 && warm > 0 &&
              Clock::now() - _last_call < std::chrono::seconds(1);
          if (warm_now) {
            _cv.wait_for(lk, std::chrono::microseconds(warm_us), has);
          } else {
            _cv.wait(lk, has);
          }
        }
        if (_quit) { return; }
        if (!has() && warm_us > 0) {
          lk.unlock();
          const std::string e = l->read(0, scratch.data(), warm, kWarmTag);
          if (!e.empty()) {
            std::lock_guard<std::mutex> g(_mu);
            if (!_dead) {
              _dead = true;
              _why = e;
            }
            return;
          }
          warming = true;
          moved = Clock::now();
          continue;
        }
      }
      if (room && _queued.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> lk(_mu);
        if (_quit) { return; }
        // Reads first; a write only where no read waits.
        bool r = l->can_read(), w = l->can_write();
        std::size_t nr = 0, nw = 0;
        while (r && nr < 3 && !_reads.empty()) {
          take.push_back(_reads.front());
          _reads.pop_front();
          ++nr;
        }
        while (w && nw < 3 && _reads.empty() && !_writes.empty()) {
          take.push_back(_writes.front());
          _writes.pop_front();
          ++nw;
        }
        _queued.fetch_sub((int)take.size(), std::memory_order_relaxed);
      }
      // Posted one at a time while the link has room; the rest back.
      std::string e;
      for (std::size_t i = 0; i < take.size(); ++i) {
        const Chunk& ch = take[i];
        const bool rd = ch.op == fw::kRead;
        if (_dead || !e.empty()) {
          std::lock_guard<std::mutex> lk(_mu);
          finish_(ch, e.empty() ? _why : e);
          continue;
        }
        if (rd ? !l->can_read() : !l->can_write()) {
          std::lock_guard<std::mutex> lk(_mu);
          (rd ? _reads : _writes).push_front(ch);
          _queued.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const std::uint64_t t = next++;
        e = rd ? l->read(ch.offset, ch.local, ch.bytes, t)
               : l->write(ch.offset, ch.local, ch.bytes, t);
        if (e.empty()) {
          out.emplace(t, ch);
        } else {
          std::lock_guard<std::mutex> lk(_mu);
          finish_(ch, e);
        }
      }
      done.clear();
      bool now_moved = false;
      if (e.empty()) {
        if (l->poll(&done, &e)) {
          moved = Clock::now();
          now_moved = true;
        }
      }
      if (nap_us > 0 && e.empty() && !now_moved && take.empty()) {
        std::this_thread::sleep_for(std::chrono::microseconds(nap_us));
      }
      if (_stopping.load(std::memory_order_relaxed)) { return; }
      if (e.empty() && !l->idle() &&
          Clock::now() - moved > std::chrono::milliseconds(_wait_ms)) {
        e = l->name() + ": the lender stopped answering";
      }
      if (!done.empty() || !e.empty()) {
        std::lock_guard<std::mutex> lk(_mu);
        for (std::uint64_t t : done) {
          if (t == kWarmTag) {
            warming = false;
            continue;
          }
          auto it = out.find(t);
          if (it == out.end()) { continue; }
          finish_(it->second, {});
          out.erase(it);
        }
        if (!e.empty()) {
          // Everything out on this link lost with it: their calls fail.
          for (auto& [t, ch] : out) { finish_(ch, e); }
          out.clear();
          if (!_dead) {
            _dead = true;
            _why = e;
          }
          // Calls after it fail at once (the TCP workers' rule).
          while (!_reads.empty() || !_writes.empty()) {
            std::deque<Chunk>& q = !_reads.empty() ? _reads : _writes;
            finish_(q.front(), _why);
            q.pop_front();
            _queued.fetch_sub(1, std::memory_order_relaxed);
          }
          _cv_done.notify_all();
          return;
        }
      }
    }
  }

  std::mutex _mu, _cmu;
  std::vector<float> _call_us;      // each expert call's, under _cmu
  // calls over the session's TCP connection, not RDMA's call queue pair
  // (VALTZ_FAR_CALL_TCP=1: an A/B)
  const bool _call_tcp = [] {
    const char* v = std::getenv("VALTZ_FAR_CALL_TCP");
    return v != nullptr && std::atoi(v) != 0;
  }();
  std::uint64_t _call_n = 0;
  std::condition_variable _cv, _cv_done;
  std::deque<Chunk> _reads, _writes;
  bool _quit = false, _dead = false;
  std::string _why;
  std::vector<int> _fds, _warm_fds;
  std::vector<std::thread> _workers;
  std::thread _warm;
  std::chrono::steady_clock::time_point _last_call{};
  std::uint64_t _bytes = 0;
  std::atomic<int> _queued{0};
  std::atomic<bool> _stopping{false};
  std::vector<std::unique_ptr<far_rdma::UserLink>> _rdma;
  int _rdma_fd = -1;
  int _wait_ms = 10000;
  std::size_t _chunk = 2 << 20;
  std::size_t _links = 0;
  std::string _where;
};

class Transport final : public FarMemoryTransport {
public:
  std::string_view name() const override { return "valtz-far"; }

  std::shared_ptr<FarMemory>
  open(const FlexData& params, std::string* err) override
  {
    auto fail = [&](const std::string& why) {
      if (err != nullptr) { *err = why; }
      return std::shared_ptr<FarMemory>();
    };
    if (!params.is_object()) { return fail("no params"); }
    auto o = params.as_object();
    std::vector<Lent::Link> links;
    const FlexData lk = o.contains("links") ? o.at("links") : FlexData();
    if (lk.is_array()) {
      auto a = lk.as_array();
      for (std::size_t i = 0; i < a.size(); ++i) {
        links.push_back({std::string(a.at(i).as_string("")), {}});
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
    const std::string hex =
        o.contains("key") ? std::string(o.at("key").as_string("")) : "";
    const std::vector<std::uint8_t> key = fw::from_hex(hex.data(),
                                                       hex.size());
    if (!hex.empty() && key.empty()) { return fail("the key is not hex"); }
    const int streams = std::clamp(
        o.contains("streams") ? (int)o.at("streams").as_int(8) : 8, 1, 32);
    const std::size_t chunk = (std::size_t)std::clamp<std::int64_t>(
        o.contains("chunk_kb") ? o.at("chunk_kb").as_int(1024) : 1024, 64,
        65536) << 10;
    const int wait_ms =
        o.contains("wait_ms") ? (int)o.at("wait_ms").as_int(10000) : 10000;
    const int warm_us = std::clamp(
        o.contains("warm_us") ? (int)o.at("warm_us").as_int(100) : 100, 0,
        1000000);
    const int warm_kb = std::clamp(
        o.contains("warm_kb") ? (int)o.at("warm_kb").as_int(4) : 4, 1, 16384);
    auto names = [&](const char* k) {
      std::vector<std::string> v;
      const FlexData a = o.contains(k) ? o.at(k) : FlexData();
      if (a.is_array()) {
        auto x = a.as_array();
        for (std::size_t i = 0; i < x.size(); ++i) {
          v.emplace_back(x.at(i).as_string(""));
        }
      }
      return v;
    };
    const std::vector<std::string> rdma = names("rdma");
    const std::vector<std::string> peers = names("rdma_peer");
    std::string e;
    std::shared_ptr<Lent> m;
    if (!rdma.empty()) {
      m = Lent::open_rdma(links, rdma, peers, key, wait_ms, warm_us, warm_kb,
                          &e);
      if (!m) {
        std::fprintf(stderr, "valtz-far: no RDMA (%s): over TCP\n",
                     e.c_str());
        e.clear();
      }
    }
    if (!m) {
      m = Lent::open(links, streams, chunk, key, wait_ms, warm_us, warm_kb,
                     &e);
    }
    if (!m) { return fail(e); }
    std::fprintf(stderr, "valtz-far: %s\n", m->describe().c_str());
    return m;
  }
};

}

void
register_far(vpipe::VpipePluginContext*)
{
  // A libvpipe without far-memory/1 has none of this.
  if (!vpipe::genai::far_memory_available()) { return; }
  vpipe::genai::register_far_memory_transport(std::make_unique<Transport>());
}

}
