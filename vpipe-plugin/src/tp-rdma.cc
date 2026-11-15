#include "tp-rdma.h"

#include <infiniband/verbs.h>

#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <list>
#include <map>
#include <thread>

namespace valtz::plugin {

namespace {

constexpr std::size_t kPage = 16384;
// A piece: one message, one receive. Three at once keep a queue pair
// under its 4,096,000 bytes of receives posted -- and of sends: a fourth
// send posted before the first's completion is reaped is refused (ENOMEM),
// and a credit for piece k + 3 can come in ahead of piece k's completion.
constexpr std::size_t kMsg = std::size_t(1) << 20;
constexpr std::uint32_t kWindow = 3;
// A memory region spans kSpan from where it starts and a piece more, so
// a piece starting in a region's span lies wholly inside it, wherever the
// buffer starts: 15 MiB and a page, under the device's 16,384,000.
constexpr std::size_t kSpan = std::size_t(14) << 20;
// The control queue pair's receives (credits) and its sends' slots.
constexpr int kSlots = 64;
constexpr int kSendSlots = 64;
constexpr std::size_t kSlotBytes = 32;

enum : std::uint64_t {
  kCtlRecv = 1,
  kDataRecv = 2,
  kCtlSend = 3,
  kDataSend = 4,
};

std::uint64_t
tag(std::uint64_t kind, std::uint64_t i)
{
  return (kind << 56) | i;
}

// A credit: the receiver has posted a receive for piece `piece` of
// exchange `seq`, `len` bytes. Of seq kHello, a link's hello at setup,
// `piece` the link's place in the peer's list.
constexpr std::uint64_t kHello = ~std::uint64_t(0);
struct Credit {
  std::uint64_t seq;
  std::uint32_t piece;
  std::uint32_t len;
};
static_assert(sizeof(Credit) <= kSlotBytes);

// What a rank tells its peer about a link, over TCP.
struct Info {
  std::uint32_t lid;
  std::uint32_t ctl_qpn;
  std::uint32_t data_qpn;
  std::uint32_t psn;
  std::uint8_t gid[16];
};

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

std::string
verr(const char* what, int rc)
{
  return std::string(what) + ": " + std::strerror(rc < 0 ? -rc : rc);
}

// A mapping of the process's memory -- an exchange buffer's -- and its
// registrations: the block from `page` + i kSpan, kSpan + kMsg long (to
// the mapping's end), registered as an exchange first touches it, null
// until then. Every exchange through the buffer falls on the same blocks.
struct Region {
  std::uintptr_t base = 0;
  std::size_t len = 0;
  std::uintptr_t page = 0;
  std::vector<ibv_mr*> mrs;

  std::uint32_t
  lkey(std::uintptr_t a) const
  {
    return mrs[(a - page) / kSpan]->lkey;
  }
};

struct Link {
  std::string name;
  ibv_context* ctx = nullptr;
  ibv_pd* pd = nullptr;
  ibv_cq* cq = nullptr;
  ibv_qp* ctl = nullptr;
  ibv_qp* data = nullptr;
  std::uint8_t* ctl_mem = nullptr;     // kSlots receive, kSendSlots send
  ibv_mr* ctl_mr = nullptr;
  std::uint16_t lid = 0;
  ibv_gid gid{};
  std::list<Region> regions;          // a list: the exchange's pointers
                                      // to two stay put

  // The exchange under way.
  bool active = false;
  std::uint64_t seq = 0;
  const std::uint8_t* src = nullptr;
  std::uint8_t* dst = nullptr;
  std::size_t len = 0;
  std::uint32_t pieces = 0;
  const Region* rsrc = nullptr;
  const Region* rdst = nullptr;
  std::uint32_t posted = 0, landed = 0, credits = 0, sent = 0, sends = 0;
  std::uint64_t ctl_posted = 0, ctl_done = 0;
  // Credits for exchanges this rank has not reached: the peer ahead.
  std::map<std::uint64_t, std::uint32_t> early;
  // The peer's hello at setup: its link's place, -1 before it.
  long hello = -1;

  ~Link()
  {
    // Its queue pairs RESET before they go: destroyed live, Apple's verbs
    // drain each for ~50-250 ms, one device after another (0.4 s for four
    // cables, between a split's denoise and its decode); reset first, the
    // wait is ~100 ms once, the links let go together (~Impl).
    for (ibv_qp* q : {data, ctl}) {
      if (q == nullptr) { continue; }
      ibv_qp_attr a{};
      a.qp_state = IBV_QPS_RESET;
      ibv_modify_qp(q, &a, IBV_QP_STATE);
    }
    if (data != nullptr) { ibv_destroy_qp(data); }
    if (ctl != nullptr) { ibv_destroy_qp(ctl); }
    if (cq != nullptr) { ibv_destroy_cq(cq); }
    for (Region& r : regions) {
      for (ibv_mr* m : r.mrs) {
        if (m != nullptr) { ibv_dereg_mr(m); }
      }
    }
    if (ctl_mr != nullptr) { ibv_dereg_mr(ctl_mr); }
    std::free(ctl_mem);
    if (pd != nullptr) { ibv_dealloc_pd(pd); }
    if (ctx != nullptr) { ibv_close_device(ctx); }
  }

  std::size_t
  piece_len(std::uint32_t k) const
  {
    return std::min(kMsg, len - (std::size_t)k * kMsg);
  }

  std::string
  open_(int gid_index)
  {
    int n = 0;
    ibv_device** list = ibv_get_device_list(&n);
    for (int i = 0; i < n && list != nullptr; ++i) {
      if (name == ibv_get_device_name(list[i])) {
        ctx = ibv_open_device(list[i]);
      }
    }
    if (list != nullptr) { ibv_free_device_list(list); }
    if (ctx == nullptr) { return "no RDMA device " + name; }
    ibv_device_attr da{};
    ibv_port_attr pa{};
    if (ibv_query_device(ctx, &da) != 0 || ibv_query_port(ctx, 1, &pa)) {
      return name + ": cannot query it";
    }
    if (pa.state != IBV_PORT_ACTIVE) { return name + ": its port is down"; }
    if (da.max_mr_size < kSpan + kMsg + kPage || pa.max_msg_sz < kMsg) {
      return name + ": its regions or messages are too small";
    }
    lid = pa.lid;
    if (ibv_query_gid(ctx, 1, gid_index, &gid) != 0) {
      return name + ": no GID " + std::to_string(gid_index);
    }
    pd = ibv_alloc_pd(ctx);
    cq = pd != nullptr ? ibv_create_cq(ctx, 1024, nullptr, nullptr, 0)
                       : nullptr;
    if (cq == nullptr) { return name + ": no protection domain or queue"; }
    ibv_qp_init_attr qa{};
    qa.send_cq = cq;
    qa.recv_cq = cq;
    qa.qp_type = IBV_QPT_UC;
    qa.cap.max_send_wr = 256;
    qa.cap.max_recv_wr = 128;
    qa.cap.max_send_sge = 1;
    qa.cap.max_recv_sge = 1;
    ctl = ibv_create_qp(pd, &qa);
    data = ctl != nullptr ? ibv_create_qp(pd, &qa) : nullptr;
    if (data == nullptr) { return name + ": no queue pairs"; }
    const std::size_t cb = (std::size_t)(kSlots + kSendSlots) * kSlotBytes;
    if (::posix_memalign((void**)&ctl_mem, kPage, cb) != 0) {
      ctl_mem = nullptr;
      return name + ": no memory";
    }
    std::memset(ctl_mem, 0, cb);
    ctl_mr = ibv_reg_mr(pd, ctl_mem, cb, IBV_ACCESS_LOCAL_WRITE);
    if (ctl_mr == nullptr) { return name + ": cannot register memory"; }
    return {};
  }

  // INIT, RTR toward the peer's queue pair `dest`, RTS.
  std::string
  bring_up_(ibv_qp* qp, std::uint32_t dest, const Info& peer,
            std::uint32_t psn, int gid_index)
  {
    ibv_qp_attr a{};
    a.qp_state = IBV_QPS_INIT;
    a.pkey_index = 0;
    a.port_num = 1;
    a.qp_access_flags = IBV_ACCESS_LOCAL_WRITE;
    int rc = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_PKEY_INDEX |
                                       IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
    if (rc != 0) { return verr("INIT", rc); }
    a = {};
    a.qp_state = IBV_QPS_RTR;
    a.path_mtu = IBV_MTU_4096;
    a.dest_qp_num = dest;
    a.rq_psn = peer.psn;
    a.ah_attr.dlid = (std::uint16_t)peer.lid;
    a.ah_attr.port_num = 1;
    a.ah_attr.is_global = 1;
    std::memcpy(a.ah_attr.grh.dgid.raw, peer.gid, 16);
    a.ah_attr.grh.sgid_index = (std::uint8_t)gid_index;
    a.ah_attr.grh.hop_limit = 1;
    rc = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
                                   IBV_QP_DEST_QPN | IBV_QP_RQ_PSN);
    if (rc != 0) { return verr("RTR", rc); }
    a = {};
    a.qp_state = IBV_QPS_RTS;
    a.sq_psn = psn;
    rc = ibv_modify_qp(qp, &a, IBV_QP_STATE | IBV_QP_SQ_PSN);
    if (rc != 0) { return verr("RTS", rc); }
    return {};
  }

  std::string
  post_slot_(int i)
  {
    ibv_sge sge{(std::uintptr_t)(ctl_mem + (std::size_t)i * kSlotBytes),
                (std::uint32_t)kSlotBytes, ctl_mr->lkey};
    ibv_recv_wr wr{};
    ibv_recv_wr* bad = nullptr;
    wr.wr_id = tag(kCtlRecv, (std::uint64_t)i);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    const int rc = ibv_post_recv(ctl, &wr, &bad);
    return rc != 0 ? verr("posting a credit's receive", rc) : std::string();
  }

  // The region holding [a, a + n), its blocks over it registered.
  //
  // A device registers a page THREE times at most -- the fourth
  // registration of any page is refused (ENOMEM), whatever its size, each
  // of a Mac's devices its own three -- and a registration let go while
  // another holds any of its pages, on this device or another, breaks
  // that one ("local protection error"). So nothing is let go before
  // vpipe's forget(), and the blocks are cut from the MAPPING the memory
  // lies in (the exchange buffer's), not from the exchange: whatever part
  // of the buffer an exchange -- a band of any size, a cable's stripe of
  // it -- goes through, it falls on the same blocks, each registered
  // once, a page held by two at most (its block's, and the piece more of
  // the one before). Registered by the exchange's range, a 1344 x 768
  // clip's bands of several sizes over four cables reached a fourth.
  const Region*
  region_(const void* p, std::size_t n, std::string* err)
  {
    const auto a = (std::uintptr_t)p;
    Region* r = nullptr;
    for (Region& x : regions) {
      if (x.base <= a && a + n <= x.base + x.len) {
        r = &x;
        break;
      }
    }
    if (r == nullptr) {
      Region x;
      mach_vm_address_t at = a;
      mach_vm_size_t size = 0;
      vm_region_basic_info_data_64_t info{};
      mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
      mach_port_t object = MACH_PORT_NULL;
      if (mach_vm_region(mach_task_self(), &at, &size,
                         VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                         &count, &object) == KERN_SUCCESS &&
          at <= a && a + n <= at + size) {
        x.base = at;
        x.len = size;
      } else {
        // Not one mapping (no host's buffer is): the exchange's pages.
        x.base = a / kPage * kPage;
        x.len = (a + n + kPage - 1) / kPage * kPage - x.base;
      }
      x.page = x.base / kPage * kPage;
      x.mrs.assign((x.base + x.len - x.page + kSpan - 1) / kSpan, nullptr);
      regions.push_back(std::move(x));
      r = &regions.back();
    }
    const std::uintptr_t end = r->base + r->len;
    for (std::size_t i = (a - r->page) / kSpan;
         i <= (a + n - 1 - r->page) / kSpan; ++i) {
      if (r->mrs[i] != nullptr) { continue; }
      const std::uintptr_t s = r->page + i * kSpan;
      const std::uintptr_t e = std::min(end, s + kSpan + kMsg);
      r->mrs[i] = ibv_reg_mr(pd, (void*)s, e - s, IBV_ACCESS_LOCAL_WRITE);
      if (r->mrs[i] == nullptr) {
        *err = name + ": cannot register " + std::to_string(e - s) +
               " bytes of the exchange's memory: " + std::strerror(errno);
        return nullptr;
      }
    }
    return r;
  }

  std::string
  send_credit_(std::uint32_t k)
  {
    return send_ctl_(Credit{seq, k, (std::uint32_t)piece_len(k)});
  }

  std::string
  send_ctl_(const Credit& c)
  {
    // A send slot is free once its send has completed: at most
    // kSendSlots outstanding.
    if (ctl_posted - ctl_done >= (std::uint64_t)kSendSlots) {
      return "too many credits in flight";
    }
    const int slot = (int)(ctl_posted % kSendSlots);
    std::uint8_t* m = ctl_mem + (std::size_t)(kSlots + slot) * kSlotBytes;
    std::memcpy(m, &c, sizeof c);
    ibv_sge sge{(std::uintptr_t)m, (std::uint32_t)sizeof c, ctl_mr->lkey};
    ibv_send_wr wr{};
    ibv_send_wr* bad = nullptr;
    wr.wr_id = tag(kCtlSend, (std::uint64_t)slot);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    const int rc = ibv_post_send(ctl, &wr, &bad);
    if (rc != 0) { return verr("sending a credit", rc); }
    ++ctl_posted;
    return {};
  }

  // Piece k's receive straight into the destination, then its credit.
  std::string
  post_piece_recv_(std::uint32_t k)
  {
    const std::uintptr_t a = (std::uintptr_t)(dst + (std::size_t)k * kMsg);
    ibv_sge sge{a, (std::uint32_t)piece_len(k), rdst->lkey(a)};
    ibv_recv_wr wr{};
    ibv_recv_wr* bad = nullptr;
    wr.wr_id = tag(kDataRecv, k);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    const int rc = ibv_post_recv(data, &wr, &bad);
    if (rc != 0) { return verr("posting a piece's receive", rc); }
    return send_credit_(k);
  }

  // Piece k sent. False, no error, when the queue pair has no room for
  // it yet: tried again at the next completion.
  bool
  post_piece_send_(std::uint32_t k, std::string* err)
  {
    const auto a = (std::uintptr_t)(src + (std::size_t)k * kMsg);
    ibv_sge sge{a, (std::uint32_t)piece_len(k), rsrc->lkey(a)};
    ibv_send_wr wr{};
    ibv_send_wr* bad = nullptr;
    wr.wr_id = tag(kDataSend, k);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    const int rc = ibv_post_send(data, &wr, &bad);
    if (rc == ENOMEM || rc == -ENOMEM) { return false; }
    if (rc != 0) { *err = verr("sending a piece", rc); }
    return rc == 0;
  }

  std::string
  begin_(std::uint64_t s, const std::uint8_t* from, std::uint8_t* to,
         std::size_t n)
  {
    seq = s;
    src = from;
    dst = to;
    len = n;
    pieces = (std::uint32_t)((n + kMsg - 1) / kMsg);
    posted = landed = sent = sends = 0;
    credits = 0;
    if (auto it = early.find(s); it != early.end()) {
      credits = it->second;
      early.erase(it);
    }
    active = true;
    if (pieces == 0) { return {}; }
    std::string e;
    rsrc = region_(from, n, &e);
    if (rsrc == nullptr) { return e; }
    rdst = region_(to, n, &e);
    if (rdst == nullptr) { return e; }
    while (posted < std::min(pieces, kWindow)) {
      e = post_piece_recv_(posted++);
      if (!e.empty()) { return e; }
    }
    return send_ready_();
  }

  // Every piece the peer has credited, sent -- kWindow at most in flight.
  std::string
  send_ready_()
  {
    std::string e;
    while (sent < credits && sent < pieces && sent - sends < kWindow) {
      if (!post_piece_send_(sent, &e)) { return e; }
      ++sent;
    }
    return {};
  }

  bool
  done_() const
  {
    return landed == pieces && sends == pieces && ctl_done == ctl_posted;
  }

  // The regions holding any of [a, a + n) deregistered and dropped.
  void
  forget_(std::uintptr_t a, std::size_t n)
  {
    for (auto it = regions.begin(); it != regions.end();) {
      if (it->base < a + n && a < it->base + it->len) {
        for (ibv_mr* m : it->mrs) {
          if (m != nullptr) { ibv_dereg_mr(m); }
        }
        it = regions.erase(it);
      } else {
        ++it;
      }
    }
  }

  // What has completed. Returns whether anything did; `err` set on a
  // failure.
  bool
  poll_(std::string* err)
  {
    ibv_wc wc[32];
    const int n = ibv_poll_cq(cq, 32, wc);
    if (n < 0) {
      *err = name + ": polling its queue failed";
      return false;
    }
    for (int i = 0; i < n && err->empty(); ++i) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        *err = name + ": " + ibv_wc_status_str(wc[i].status);
        return true;
      }
      const std::uint64_t kind = wc[i].wr_id >> 56;
      const std::uint64_t idx = wc[i].wr_id & ((std::uint64_t(1) << 56) - 1);
      if (kind == kCtlRecv) {
        Credit c{};
        std::memcpy(&c, ctl_mem + idx * kSlotBytes, sizeof c);
        *err = post_slot_((int)idx);
        if (!err->empty()) { return true; }
        if (c.seq == kHello) {
          hello = (long)c.piece;
        } else if (active && c.seq == seq) {
          if (c.piece != credits || c.len != piece_len(c.piece)) {
            *err = "the ranks are out of step (piece " +
                   std::to_string(c.piece) + " of " + std::to_string(c.len) +
                   " bytes credited)";
            return true;
          }
          ++credits;
        } else if (!active || c.seq > seq) {
          ++early[c.seq];
        } else {
          *err = "the ranks are out of step (a credit of exchange " +
                 std::to_string(c.seq) + ", in " + std::to_string(seq) + ")";
          return true;
        }
      } else if (kind == kDataRecv) {
        if (!active || idx != landed || wc[i].byte_len != piece_len(landed)) {
          *err = "a piece came in out of place (" +
                 std::to_string(wc[i].byte_len) + " bytes)";
          return true;
        }
        ++landed;
        if (posted < pieces) { *err = post_piece_recv_(posted++); }
      } else if (kind == kCtlSend) {
        ++ctl_done;
      } else if (kind == kDataSend) {
        ++sends;
      }
    }
    if (err->empty() && active) { *err = send_ready_(); }
    return n > 0;
  }
};

}

struct RdmaPath::Impl {
  std::vector<std::unique_ptr<Link>> links;
  int fd = -1;
  int wait_ms = 0;
  std::vector<int> weights;   // one a link, or empty: even

  // THE LINKS LET GO TOGETHER: a device's close takes ~100 ms (its
  // queue pairs, registrations, the device itself), and a split lets go
  // of a channel between its denoise and its decode -- one at a time,
  // four cables were 0.4 s of every clip.
  ~Impl()
  {
    std::vector<std::thread> ts;
    for (auto& l : links) {
      ts.emplace_back([p = l.release()]() { delete p; });
    }
    for (auto& t : ts) { t.join(); }
  }
};

namespace {

// Where each link's stripe of `bytes` starts, and its end last: by the
// links' weights -- whole 4 KB pages but the last stripe, so a stripe
// starts on a page -- else evenly. Integer arithmetic: both ranks cut
// alike.
std::vector<std::size_t>
stripes(std::size_t bytes, std::size_t n, const std::vector<int>& w)
{
  std::vector<std::size_t> at(n + 1, 0);
  long long total = 0;
  for (int x : w) { total += x > 0 ? x : 0; }
  const bool weighed = w.size() == n && total > 0;
  std::size_t off = 0;
  for (std::size_t i = 0; i < n; ++i) {
    at[i] = off;
    std::size_t len;
    if (i + 1 == n) {
      len = bytes - off;
    } else if (weighed) {
      const unsigned __int128 share =
          static_cast<unsigned __int128>(bytes) *
          static_cast<unsigned long long>(std::max(w[i], 0)) /
          static_cast<unsigned long long>(total);
      len = static_cast<std::size_t>(share) / 4096 * 4096;
    } else {
      len = (bytes + n - 1) / n;
    }
    off = std::min(bytes, off + len);
  }
  at[n] = bytes;
  return at;
}

}  // namespace

bool
rdma_available()
{
  // Weakly linked: absent on a macOS without it, every call null.
  return ::dlsym(RTLD_DEFAULT, "ibv_get_device_list") != nullptr;
}

std::vector<std::string>
rdma_active_devices()
{
  std::vector<std::string> out;
  if (!rdma_available()) { return out; }
  int n = 0;
  ibv_device** list = ibv_get_device_list(&n);
  for (int i = 0; i < n && list != nullptr; ++i) {
    ibv_context* c = ibv_open_device(list[i]);
    ibv_port_attr pa{};
    if (c != nullptr && ibv_query_port(c, 1, &pa) == 0 &&
        pa.state == IBV_PORT_ACTIVE) {
      out.emplace_back(ibv_get_device_name(list[i]));
    }
    if (c != nullptr) { ibv_close_device(c); }
  }
  if (list != nullptr) { ibv_free_device_list(list); }
  std::sort(out.begin(), out.end());
  return out;
}

RdmaPath::RdmaPath(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

RdmaPath::~RdmaPath() = default;

std::unique_ptr<RdmaPath>
RdmaPath::open(const std::vector<std::string>& devices, int fd,
               int gid_index, int wait_ms, std::string* err,
               std::vector<int> weights)
{
  auto fail = [&](const std::string& why) {
    if (err != nullptr) { *err = why; }
    return std::unique_ptr<RdmaPath>();
  };
  if (!rdma_available()) {
    return fail("this macOS has no RDMA (26.2 and later, enabled with "
                "rdma_ctl from recoveryOS)");
  }
  if (devices.empty()) { return fail("no RDMA device named"); }
  auto impl = std::make_unique<Impl>();
  impl->fd = fd;
  impl->wait_ms = wait_ms;
  if (weights.size() == devices.size()) {
    impl->weights = std::move(weights);
  }
  std::vector<Info> mine;
  for (const std::string& d : devices) {
    auto l = std::make_unique<Link>();
    l->name = d;
    const std::string e = l->open_(gid_index);
    if (!e.empty()) { return fail(e); }
    Info in{};
    in.lid = l->lid;
    in.ctl_qpn = l->ctl->qp_num;
    in.data_qpn = l->data->qp_num;
    in.psn = arc4random() & 0xffffff;
    std::memcpy(in.gid, l->gid.raw, 16);
    mine.push_back(in);
    impl->links.push_back(std::move(l));
  }
  // The queue pairs' names, over the proved link: the counts first.
  const auto count = (std::uint32_t)mine.size();
  std::uint32_t theirs = 0;
  if (!send_all(fd, &count, sizeof count) ||
      !recv_all(fd, &theirs, sizeof theirs)) {
    return fail("the link was cut swapping the queue pairs");
  }
  if (theirs != count) {
    return fail("the peer has " + std::to_string(theirs) + " RDMA links, " +
                "this rank " + std::to_string(count));
  }
  std::vector<Info> peer(count);
  if (!send_all(fd, mine.data(), sizeof(Info) * count) ||
      !recv_all(fd, peer.data(), sizeof(Info) * count)) {
    return fail("the link was cut swapping the queue pairs");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    Link& l = *impl->links[i];
    std::string e = l.bring_up_(l.ctl, peer[i].ctl_qpn, peer[i],
                                mine[i].psn, gid_index);
    if (e.empty()) {
      e = l.bring_up_(l.data, peer[i].data_qpn, peer[i], mine[i].psn,
                      gid_index);
    }
    for (int s = 0; s < kSlots && e.empty(); ++s) { e = l.post_slot_(s); }
    if (!e.empty()) { return fail(l.name + ": " + e); }
  }
  // Both sides' queue pairs up and their credits' receives posted before
  // either sends: UC drops what lands on neither.
  char b = 'R';
  if (!send_all(fd, &b, 1) || !recv_all(fd, &b, 1)) {
    return fail("the link was cut bringing the queue pairs up");
  }
  // A HELLO ON EACH LINK, its place in the list: a cable down, or two
  // crossed -- the devices paired otherwise than they are cabled -- shows
  // here, by name, not as the first exchange stalled.
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::string e = impl->links[i]->send_ctl_(Credit{kHello, i, 0});
    if (!e.empty()) { return fail(impl->links[i]->name + ": " + e); }
  }
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(3000);
  for (;;) {
    bool all = true;
    for (std::uint32_t i = 0; i < count; ++i) {
      Link& l = *impl->links[i];
      std::string e;
      l.poll_(&e);
      if (!e.empty()) { return fail(l.name + ": " + e); }
      if (l.hello >= 0 && l.hello != (long)i) {
        return fail(l.name + " reaches the peer's RDMA link " +
                    std::to_string(l.hello + 1) + ", not its " +
                    std::to_string(i + 1) + ": name the devices in the " +
                    "order the cables pair them");
      }
      all = all && l.hello == (long)i && l.ctl_done == l.ctl_posted;
    }
    if (all) { break; }
    if (std::chrono::steady_clock::now() > until) {
      for (std::uint32_t i = 0; i < count; ++i) {
        if (impl->links[i]->hello < 0) {
          return fail(impl->links[i]->name + " hears nothing from the " +
                      "peer's RDMA link " + std::to_string(i + 1) +
                      ": its cable is down, or the devices are not paired " +
                      "as they are cabled");
        }
      }
      return fail("the RDMA links' hellos were not answered");
    }
  }
  return std::unique_ptr<RdmaPath>(new RdmaPath(std::move(impl)));
}

bool
RdmaPath::exchange(const void* partial, void* others, std::size_t bytes,
                   std::uint64_t seq, const std::function<bool()>& stopped,
                   std::string* err)
{
  auto fail = [&](const std::string& why) {
    if (err != nullptr) { *err = why; }
    for (auto& l : _impl->links) { l->active = false; }
    return false;
  };
  auto& links = _impl->links;
  const std::size_t n = links.size();
  const std::vector<std::size_t> at = stripes(bytes, n, _impl->weights);
  for (std::size_t i = 0; i < n; ++i) {
    const std::size_t off = at[i];
    const std::size_t len = at[i + 1] - at[i];
    const std::string e = links[i]->begin_(
        seq, static_cast<const std::uint8_t*>(partial) + off,
        static_cast<std::uint8_t*>(others) + off, len);
    if (!e.empty()) { return fail(e); }
  }
  // Spun: the GPU waits on this exchange, or the next is posted behind it.
  using Clock = std::chrono::steady_clock;
  auto moved = Clock::now();
  auto checked = moved;
  for (;;) {
    bool all = true, any = false;
    for (auto& l : links) {
      std::string e;
      any = l->poll_(&e) || any;
      if (!e.empty()) { return fail(e); }
      all = all && l->done_();
    }
    if (all) { break; }
    const auto now = Clock::now();
    if (any) { moved = now; }
    if (now - checked < std::chrono::milliseconds(50)) { continue; }
    checked = now;
    if (stopped && stopped()) { return fail("stopped"); }
    // The peer gone shows on the TCP link it was set up over, which
    // carries nothing else.
    pollfd p{_impl->fd, POLLIN, 0};
    if (::poll(&p, 1, 0) > 0) {
      char c;
      if (::recv(_impl->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT) <= 0) {
        return fail("the peer went away");
      }
    }
    if (now - moved > std::chrono::milliseconds(_impl->wait_ms)) {
      return fail("the RDMA link stalled in exchange " +
                  std::to_string(seq));
    }
  }
  for (auto& l : links) { l->active = false; }
  return true;
}

void
RdmaPath::forget(const void* base, std::size_t bytes)
{
  if (base == nullptr || bytes == 0) { return; }
  for (auto& l : _impl->links) { l->forget_((std::uintptr_t)base, bytes); }
}

}
