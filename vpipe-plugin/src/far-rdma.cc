#include "valtz-vpipe/far-rdma.h"

#include "valtz-vpipe/far-wire.h"

#include <infiniband/verbs.h>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>
#include <thread>

namespace valtz::far_rdma {

namespace {

namespace fw = valtz::far_wire;

// How long a lender's thread sleeps between looks when it has nothing out
// on the cable (VALTZ_FAR_LEND_IDLE_US). A thread that keeps a performance
// core busy -- spinning, or waking every 20 us -- slows this Mac's GPU
// about threefold (the far experts it runs: 3 FP4 experts' kernels 475 us
// against 131, M5 Max, tests/fleet-test.cc far_compute_bench; one such
// thread as much as four, at any QoS above background); waking every 50
// us does not.
int
lend_idle_us()
{
  static const int us = [] {
    const char* v = std::getenv("VALTZ_FAR_LEND_IDLE_US");
    return v != nullptr ? std::max(0, std::atoi(v)) : 50;
  }();
  return us;
}

constexpr std::size_t kPage = 16384;
// Three pieces in flight a way: a queue pair's receives (and sends) stay
// under its 4,096,000 bytes.
constexpr std::uint32_t kWindow = 3;
constexpr int kSlots = 64;       // control receives posted
constexpr int kSendSlots = 64;   // control sends' slots
constexpr std::size_t kSlotBytes = 32;
constexpr int kGid = 1;          // the port's link-local GID

enum : std::uint64_t {
  kCtlRecv = 1,
  kDataRecv = 2,
  kCtlSend = 3,
  kDataSend = 4,
  kCallRecv = 5,
  kCallSend = 6,
};
// CALLS (kCompute over RDMA; PROTOTYPE): a queue pair of their own on
// each link, its own completion queue -- the caller polls it, apart from
// the link's reads and writes -- a request always kCallBytes (a message
// must fill the receive it lands in), its answer the length the user
// posted for it. The lender keeps kCallRecvs posted; the user has one
// call out at a time. 32 KB: an answer of two of DeepSeek's 10 KB rows.
constexpr std::size_t kCallBytes = 32768;
constexpr int kCallRecvs = 2;

std::uint64_t
tag_of(std::uint64_t kind, std::uint64_t i)
{
  return (kind << 56) | i;
}

// A control message.
enum : std::uint32_t {
  kMsgRead = 1,
  kMsgWrite = 2,
  kMsgAck = 3,
  kMsgHello = 4,
  kMsgRefused = 5,
};
struct Msg {
  std::uint32_t op;
  std::uint32_t len;
  std::uint64_t offset;
  std::uint64_t id;
  std::uint64_t pad;
};
static_assert(sizeof(Msg) <= kSlotBytes);

// A link's queue pairs, as the other end needs them.
struct Info {
  std::uint32_t lid;
  std::uint32_t ctl_qpn;
  std::uint32_t data_qpn;
  std::uint32_t psn;
  std::uint8_t gid[16];
  std::uint32_t call_qpn;
  std::uint32_t pad;
};
// The user's ask for a link: the lender's device on that cable.
struct Ask {
  char device[32];
  Info info;
};
struct Answer {
  std::uint32_t ok;
  std::uint32_t pad;
  Info info;
  char why[96];
};

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

std::string
verr(const char* what, int rc)
{
  return std::string(what) + ": " + std::strerror(rc < 0 ? -rc : rc);
}

struct Dev {
  std::string name;
  ibv_context* ctx = nullptr;
  ibv_pd* pd = nullptr;
  ibv_cq* cq = nullptr;
  ibv_qp* ctl = nullptr;
  ibv_qp* data = nullptr;
  // calls: [kCallRecvs receives][one send] of kCallBytes
  ibv_cq* call_cq = nullptr;
  ibv_qp* call = nullptr;
  std::uint8_t* call_mem = nullptr;
  ibv_mr* call_mr = nullptr;
  std::uint8_t* ctl_mem = nullptr;   // kSlots receive, kSendSlots send
  ibv_mr* ctl_mr = nullptr;
  // STAGING, registered once: kWindow receive slots, then kWindow send
  // slots, a piece each. A device registers 100 regions of 16,384,000
  // bytes at most (M5, macOS 27.2) -- 1.6 GB, not the lent memory nor an
  // expert cache -- so every piece is copied through these.
  std::uint8_t* stage = nullptr;
  ibv_mr* stage_mr = nullptr;
  std::uint16_t lid = 0;
  ibv_gid gid{};
  std::uint32_t psn = 0;
  std::uint64_t ctl_posted = 0, ctl_done = 0;

  ~Dev()
  {
    // Reset before destroyed: a live queue pair drains for ~50-250 ms
    // (tp-rdma.cc).
    for (ibv_qp* q : {call, data, ctl}) {
      if (q == nullptr) { continue; }
      ibv_qp_attr a{};
      a.qp_state = IBV_QPS_RESET;
      ibv_modify_qp(q, &a, IBV_QP_STATE);
    }
    if (ctl_mr != nullptr) { ibv_dereg_mr(ctl_mr); }
    if (call_mr != nullptr) { ibv_dereg_mr(call_mr); }
    if (call != nullptr) { ibv_destroy_qp(call); }
    if (call_cq != nullptr) { ibv_destroy_cq(call_cq); }
    std::free(call_mem);
    if (stage_mr != nullptr) { ibv_dereg_mr(stage_mr); }
    if (data != nullptr) { ibv_destroy_qp(data); }
    if (ctl != nullptr) { ibv_destroy_qp(ctl); }
    if (cq != nullptr) { ibv_destroy_cq(cq); }
    std::free(ctl_mem);
    std::free(stage);
    if (pd != nullptr) { ibv_dealloc_pd(pd); }
    if (ctx != nullptr) { ibv_close_device(ctx); }
  }

  std::string
  open(const std::string& device)
  {
    name = device;
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
    if (da.max_mr_size < 2 * kWindow * kPiece || pa.max_msg_sz < kPiece) {
      return name + ": its regions or messages are too small";
    }
    lid = pa.lid;
    if (ibv_query_gid(ctx, 1, kGid, &gid) != 0) {
      return name + ": no GID " + std::to_string(kGid);
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
    call_cq = ibv_create_cq(ctx, 64, nullptr, nullptr, 0);
    if (call_cq == nullptr) { return name + ": no queue for calls"; }
    ibv_qp_init_attr ca = qa;
    ca.send_cq = call_cq;
    ca.recv_cq = call_cq;
    ca.cap.max_send_wr = 16;
    ca.cap.max_recv_wr = 16;
    call = ibv_create_qp(pd, &ca);
    if (call == nullptr) { return name + ": no queue pair for calls"; }
    const std::size_t clb = (std::size_t)(kCallRecvs + 1) * kCallBytes;
    if (::posix_memalign((void**)&call_mem, kPage, clb) != 0) {
      call_mem = nullptr;
      return name + ": no memory";
    }
    std::memset(call_mem, 0, clb);
    call_mr = ibv_reg_mr(pd, call_mem, clb, IBV_ACCESS_LOCAL_WRITE);
    if (call_mr == nullptr) { return name + ": cannot register memory"; }
    const std::size_t cb = (std::size_t)(kSlots + kSendSlots) * kSlotBytes;
    if (::posix_memalign((void**)&ctl_mem, kPage, cb) != 0) {
      ctl_mem = nullptr;
      return name + ": no memory";
    }
    std::memset(ctl_mem, 0, cb);
    ctl_mr = ibv_reg_mr(pd, ctl_mem, cb, IBV_ACCESS_LOCAL_WRITE);
    const std::size_t sb = 2 * kWindow * kPiece;
    if (::posix_memalign((void**)&stage, kPage, sb) != 0) {
      stage = nullptr;
      return name + ": no memory";
    }
    stage_mr = ibv_reg_mr(pd, stage, sb, IBV_ACCESS_LOCAL_WRITE);
    if (ctl_mr == nullptr || stage_mr == nullptr) {
      return name + ": cannot register memory";
    }
    psn = arc4random() & 0xffffff;
    return {};
  }

  Info
  info() const
  {
    Info in{};
    in.lid = lid;
    in.ctl_qpn = ctl->qp_num;
    in.data_qpn = data->qp_num;
    in.call_qpn = call->qp_num;
    in.psn = psn;
    std::memcpy(in.gid, gid.raw, 16);
    return in;
  }

  // INIT, RTR toward the peer's queue pair `dest`, RTS.
  std::string
  bring_up_(ibv_qp* qp, std::uint32_t dest, const Info& peer)
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
    a.ah_attr.grh.sgid_index = (std::uint8_t)kGid;
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

  // Both queue pairs up toward the peer's, the control receives posted.
  std::string
  connect(const Info& peer)
  {
    std::string e = bring_up_(ctl, peer.ctl_qpn, peer);
    if (e.empty()) { e = bring_up_(data, peer.data_qpn, peer); }
    if (e.empty()) { e = bring_up_(call, peer.call_qpn, peer); }
    for (int s = 0; s < kSlots && e.empty(); ++s) { e = post_slot(s); }
    return e.empty() ? e : name + ": " + e;
  }

  std::string
  post_slot(int i)
  {
    ibv_sge sge{(std::uintptr_t)(ctl_mem + (std::size_t)i * kSlotBytes),
                (std::uint32_t)kSlotBytes, ctl_mr->lkey};
    ibv_recv_wr wr{};
    ibv_recv_wr* bad = nullptr;
    wr.wr_id = tag_of(kCtlRecv, (std::uint64_t)i);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    const int rc = ibv_post_recv(ctl, &wr, &bad);
    return rc != 0 ? verr("posting a control receive", rc) : std::string();
  }

  bool ctl_room() const
  {
    return ctl_posted - ctl_done < (std::uint64_t)kSendSlots;
  }

  std::string
  send_msg(const Msg& m)
  {
    if (!ctl_room()) { return name + ": too many control messages out"; }
    const int slot = (int)(ctl_posted % kSendSlots);
    std::uint8_t* p = ctl_mem + (std::size_t)(kSlots + slot) * kSlotBytes;
    std::memcpy(p, &m, sizeof m);
    ibv_sge sge{(std::uintptr_t)p, (std::uint32_t)sizeof m, ctl_mr->lkey};
    ibv_send_wr wr{};
    ibv_send_wr* bad = nullptr;
    wr.wr_id = tag_of(kCtlSend, (std::uint64_t)slot);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    const int rc = ibv_post_send(ctl, &wr, &bad);
    if (rc != 0) { return name + ": " + verr("sending a message", rc); }
    ++ctl_posted;
    return {};
  }

  std::uint8_t* call_recv_slot(int i)
  {
    return call_mem + (std::size_t)i * kCallBytes;
  }
  std::uint8_t* call_send_slot()
  {
    return call_mem + (std::size_t)kCallRecvs * kCallBytes;
  }
  std::string
  post_call_recv(int i, std::size_t n)
  {
    ibv_sge sge{(std::uintptr_t)call_recv_slot(i), (std::uint32_t)n,
                call_mr->lkey};
    ibv_recv_wr wr{};
    ibv_recv_wr* bad = nullptr;
    wr.wr_id = tag_of(kCallRecv, (std::uint64_t)i);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    const int rc = ibv_post_recv(call, &wr, &bad);
    return rc != 0 ? name + ": " + verr("posting a call's receive", rc)
                   : std::string();
  }
  std::string
  post_call_send(std::size_t n)
  {
    ibv_sge sge{(std::uintptr_t)call_send_slot(), (std::uint32_t)n,
                call_mr->lkey};
    ibv_send_wr wr{};
    ibv_send_wr* bad = nullptr;
    wr.wr_id = tag_of(kCallSend, 0);
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    const int rc = ibv_post_send(call, &wr, &bad);
    return rc != 0 ? name + ": " + verr("sending a call", rc)
                   : std::string();
  }

  std::uint8_t* recv_slot(std::uint64_t i)
  {
    return stage + (i % kWindow) * kPiece;
  }
  std::uint8_t* send_slot(std::uint64_t i)
  {
    return stage + (kWindow + i % kWindow) * kPiece;
  }

  // A data receive into receive slot `i` (kWindow apart: posted in
  // order, completed in order).
  std::string
  post_recv(std::uint64_t wr_id, std::uint64_t i, std::size_t n)
  {
    ibv_sge sge{(std::uintptr_t)recv_slot(i), (std::uint32_t)n,
                stage_mr->lkey};
    ibv_recv_wr wr{};
    ibv_recv_wr* bad = nullptr;
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    const int rc = ibv_post_recv(data, &wr, &bad);
    return rc != 0 ? name + ": " + verr("posting a piece's receive", rc)
                   : std::string();
  }

  // Send slot `i`'s first `n` bytes: free once kWindow sends before it
  // have completed.
  std::string
  post_send(std::uint64_t wr_id, std::uint64_t i, std::size_t n)
  {
    ibv_sge sge{(std::uintptr_t)send_slot(i), (std::uint32_t)n,
                stage_mr->lkey};
    ibv_send_wr wr{};
    ibv_send_wr* bad = nullptr;
    wr.wr_id = wr_id;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    const int rc = ibv_post_send(data, &wr, &bad);
    return rc != 0 ? name + ": " + verr("sending a piece", rc)
                   : std::string();
  }
};

}  // namespace

bool
available()
{
  // Weakly linked: absent on a macOS without it, every call null.
  return ::dlsym(RTLD_DEFAULT, "ibv_get_device_list") != nullptr;
}

// ---- the user's end -----------------------------------------------------

struct UserLink::Impl {
  Dev dev;
  struct Out {
    std::uint64_t tag;
    std::uint32_t len;
    std::uint64_t id;
    void* dst;          // a read's: where its slot is copied
  };
  std::deque<Out> reads;    // receives posted, in the order asked
  std::deque<Out> writes;   // sent, waiting for their acks
  std::uint32_t credits = kWindow;
  std::uint32_t data_sends = 0;   // pieces sent, completions not reaped
  std::uint64_t next_id = 1;
  std::uint64_t recvs = 0, sends = 0;   // slots used, in order
  bool hello = false;
};

UserLink::UserLink(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

UserLink::~UserLink() = default;

const std::string&
UserLink::name() const
{
  return _impl->dev.name;
}

bool
UserLink::can_read() const
{
  return _impl->reads.size() < kWindow && _impl->dev.ctl_room();
}

bool
UserLink::can_write() const
{
  return _impl->credits > 0 && _impl->data_sends < kWindow &&
         _impl->dev.ctl_room();
}

bool
UserLink::idle() const
{
  return _impl->reads.empty() && _impl->writes.empty();
}

std::string
UserLink::read(std::uint64_t offset, void* dst, std::size_t n,
               std::uint64_t tag)
{
  Impl& m = *_impl;
  if (n == 0 || n > kPiece) { return "a piece of " + std::to_string(n); }
  // The receive first: what answers lands in the next one posted.
  std::string e = m.dev.post_recv(tag_of(kDataRecv, 0), m.recvs++, n);
  if (!e.empty()) { return e; }
  const std::uint64_t id = m.next_id++;
  m.reads.push_back({tag, (std::uint32_t)n, id, dst});
  return m.dev.send_msg(Msg{kMsgRead, (std::uint32_t)n, offset, id, 0});
}

std::string
UserLink::write(std::uint64_t offset, const void* src, std::size_t n,
                std::uint64_t tag)
{
  Impl& m = *_impl;
  if (n == 0 || n > kPiece) { return "a piece of " + std::to_string(n); }
  const std::uint64_t id = m.next_id++;
  std::string e =
      m.dev.send_msg(Msg{kMsgWrite, (std::uint32_t)n, offset, id, 0});
  if (!e.empty()) { return e; }
  // A WHOLE slot sent, whatever `n`: a message must fill the receive it
  // lands in (a shorter one fails it, "local length error"), and the
  // lender's are posted before it knows the length. Its header says it.
  std::memcpy(m.dev.send_slot(m.sends), src, n);
  e = m.dev.post_send(tag_of(kDataSend, 0), m.sends++, kPiece);
  if (!e.empty()) { return e; }
  --m.credits;
  ++m.data_sends;
  m.writes.push_back({tag, (std::uint32_t)n, id, nullptr});
  return {};
}

std::string
UserLink::call(const void* req, std::size_t n, void* reply,
               std::size_t reply_n, int wait_ms)
{
  Dev& d = _impl->dev;
  if (n == 0 || n > kCallBytes || reply_n == 0 || reply_n > kCallBytes) {
    return "a call of " + std::to_string(n) + " bytes";
  }
  // its answer's receive first, then the request -- a whole slot
  std::string e = d.post_call_recv(0, reply_n);
  if (!e.empty()) { return e; }
  std::memcpy(d.call_send_slot(), req, n);
  std::memset(d.call_send_slot() + n, 0, kCallBytes - n);
  e = d.post_call_send(kCallBytes);
  if (!e.empty()) { return e; }
  bool sent = false, answered = false;
  const auto until = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(std::max(wait_ms, 100));
  std::uint32_t spins = 0;
  while (!sent || !answered) {
    ibv_wc wc[4];
    const int k = ibv_poll_cq(d.call_cq, 4, wc);
    if (k < 0) { return d.name + ": polling its calls failed"; }
    for (int i = 0; i < k; ++i) {
      if (wc[i].status != IBV_WC_SUCCESS) {
        return d.name + ": a call: " + ibv_wc_status_str(wc[i].status);
      }
      const std::uint64_t kind = wc[i].wr_id >> 56;
      if (kind == kCallSend) {
        sent = true;
      } else if (kind == kCallRecv) {
        if (wc[i].byte_len != reply_n) {
          return d.name + ": a call's answer out of place";
        }
        answered = true;
      }
    }
    if (k == 0 && (++spins & 1023) == 0 &&
        std::chrono::steady_clock::now() > until) {
      // the receive left posted would take a late answer; the link is
      // not to be trusted for calls after this
      return d.name + ": the lender did not answer a call";
    }
  }
  std::memcpy(reply, d.call_recv_slot(0), reply_n);
  return {};
}

bool
UserLink::poll(std::vector<std::uint64_t>* done, std::string* err)
{
  Impl& m = *_impl;
  Dev& d = m.dev;
  ibv_wc wc[32];
  const int n = ibv_poll_cq(d.cq, 32, wc);
  if (n < 0) {
    *err = d.name + ": polling its queue failed";
    return false;
  }
  for (int i = 0; i < n; ++i) {
    if (wc[i].status != IBV_WC_SUCCESS) {
      *err = d.name + ": " + ibv_wc_status_str(wc[i].status);
      return true;
    }
    const std::uint64_t kind = wc[i].wr_id >> 56;
    const std::uint64_t idx = wc[i].wr_id & ((std::uint64_t(1) << 56) - 1);
    if (kind == kDataRecv) {
      if (m.reads.empty() || wc[i].byte_len != m.reads.front().len) {
        *err = d.name + ": a piece came in out of place (" +
               std::to_string(wc[i].byte_len) + " bytes)";
        return true;
      }
      const Impl::Out& o = m.reads.front();
      std::memcpy(o.dst, d.recv_slot(m.recvs - m.reads.size()), o.len);
      done->push_back(o.tag);
      m.reads.pop_front();
    } else if (kind == kCtlRecv) {
      Msg g{};
      std::memcpy(&g, d.ctl_mem + idx * kSlotBytes, sizeof g);
      *err = d.post_slot((int)idx);
      if (!err->empty()) { return true; }
      if (g.op == kMsgAck) {
        if (m.writes.empty() || m.writes.front().id != g.id) {
          *err = d.name + ": an ack out of place";
          return true;
        }
        done->push_back(m.writes.front().tag);
        m.writes.pop_front();
        ++m.credits;
      } else if (g.op == kMsgHello) {
        m.hello = true;
      } else if (g.op == kMsgRefused) {
        *err = d.name + ": the lender refused a piece (" +
               std::to_string(g.len) + " bytes at " +
               std::to_string(g.offset) + ")";
        return true;
      } else {
        *err = d.name + ": a message of the wrong kind";
        return true;
      }
    } else if (kind == kCtlSend) {
      ++d.ctl_done;
    } else if (kind == kDataSend) {
      --m.data_sends;
    }
  }
  return n > 0;
}

std::vector<std::unique_ptr<UserLink>>
connect(int fd, const std::vector<std::string>& devices,
        const std::vector<std::string>& peer_devices, std::string* err)
{
  std::vector<std::unique_ptr<UserLink>> out;
  auto fail = [&](const std::string& why) {
    if (err != nullptr) { *err = why; }
    out.clear();
    return std::vector<std::unique_ptr<UserLink>>();
  };
  if (!available()) {
    return fail("this macOS has no RDMA (26.2 and later, enabled with "
                "rdma_ctl from recoveryOS)");
  }
  if (devices.empty() || devices.size() != peer_devices.size()) {
    return fail("RDMA devices not named in pairs");
  }
  for (const std::string& d : devices) {
    auto impl = std::make_unique<UserLink::Impl>();
    const std::string e = impl->dev.open(d);
    if (!e.empty()) { return fail(e); }
    out.push_back(std::unique_ptr<UserLink>(new UserLink(std::move(impl))));
  }
  const auto count = (std::uint32_t)out.size();
  const fw::Request r{fw::kRdma, 0, count, 0};
  std::vector<Ask> asks(count);
  for (std::uint32_t i = 0; i < count; ++i) {
    std::snprintf(asks[i].device, sizeof asks[i].device, "%s",
                  peer_devices[i].c_str());
    asks[i].info = out[i]->_impl->dev.info();
  }
  std::vector<Answer> ans(count);
  if (!send_all(fd, &r, sizeof r) ||
      !send_all(fd, asks.data(), sizeof(Ask) * count) ||
      !recv_all(fd, ans.data(), sizeof(Answer) * count)) {
    return fail("the lender's connection was cut setting up RDMA");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    if (ans[i].ok == 0) {
      ans[i].why[sizeof ans[i].why - 1] = 0;
      return fail(std::string("the lender: ") + ans[i].why);
    }
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::string e = out[i]->_impl->dev.connect(ans[i].info);
    if (!e.empty()) { return fail(e); }
  }
  // Both ends up and their receives posted before either sends: UC drops
  // what lands on nothing.
  char b = 'R';
  if (!send_all(fd, &b, 1) || !recv_all(fd, &b, 1)) {
    return fail("the lender's connection was cut bringing RDMA up");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::string e =
        out[i]->_impl->dev.send_msg(Msg{kMsgHello, 0, i, 0, 0});
    if (!e.empty()) { return fail(e); }
  }
  const auto until =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(3000);
  for (;;) {
    bool all = true;
    for (auto& l : out) {
      std::vector<std::uint64_t> none;
      std::string e;
      l->poll(&none, &e);
      if (!e.empty()) { return fail(e); }
      all = all && l->_impl->hello &&
            l->_impl->dev.ctl_done == l->_impl->dev.ctl_posted;
    }
    if (all) { break; }
    if (std::chrono::steady_clock::now() > until) {
      for (auto& l : out) {
        if (!l->_impl->hello) {
          return fail(l->name() + " hears nothing from the lender's " +
                      "device: its cable is down, or the devices are not " +
                      "paired as they are cabled");
        }
      }
      return fail("the RDMA links' hellos were not answered");
    }
  }
  return out;
}

// ---- the lender's end ---------------------------------------------------

namespace {

// The lender's files (lend_files): the root, and each opened, by id.
struct LentFile {
  int fd = -1;
  std::uint64_t size = 0;
};
std::mutex g_files_mu;
std::string g_files_root;
std::vector<LentFile> g_files;

bool
lent_file_fits(std::uint64_t id, std::uint64_t pos, std::uint64_t n)
{
  std::lock_guard<std::mutex> lk(g_files_mu);
  return id < g_files.size() && pos <= g_files[id].size &&
         n <= g_files[id].size - pos;
}

bool
read_lent_file(std::uint64_t id, std::uint64_t pos, void* dst,
               std::size_t n)
{
  int fd = -1;
  {
    std::lock_guard<std::mutex> lk(g_files_mu);
    if (id >= g_files.size()) { return false; }
    fd = g_files[id].fd;
  }
  auto* d = static_cast<std::uint8_t*>(dst);
  std::size_t got = 0;
  while (got < n) {
    const ssize_t k = ::pread(fd, d + got, n - got, (off_t)(pos + got));
    if (k <= 0) { return false; }
    got += (std::size_t)k;
  }
  return true;
}

struct LendLink {
  Dev dev;
  std::uint8_t* mem = nullptr;
  std::uint64_t bytes = 0;
  std::atomic<std::uint64_t>* read = nullptr;
  std::atomic<std::uint64_t>* written = nullptr;
  int fd = -1;
  std::uint64_t recvs = 0, sends_used = 0;   // slots used, in order
  std::deque<Msg> to_send;      // reads asked, not yet sent
  std::uint32_t sends = 0;      // reads sent, completions not reaped
  std::deque<Msg> headers;      // writes told, their bytes not paired
  struct Landed {
    std::uint64_t idx;
    std::uint32_t len;
  };
  std::deque<Landed> landed;    // staged bytes, their header not paired
  std::atomic<bool> stop{false};
  std::thread thread;
  // calls (link 0's): their own thread, spinning on their queue
  Lender::Call on_call;
  std::thread call_thread;
  std::vector<std::uint8_t> answer;

  void
  run_calls()
  {
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    std::uint32_t sends = 0;
    while (!stop.load(std::memory_order_relaxed)) {
      ibv_wc wc[4];
      const int k = ibv_poll_cq(dev.call_cq, 4, wc);
      std::string e;
      if (k < 0) { e = dev.name + ": polling its calls failed"; }
      for (int i = 0; i < k && e.empty(); ++i) {
        if (wc[i].status != IBV_WC_SUCCESS) {
          e = dev.name + ": a call: " + ibv_wc_status_str(wc[i].status);
          break;
        }
        const std::uint64_t kind = wc[i].wr_id >> 56;
        const int idx = (int)(wc[i].wr_id & 0xff);
        if (kind == kCallSend) {
          --sends;
          continue;
        }
        if (kind != kCallRecv) { continue; }
        // the answer: its length the caller's, whatever it says
        answer.clear();
        on_call(dev.call_recv_slot(idx), wc[i].byte_len, &answer);
        e = dev.post_call_recv(idx, kCallBytes);
        if (e.empty() && !answer.empty() && answer.size() <= kCallBytes) {
          while (sends > 0 && e.empty()) {   // the one send slot
            ibv_wc w2;
            const int j = ibv_poll_cq(dev.call_cq, 1, &w2);
            if (j < 0) { e = dev.name + ": polling its calls failed"; }
            if (j == 1 && (w2.wr_id >> 56) == kCallSend) { --sends; }
            if (j == 1 && (w2.wr_id >> 56) == kCallRecv) {
              e = dev.name + ": two calls out at once";
            }
          }
          if (e.empty()) {
            std::memcpy(dev.call_send_slot(), answer.data(), answer.size());
            e = dev.post_call_send(answer.size());
            ++sends;
          }
        }
      }
      if (!e.empty()) {
        std::fprintf(stderr, "valtz far lender: %s\n", e.c_str());
        ::shutdown(fd, SHUT_RDWR);
        return;
      }
      // between calls a look every lend_idle_us, not a spin: a busy core
      // here would slow the GPU the calls run on
      if (sends == 0) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(lend_idle_us()));
      }
    }
  }


  bool
  in_range(const Msg& g) const
  {
    if (fw::is_file(g.offset)) {
      return g.len > 0 && g.len <= kPiece &&
             lent_file_fits(fw::file_id(g.offset),
                            fw::file_pos(g.offset), g.len);
    }
    return g.len > 0 && g.len <= kPiece && g.offset <= bytes &&
           g.len <= bytes - g.offset;
  }

  // One pass over what completed and what can go now. Whether anything
  // moved; `err` on a failure.
  bool
  step(std::string* err)
  {
    ibv_wc wc[32];
    const int n = ibv_poll_cq(dev.cq, 32, wc);
    if (n < 0) {
      *err = dev.name + ": polling its queue failed";
      return false;
    }
    for (int i = 0; i < n; ++i) {
      const std::uint64_t kind = wc[i].wr_id >> 56;
      const std::uint64_t idx = wc[i].wr_id & ((std::uint64_t(1) << 56) - 1);
      if (wc[i].status != IBV_WC_SUCCESS) {
        static const char* kinds[] = {"?", "a control receive",
                                      "a piece's receive", "a control send",
                                      "a piece's send"};
        *err = dev.name + ": " + ibv_wc_status_str(wc[i].status) + " (" +
               kinds[kind < 5 ? kind : 0] + ", " +
               std::to_string(wc[i].byte_len) + " bytes)";
        return true;
      }
      if (kind == kCtlRecv) {
        Msg g{};
        std::memcpy(&g, dev.ctl_mem + idx * kSlotBytes, sizeof g);
        *err = dev.post_slot((int)idx);
        if (!err->empty()) { return true; }
        if (g.op == kMsgRead) {
          if (in_range(g)) {
            to_send.push_back(g);
          } else {
            *err = dev.send_msg(Msg{kMsgRefused, g.len, g.offset, g.id, 0});
          }
        } else if (g.op == kMsgWrite) {
          headers.push_back(g);
        } else if (g.op != kMsgHello) {
          *err = dev.name + ": a message of the wrong kind";
        }
      } else if (kind == kDataRecv) {
        landed.push_back({recvs++, wc[i].byte_len});
      } else if (kind == kCtlSend) {
        ++dev.ctl_done;
      } else if (kind == kDataSend) {
        --sends;
      }
      if (!err->empty()) { return true; }
    }
    // A write's bytes and its header -- each in the order sent -- paired,
    // copied into place, the staging receive posted again and acked.
    while (!headers.empty() && !landed.empty()) {
      const Msg h = headers.front();
      const Landed s = landed.front();
      headers.pop_front();
      landed.pop_front();
      const bool ok = in_range(h) && s.len == kPiece;
      if (ok) {
        std::memcpy(mem + h.offset, dev.recv_slot(s.idx), h.len);
        written->fetch_add(h.len);
      }
      // The slot taken, posted again: the user's credit back.
      *err = dev.post_recv(tag_of(kDataRecv, 0), s.idx, kPiece);
      if (err->empty()) {
        *err = dev.send_msg(
            Msg{ok ? kMsgAck : kMsgRefused, h.len, h.offset, h.id, 0});
      }
      if (!err->empty()) { return true; }
    }
    // The reads asked, kWindow at most in flight.
    bool sent = false;
    while (!to_send.empty() && sends < kWindow) {
      const Msg g = to_send.front();
      if (fw::is_file(g.offset)) {
        // a file's bytes, off this Mac's drive
        if (!read_lent_file(fw::file_id(g.offset),
                            fw::file_pos(g.offset),
                            dev.send_slot(sends_used), g.len)) {
          *err = dev.name + ": a lent file did not read";
          return true;
        }
      } else {
        std::memcpy(dev.send_slot(sends_used), mem + g.offset, g.len);
      }
      *err = dev.post_send(tag_of(kDataSend, 0), sends_used++, g.len);
      if (!err->empty()) { return true; }
      to_send.pop_front();
      ++sends;
      read->fetch_add(g.len);
      sent = true;
    }
    return n > 0 || sent;
  }

  void
  run()
  {
    // A member's decode waits on each answer: on a performance core,
    // spinning while asked (VALTZ_FAR_LINK_QOS=bg: an efficiency core's,
    // a study of what keeps this Mac's GPU slow).
    const char* qv = std::getenv("VALTZ_FAR_LINK_QOS");
    pthread_set_qos_class_self_np(qv != nullptr && std::string(qv) == "bg"
                                      ? QOS_CLASS_BACKGROUND
                                      : QOS_CLASS_USER_INTERACTIVE,
                                  0);
    using Clock = std::chrono::steady_clock;
    auto moved = Clock::now();
    while (!stop.load(std::memory_order_relaxed)) {
      std::string e;
      const bool now_moved = step(&e);
      if (now_moved) { moved = Clock::now(); }
      if (!e.empty()) {
        std::fprintf(stderr, "valtz far lender: %s\n", e.c_str());
        // The user hears of it as its connection closes.
        ::shutdown(fd, SHUT_RDWR);
        return;
      }
      // NEVER SPINNING: a pass that found nothing sleeps lend_idle_us,
      // its sends on the cable or not. A spinning thread here kept the
      // GPU's experts at 2.5x their time (a call 343 us at the user against
      // 222 so) and fetching costs nothing by it (54 ms a GB either way).
      // Apple's verbs have no completion events to block on (req_notify:
      // EOPNOTSUPP). VALTZ_FAR_LINK_WAIT=spin: spinning while sends are
      // out, as before.
      static const bool never_spin = [] {
        const char* v = std::getenv("VALTZ_FAR_LINK_WAIT");
        return v == nullptr || std::string(v) != "spin";
      }();
      const bool out = !never_spin && (sends > 0 || !to_send.empty());
      if (!out && (never_spin ? !now_moved
                              : Clock::now() - moved >
                                    std::chrono::microseconds(20))) {
        std::this_thread::sleep_for(
            std::chrono::microseconds(lend_idle_us()));
      }
    }
  }
};

}  // namespace

struct Lender::Impl {
  std::vector<std::unique_ptr<LendLink>> links;

  ~Impl()
  {
    for (auto& l : links) { l->stop.store(true); }
    for (auto& l : links) {
      if (l->thread.joinable()) { l->thread.join(); }
      if (l->call_thread.joinable()) { l->call_thread.join(); }
    }
    // Let go together: each device's close takes ~100 ms.
    std::vector<std::thread> ts;
    for (auto& l : links) {
      ts.emplace_back([p = l.release()]() { delete p; });
    }
    for (auto& t : ts) { t.join(); }
  }
};

Lender::Lender(std::unique_ptr<Impl> impl) : _impl(std::move(impl)) {}

Lender::~Lender() = default;

std::unique_ptr<Lender>
Lender::serve(int fd, std::uint32_t count, std::uint8_t* mem,
              std::uint64_t bytes, std::atomic<std::uint64_t>* read,
              std::atomic<std::uint64_t>* written, Call on_call,
              std::string* err)
{
  auto fail = [&](const std::string& why) {
    if (err != nullptr) { *err = why; }
    return std::unique_ptr<Lender>();
  };
  if (count == 0 || count > 16) { return fail("a bad count of links"); }
  std::vector<Ask> asks(count);
  if (!recv_all(fd, asks.data(), sizeof(Ask) * count)) {
    return fail("the user's connection was cut setting up RDMA");
  }
  auto impl = std::make_unique<Impl>();
  std::vector<Answer> ans(count);
  std::string why;
  for (std::uint32_t i = 0; i < count; ++i) {
    auto l = std::make_unique<LendLink>();
    l->mem = mem;
    l->bytes = bytes;
    l->read = read;
    l->written = written;
    l->fd = fd;
    asks[i].device[sizeof asks[i].device - 1] = 0;
    std::string e = available() ? l->dev.open(asks[i].device)
                                : "this Mac has no RDMA";
    if (e.empty()) { e = l->dev.connect(asks[i].info); }
    // Its credits: every receive slot posted.
    for (std::uint64_t k = 0; k < kWindow && e.empty(); ++k) {
      e = l->dev.post_recv(tag_of(kDataRecv, 0), k, kPiece);
    }
    for (int k = 0; k < kCallRecvs && e.empty(); ++k) {
      e = l->dev.post_call_recv(k, kCallBytes);
    }
    if (e.empty()) {
      ans[i].ok = 1;
      ans[i].info = l->dev.info();
    } else {
      std::snprintf(ans[i].why, sizeof ans[i].why, "%s", e.c_str());
      if (why.empty()) { why = e; }
    }
    impl->links.push_back(std::move(l));
  }
  if (!send_all(fd, ans.data(), sizeof(Answer) * count)) {
    return fail("the user's connection was cut setting up RDMA");
  }
  if (!why.empty()) { return fail(why); }
  char b = 'R';
  if (!send_all(fd, &b, 1) || !recv_all(fd, &b, 1)) {
    return fail("the user's connection was cut bringing RDMA up");
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    LendLink& l = *impl->links[i];
    const std::string e = l.dev.send_msg(Msg{kMsgHello, 0, i, 0, 0});
    if (!e.empty()) { return fail(e); }
    l.thread = std::thread([p = &l] { p->run(); });
    if (i == 0 && on_call) {
      l.on_call = on_call;
      l.call_thread = std::thread([p = &l] { p->run_calls(); });
    }
  }
  return std::unique_ptr<Lender>(new Lender(std::move(impl)));
}

void
lend_files(const std::string& root)
{
  std::lock_guard<std::mutex> lk(g_files_mu);
  g_files_root = root;
}

std::int64_t
open_lent_file(const std::string& name, std::uint64_t size,
               std::string* err)
{
  std::lock_guard<std::mutex> lk(g_files_mu);
  if (g_files_root.empty()) {
    if (err != nullptr) { *err = "this lender keeps no files"; }
    return -1;
  }
  if (name.empty() || name.front() == '/' ||
      name.find("..") != std::string::npos) {
    if (err != nullptr) { *err = "a file named outside the root"; }
    return -1;
  }
  const std::string path = g_files_root + "/" + name;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    if (err != nullptr) { *err = "no such file here: " + name; }
    return -1;
  }
  struct stat st{};
  if (::fstat(fd, &st) != 0 || (std::uint64_t)st.st_size != size) {
    ::close(fd);
    if (err != nullptr) { *err = "a file of another size here: " + name; }
    return -1;
  }
  // read as the model reads its own: past the memory cache
  (void)::fcntl(fd, F_NOCACHE, 1);
  for (std::size_t i = 0; i < g_files.size(); ++i) {
    if (g_files[i].size == size) {
      // the same file opened again (a model loaded again): one fd
      struct stat o{};
      if (::fstat(g_files[i].fd, &o) == 0 && o.st_ino == st.st_ino &&
          o.st_dev == st.st_dev) {
        ::close(fd);
        return (std::int64_t)i;
      }
    }
  }
  g_files.push_back({fd, size});
  return (std::int64_t)g_files.size() - 1;
}

}
