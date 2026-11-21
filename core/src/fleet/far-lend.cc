#include "valtz/fleet/far-lend.h"

#include "far-compute.h"
#include "valtz-vpipe/far-rdma.h"
#include "valtz-vpipe/far-wire.h"

#include <arpa/inet.h>
#include <mach/mach.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace valtz::fleet {

namespace fw = valtz::far_wire;

namespace {

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

std::uint64_t
total_memory()
{
  std::uint64_t m = 0;
  std::size_t n = sizeof m;
  ::sysctlbyname("hw.memsize", &m, &n, nullptr, 0);
  return m;
}

// Free, inactive and purgeable pages: what can be had without paging out
// what this Mac's apps hold now.
std::uint64_t
available_memory()
{
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        reinterpret_cast<host_info64_t>(&vm), &n) !=
      KERN_SUCCESS) {
    return 0;
  }
  vm_size_t page = 0;
  host_page_size(mach_host_self(), &page);
  return ((std::uint64_t)vm.free_count + vm.inactive_count +
          vm.purgeable_count) *
         (std::uint64_t)page;
}

// A session's experts run here (kCompute), however the calls come -- its
// TCP connection or RDMA's call queue pair: made at the first call, over
// the session's memory.
struct Runner {
  std::mutex mu;
  std::uint8_t* mem = nullptr;
  std::uint64_t bytes = 0;
  std::unique_ptr<FarCompute> fc;
  bool failed = false;
};

// One call: `body` (`len` bytes: the form, `n` calls, the row) run into
// `sum` (n rows of the form's hidden elements). Its status.
std::uint64_t
run_call(Runner& r, std::uint64_t n, const std::uint8_t* body,
         std::size_t len, std::vector<std::uint8_t>* sum)
{
  const std::size_t head = sizeof(fw::ComputeForm);
  if (n == 0 || n > fw::kComputeMaxCalls ||
      len < head + n * sizeof(fw::ComputeCall)) {
    return fw::kBadOp;
  }
  fw::ComputeForm form{};
  std::memcpy(&form, body, head);
  const auto* calls = reinterpret_cast<const fw::ComputeCall*>(body + head);
  const std::size_t xat = head + (std::size_t)n * sizeof(fw::ComputeCall);
  sum->assign((std::size_t)n * form.hidden * 2, 0);   // a row a call
  if (len != xat + (std::size_t)form.hidden * 2) { return fw::kBadOp; }
  std::lock_guard<std::mutex> lk(r.mu);
  if (!r.fc && !r.failed) {
    std::string e;
    r.fc = FarCompute::create(r.mem, r.bytes, &e);
    if (!r.fc) {
      r.failed = true;
      std::fprintf(stderr, "valtz far lender: runs no experts: %s\n",
                   e.c_str());
    }
  }
  if (!r.fc) { return fw::kNoCompute; }
  for (std::uint64_t j = 0; j < n; ++j) {
    for (int k = 0; k < 6; ++k) {
      const std::uint64_t at = calls[j].part[k];
      const std::uint64_t pl = fw::compute_part_bytes(form, k);
      if (at > r.bytes || pl > r.bytes - at) { return fw::kOutOfRange; }
    }
  }
  std::string e;
  if (!r.fc->run(form, calls, (std::size_t)n, body + xat, sum->data(), &e)) {
    std::fprintf(stderr, "valtz far lender: an expert call: %s\n",
                 e.c_str());
    return fw::kBadOp;
  }
  return fw::kOk;
}

}

std::uint64_t
lendable_bytes(std::uint64_t reserve)
{
  const std::uint64_t avail = available_memory();
  std::uint64_t b = avail > reserve ? avail - reserve : 0;
  b = std::min(b, total_memory() / 4 * 3);
  return b >> 30 << 30;
}

std::uint64_t
lend_bytes_for(const std::string& setting)
{
  if (setting == "off") { return 0; }
  const std::uint64_t can = lendable_bytes();
  if (setting.empty() || setting == "auto") { return can; }
  const double gb = std::atof(setting.c_str());
  if (gb <= 0) { return 0; }
  return std::min(can, (std::uint64_t)(gb * (double)(1ull << 30)) >> 30
                           << 30);
}

// A connection's thread runs detached (a lender lives as long as the
// fleet: its connections come and go), counted, the count waited for.
struct FarLender::Threads {
  std::mutex mu;
  std::condition_variable cv;
  std::thread acceptor;
  int live = 0;
  std::vector<int> fds;
};

Result<std::unique_ptr<FarLender>>
FarLender::start(Options o)
{
  std::unique_ptr<FarLender> l(new FarLender());
  l->_o = std::move(o);
  far_rdma::lend_files(l->_o.files);
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons((std::uint16_t)l->_o.port);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  if (fd < 0 || ::bind(fd, (sockaddr*)&a, sizeof a) != 0 ||
      ::listen(fd, 64) != 0) {
    const std::string why = std::strerror(errno);
    if (fd >= 0) { ::close(fd); }
    return make_error(Code::Network, "cannot listen: " + why);
  }
  socklen_t n = sizeof a;
  ::getsockname(fd, (sockaddr*)&a, &n);
  l->_port = ntohs(a.sin_port);
  l->_listen = fd;
  l->_threads = std::make_unique<Threads>();
  l->_threads->acceptor = std::thread([p = l.get()] { p->accept_(); });
  return Result<std::unique_ptr<FarLender>>(std::move(l));
}

FarLender::~FarLender()
{
  _stop.store(true);
  if (_threads) {
    if (_threads->acceptor.joinable()) { _threads->acceptor.join(); }
    {
      std::lock_guard<std::mutex> lk(_threads->mu);
      for (int fd : _threads->fds) { ::shutdown(fd, SHUT_RDWR); }
    }
    std::unique_lock<std::mutex> lk(_threads->mu);
    _threads->cv.wait(lk, [&] { return _threads->live == 0; });
  }
  if (_listen >= 0) { ::close(_listen); }
  if (_mem != nullptr) { ::munmap(_mem, _bytes); }
}

FarLender::Traffic
FarLender::traffic() const
{
  std::lock_guard<std::mutex> lk(_smu);
  return {_read.load(), _written.load(), _bytes, _conns,
          _compute_calls.load(), _computed.load(), _compute_us.load(),
          _gpu.calls, _gpu.lag_us, _gpu.run_us, _gpu.notice_us};
}

void
FarLender::accept_()
{
  while (!_stop.load()) {
    pollfd p{_listen, POLLIN, 0};
    if (::poll(&p, 1, 200) <= 0) { continue; }
    const int fd = ::accept(_listen, nullptr, nullptr);
    if (fd < 0) { continue; }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int buf = 8 << 20;
    ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
    ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
    std::lock_guard<std::mutex> lk(_threads->mu);
    _threads->fds.push_back(fd);
    ++_threads->live;
    std::thread([this, fd] { serve_(fd); }).detach();
  }
}

void
FarLender::serve_(int fd)
{
  // A member's decode waits on each answer: on a performance core.
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  // The hello each way, then the proof where there is a key; only then
  // the session -- memory taken for a user who proved itself.
  const std::vector<std::uint8_t> key =
      _o.key ? _o.key() : std::vector<std::uint8_t>();
  fw::Hello mine{}, theirs{};
  std::memcpy(mine.magic, fw::kMagic, 4);
  mine.version = fw::kVersion;
  arc4random_buf(mine.nonce, fw::kNonce);
  mine.keyed = key.empty() ? 0 : 1;
  bool ok = recv_all(fd, &theirs, sizeof theirs) &&
            std::memcmp(theirs.magic, fw::kMagic, 4) == 0 &&
            theirs.version == fw::kVersion &&
            send_all(fd, &mine, sizeof mine);
  if (ok && !key.empty()) {
    fw::Proof got{};
    ok = recv_all(fd, got.data(), got.size()) &&
         got == fw::proof(key, 'u', theirs.nonce, mine.nonce);
    if (ok) {
      const fw::Proof p = fw::proof(key, 'l', mine.nonce, theirs.nonce);
      ok = send_all(fd, p.data(), p.size());
    }
  }
  // Its session: started, joined, or another user's (0 lent).
  std::uint64_t lent = 0;
  std::uint8_t* mem = nullptr;
  if (ok) {
    std::lock_guard<std::mutex> lk(_smu);
    if (_conns == 0) {
      const std::uint64_t b = _o.size ? _o.size() : 0;
      void* m = b > 0 ? ::mmap(nullptr, b, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0)
                      : MAP_FAILED;
      if (m != MAP_FAILED) {
        _mem = static_cast<std::uint8_t*>(m);
        _bytes = b;
        _user = theirs.user;
      }
    }
    if (_mem != nullptr && _user == theirs.user) {
      ++_conns;
      lent = _bytes;
      mem = _mem;
    }
    ok = send_all(fd, &lent, sizeof lent) && lent > 0;
  }
  const std::uint64_t bytes = lent;
  // Asked so, the session's reads and writes over RDMA (far-rdma.h): this
  // connection then carries nothing, its close their end.
  std::unique_ptr<far_rdma::Lender> rdma;
  // Experts run here (kCompute): over this connection, or RDMA's calls.
  auto runner = std::make_shared<Runner>();
  runner->mem = mem;
  runner->bytes = bytes;
  std::vector<std::uint8_t> body, sum;
  // one call, answered with its status, then the sum
  auto answer = [this, runner](std::uint64_t n, const std::uint8_t* b,
                               std::size_t len,
                               std::vector<std::uint8_t>* out) {
    std::vector<std::uint8_t> s;
    const auto g0 = std::chrono::steady_clock::now();
    const std::uint64_t st = run_call(*runner, n, b, len, &s);
    _compute_us.fetch_add((std::uint64_t)
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - g0).count());
    if (st == fw::kOk) {
      _computed.fetch_add(n);
      _compute_calls.fetch_add(1);
    }
    {
      std::lock_guard<std::mutex> lk(runner->mu);
      if (runner->fc) {
        const FarCompute::Timing t = runner->fc->timing();
        std::lock_guard<std::mutex> sl(_smu);
        _gpu = t;
      }
    }
    out->resize(sizeof st + s.size());
    std::memcpy(out->data(), &st, sizeof st);
    std::memcpy(out->data() + sizeof st, s.data(), s.size());
    return st;
  };
  while (ok && !_stop.load()) {
    fw::Request r{};
    if (!recv_all(fd, &r, sizeof r)) { break; }
    if (r.op == fw::kCompute) {
      if (r.length > (1u << 20)) { break; }   // not a call to read past
      body.resize((std::size_t)r.length);
      if (!recv_all(fd, body.data(), body.size())) { break; }
      const std::uint64_t st = answer(r.offset, body.data(), body.size(),
                                      &sum);
      // the sum only when it ran
      ok = send_all(fd, sum.data(),
                    st == fw::kOk ? sum.size() : sizeof st);
      continue;
    }
    if (r.op == fw::kRdma) {
      std::string e;
      // its calls on the links' call queue pair: the request is the TCP
      // call's -- a Request, then the body -- and so is the answer
      far_rdma::Lender::Call on_call =
          [answer](const std::uint8_t* q, std::size_t n,
                   std::vector<std::uint8_t>* out) {
            fw::Request h{};
            if (n < sizeof h) { return; }
            std::memcpy(&h, q, sizeof h);
            if (h.length > n - sizeof h) { return; }
            if (h.op == fw::kOpenFile) {
              // a file of the model's, by its size and name: its id
              std::uint64_t st = fw::kNoFile, id = 0, size = 0;
              if (h.length > sizeof size) {
                std::memcpy(&size, q + sizeof h, sizeof size);
                const std::string name(
                    reinterpret_cast<const char*>(q + sizeof h + sizeof size),
                    (std::size_t)h.length - sizeof size);
                std::string e;
                const std::int64_t got =
                    far_rdma::open_lent_file(name, size, &e);
                if (got >= 0) {
                  st = fw::kOk;
                  id = (std::uint64_t)got;
                } else {
                  std::fprintf(stderr, "valtz far lender: %s\n", e.c_str());
                }
              }
              out->resize(sizeof st + sizeof id);
              std::memcpy(out->data(), &st, sizeof st);
              std::memcpy(out->data() + sizeof st, &id, sizeof id);
              return;
            }
            if (h.op != fw::kCompute) { return; }
            answer(h.offset, q + sizeof h, (std::size_t)h.length, out);
          };
      rdma = rdma ? nullptr
                  : far_rdma::Lender::serve(fd, (std::uint32_t)r.offset, mem,
                                            bytes, &_read, &_written,
                                            std::move(on_call), &e);
      if (!rdma) {
        std::fprintf(stderr, "valtz far lender: no RDMA: %s\n", e.c_str());
        break;
      }
      continue;
    }
    std::uint64_t st = fw::kOk;
    if (r.op != fw::kRead && r.op != fw::kWrite) {
      st = fw::kBadOp;
    } else if (r.offset > bytes || r.length > bytes - r.offset) {
      st = fw::kOutOfRange;
    }
    if (r.op == fw::kRead) {
      ok = send_all(fd, &st, sizeof st) &&
           (st != fw::kOk || send_all(fd, mem + r.offset, r.length));
      if (ok && st == fw::kOk) { _read.fetch_add(r.length); }
    } else if (r.op == fw::kWrite) {
      // The bytes come whatever the status: a bad range is drained.
      if (st == fw::kOk) {
        ok = recv_all(fd, mem + r.offset, r.length);
      } else {
        std::vector<std::uint8_t> sink(1 << 20);
        std::uint64_t left = r.length;
        while (ok && left > 0) {
          const std::size_t k = (std::size_t)std::min<std::uint64_t>(
              left, sink.size());
          ok = recv_all(fd, sink.data(), k);
          left -= k;
        }
      }
      ok = ok && send_all(fd, &st, sizeof st);
      if (ok && st == fw::kOk) { _written.fetch_add(r.length); }
    } else {
      ok = send_all(fd, &st, sizeof st);
    }
  }
  rdma.reset();   // before the memory it reads and writes can go
  {
    std::lock_guard<std::mutex> lk(runner->mu);
    runner->fc.reset();
  }
  if (mem != nullptr) {
    // The session's last connection: its memory given back.
    std::lock_guard<std::mutex> lk(_smu);
    if (--_conns == 0) {
      ::munmap(_mem, _bytes);
      _mem = nullptr;
      _bytes = 0;
      _user = 0;
    }
  }
  ::close(fd);
  std::lock_guard<std::mutex> lk(_threads->mu);
  std::erase(_threads->fds, fd);
  --_threads->live;
  _threads->cv.notify_all();
}

}
