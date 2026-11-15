#include "valtz/fleet/model-share.h"

#include "valtz/base/log.h"
#include "valtz/models/model-store.h"

#include <CommonCrypto/CommonHMAC.h>
#include <Security/SecRandom.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <fstream>
#include <format>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace valtz::fleet {

namespace fs = std::filesystem;

namespace {

constexpr char kMagic[4] = {'V', 'Z', 'M', 'S'};
constexpr std::size_t kNonce = 32;
constexpr std::size_t kMac = CC_SHA256_DIGEST_LENGTH;
constexpr std::uint64_t kPiece = 64ull << 20;
constexpr std::size_t kMaxMessage = 64u << 20;
constexpr int kSocketBuffer = 8 << 20;
// What a bundle pushed whole is marked by, in its folder.
constexpr const char* kWhole = ".valtz-whole";

std::vector<std::uint8_t>
key_bytes(const std::string& hex)
{
  auto nib = [](char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
  };
  std::vector<std::uint8_t> out;
  for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
    const int hi = nib(hex[i]), lo = nib(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    out.push_back(static_cast<std::uint8_t>(hi << 4 | lo));
  }
  return out;
}

// HMAC-SHA256 of `side` and the two nonces, keyed by the fleet's key.
std::array<std::uint8_t, kMac>
proof(const std::vector<std::uint8_t>& key, char side,
      const std::uint8_t* cn, const std::uint8_t* sn)
{
  std::vector<std::uint8_t> msg = {'v', 'a', 'l', 't', 'z', '-', 's', 'h',
                                   'a', 'r', 'e', '/',
                                   static_cast<std::uint8_t>(side)};
  msg.insert(msg.end(), cn, cn + kNonce);
  msg.insert(msg.end(), sn, sn + kNonce);
  std::array<std::uint8_t, kMac> mac{};
  CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), msg.data(), msg.size(),
         mac.data());
  return mac;
}

bool
same(const std::uint8_t* a, const std::uint8_t* b, std::size_t n)
{
  std::uint8_t d = 0;
  for (std::size_t i = 0; i < n; ++i) {
    d |= a[i] ^ b[i];
  }
  return d == 0;
}

void
put_u32(std::uint8_t* p, std::uint32_t v)
{
  p[0] = static_cast<std::uint8_t>(v >> 24);
  p[1] = static_cast<std::uint8_t>(v >> 16);
  p[2] = static_cast<std::uint8_t>(v >> 8);
  p[3] = static_cast<std::uint8_t>(v);
}

std::uint32_t
get_u32(const std::uint8_t* p)
{
  return std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 |
         std::uint32_t(p[2]) << 8 | std::uint32_t(p[3]);
}

// A socket's reads and writes, each giving up when `stop` turns (checked
// at every timeout of the socket's own, a second) or the peer goes.
struct Io {
  int                      fd = -1;
  const std::atomic<bool>* stop = nullptr;

  bool
  stopped() const
  {
    return stop && stop->load();
  }

  bool
  write_all(const void* p, std::size_t n)
  {
    const auto* b = static_cast<const std::uint8_t*>(p);
    while (n > 0) {
      if (stopped()) {
        return false;
      }
      const ssize_t w = ::send(fd, b, n, 0);
      if (w < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        return false;
      }
      b += w;
      n -= static_cast<std::size_t>(w);
    }
    return true;
  }

  bool
  read_all(void* p, std::size_t n)
  {
    auto* b = static_cast<std::uint8_t*>(p);
    while (n > 0) {
      if (stopped()) {
        return false;
      }
      const ssize_t r = ::recv(fd, b, n, 0);
      if (r == 0) {
        return false;
      }
      if (r < 0) {
        if (errno == EINTR || errno == EAGAIN) {
          continue;
        }
        return false;
      }
      b += r;
      n -= static_cast<std::size_t>(r);
    }
    return true;
  }

  bool
  send_message(const Json& j)
  {
    const auto body = to_cbor(j);
    std::uint8_t len[4];
    put_u32(len, static_cast<std::uint32_t>(body.size()));
    return write_all(len, 4) && write_all(body.data(), body.size());
  }

  Result<Json>
  read_message()
  {
    std::uint8_t len[4];
    if (!read_all(len, 4)) {
      return make_error(stopped() ? Code::Cancelled : Code::Network,
                        "the member's connection was cut");
    }
    const std::uint32_t n = get_u32(len);
    if (n > kMaxMessage) {
      return make_error(Code::Corrupt, "a message past its size");
    }
    std::vector<std::uint8_t> body(n);
    if (!read_all(body.data(), n)) {
      return make_error(stopped() ? Code::Cancelled : Code::Network,
                        "the member's connection was cut");
    }
    return from_cbor(body);
  }
};

void
tune_socket(int fd)
{
  const int one = 1;
  ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &kSocketBuffer,
               sizeof kSocketBuffer);
  ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &kSocketBuffer,
               sizeof kSocketBuffer);
  timeval tv{1, 0};
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
}

bool
random_bytes(std::uint8_t* p, std::size_t n)
{
  return SecRandomCopyBytes(kSecRandomDefault, n, p) == errSecSuccess;
}

// --- The member's side -------------------------------------------------

class Server final : public ShareServer {
public:
  Server(Key key, Resolve resolve, Receive receive)
      : _key(std::move(key)), _resolve(std::move(resolve)),
        _receive(std::move(receive))
  {}

  ~Server() override
  {
    _stop = true;
    if (_listen >= 0) {
      ::shutdown(_listen, SHUT_RDWR);
      ::close(_listen);
    }
    if (_accept.joinable()) {
      _accept.join();
    }
    std::vector<Conn> ts;
    {
      std::lock_guard lk(_mu);
      for (int fd : _open) {
        ::shutdown(fd, SHUT_RDWR);
      }
      ts.swap(_threads);
    }
    for (auto& t : ts) {
      if (t.thread.joinable()) {
        t.thread.join();
      }
    }
  }

  Status
  open()
  {
    _listen = ::socket(AF_INET, SOCK_STREAM, 0);
    if (_listen < 0) {
      return make_error(Code::Network, std::format(
          "no socket for the model share: {}", std::strerror(errno)));
    }
    const int one = 1;
    ::setsockopt(_listen, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = 0;
    if (::bind(_listen, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 ||
        ::listen(_listen, 32) != 0) {
      return make_error(Code::Network, std::format(
          "the model share cannot listen: {}", std::strerror(errno)));
    }
    socklen_t len = sizeof a;
    ::getsockname(_listen, reinterpret_cast<sockaddr*>(&a), &len);
    _port = ntohs(a.sin_port);
    _accept = std::thread([this] { accept_loop_(); });
    VALTZ_LOG_INFO("fleet", "models shared on port {}", _port);
    return ok_status();
  }

  int port() const override { return _port; }
  std::uint64_t served() const override { return _served.load(); }

private:
  void
  accept_loop_()
  {
    while (!_stop) {
      pollfd p{_listen, POLLIN, 0};
      if (::poll(&p, 1, 500) <= 0) {
        continue;
      }
      const int fd = ::accept(_listen, nullptr, nullptr);
      if (fd < 0) {
        continue;
      }
      tune_socket(fd);
      std::vector<std::thread> ended;
      {
        std::lock_guard lk(_mu);
        // Threads of connections ended are let go as new ones come.
        for (auto it = _threads.begin(); it != _threads.end();) {
          if (it->done->load()) {
            ended.push_back(std::move(it->thread));
            it = _threads.erase(it);
          } else {
            ++it;
          }
        }
        _open.insert(fd);
        auto done = std::make_shared<std::atomic<bool>>(false);
        _threads.push_back({std::thread([this, fd, done] {
                              serve_(fd);
                              {
                                std::lock_guard l(_mu);
                                _open.erase(fd);
                              }
                              ::close(fd);
                              *done = true;
                            }),
                            done});
      }
      for (auto& t : ended) {
        t.join();
      }
    }
  }

  bool
  handshake_(Io& io)
  {
    std::uint8_t hello[4 + 4 + kNonce];
    if (!io.read_all(hello, sizeof hello) ||
        std::memcmp(hello, kMagic, 4) != 0 ||
        get_u32(hello + 4) != kShareProtocol) {
      return false;
    }
    const auto key = key_bytes(_key ? _key() : std::string());
    if (key.empty()) {
      return false;
    }
    const std::uint8_t* cn = hello + 8;
    std::uint8_t reply[4 + 4 + kNonce + kMac];
    std::memcpy(reply, kMagic, 4);
    put_u32(reply + 4, kShareProtocol);
    std::uint8_t* sn = reply + 8;
    if (!random_bytes(sn, kNonce)) {
      return false;
    }
    const auto mine = proof(key, 's', cn, sn);
    std::memcpy(reply + 8 + kNonce, mine.data(), kMac);
    if (!io.write_all(reply, sizeof reply)) {
      return false;
    }
    std::uint8_t theirs[kMac];
    if (!io.read_all(theirs, kMac)) {
      return false;
    }
    const auto want = proof(key, 'c', cn, sn);
    return same(theirs, want.data(), kMac);
  }

  void
  serve_(int fd)
  {
    Io io{fd, &_stop};
    if (!handshake_(io)) {
      VALTZ_LOG_WARN("fleet", "a model share connection refused: it did "
                     "not prove the fleet's key");
      return;
    }
    // Each model asked for, listed once a connection.
    std::map<std::string, ShareList> lists;
    const auto list_of = [&](const std::string& model)
        -> Result<const ShareList*> {
      if (auto it = lists.find(model); it != lists.end()) {
        return &it->second;
      }
      VALTZ_ASSIGN(ShareList l, _resolve(model));
      return &(lists[model] = std::move(l));
    };
    while (!_stop) {
      auto m = io.read_message();
      if (!m.ok()) {
        return;
      }
      const auto op = jget<std::string>(*m, "op", "");
      if (op == "begin" || op == "write" || op == "end") {
        if (!receive_(io, op, *m)) {
          return;
        }
        continue;
      }
      const auto model = jget<std::string>(*m, "model", "");
      auto l = list_of(model);
      if (!l.ok()) {
        io.send_message({{"error", l.error().message}});
        continue;
      }
      if (op == "list") {
        Json files = Json::array();
        for (const auto& f : (*l)->files) {
          files.push_back({{"path", f.path}, {"size", f.size}});
        }
        if (!io.send_message({{"files", std::move(files)}})) {
          return;
        }
        continue;
      }
      if (op != "read") {
        io.send_message({{"error", "not a request of this protocol"}});
        continue;
      }
      // Only a file it listed, within its size.
      const auto path = jget<std::string>(*m, "path", "");
      const auto offset = jget<std::uint64_t>(*m, "offset", 0);
      const auto length = jget<std::uint64_t>(*m, "length", 0);
      const ShareFile* f = nullptr;
      for (const auto& x : (*l)->files) {
        if (x.path == path) {
          f = &x;
        }
      }
      if (!f || offset > f->size || length > f->size - offset) {
        io.send_message({{"error", "not a file of the model"}});
        continue;
      }
      const fs::path at =
          f->from.empty() ? (*l)->root / fs::path(path) : f->from;
      const int in = ::open(at.c_str(), O_RDONLY);
      if (in < 0) {
        io.send_message({{"error", std::format("cannot read {}: {}", path,
                                               std::strerror(errno))}});
        continue;
      }
      if (!io.send_message({{"length", length}})) {
        ::close(in);
        return;
      }
      // The file's pages straight onto the socket.
      off_t at_off = static_cast<off_t>(offset);
      std::uint64_t left = length;
      bool ok = true;
      while (left > 0 && !_stop) {
        off_t n = static_cast<off_t>(std::min<std::uint64_t>(left,
                                                             16u << 20));
        const int r = ::sendfile(in, fd, at_off, &n, nullptr, 0);
        if (n > 0) {
          at_off += n;
          left -= static_cast<std::uint64_t>(n);
          _served += static_cast<std::uint64_t>(n);
        }
        if (r != 0 && errno != EAGAIN && errno != EINTR) {
          ok = false;
          break;
        }
      }
      ::close(in);
      if (!ok || left > 0) {
        return;
      }
    }
  }

  // A BUNDLE PUSHED here (a job's own weights, from its sender: a
  // member serving detached may not connect out, so the sender pushes).
  // "begin" names its files and makes their `.part`s -- those whole here
  // already kept, said back as not needed; "write" a piece of one, its
  // bytes after the message, acknowledged once on disk; "end" marks it
  // whole. False: the connection is to be let go.
  bool
  receive_(Io& io, const std::string& op, const Json& m)
  {
    const auto bundle = jget<std::string>(m, "bundle", "");
    if (bundle.empty() || bundle.size() > 64 ||
        bundle.find_first_not_of("0123456789abcdef") != std::string::npos) {
      return io.send_message({{"error", "not a bundle's name"}});
    }
    const auto safe = [](const std::string& path) {
      const fs::path rel(path);
      return !path.empty() && !rel.is_absolute() &&
             !rel.lexically_normal().string().starts_with("..");
    };
    std::error_code ec;
    if (op == "begin") {
      std::uint64_t bytes = 0;
      std::vector<ShareFile> files;
      for (const auto& f : jget(m, "files", Json::array())) {
        const auto path = jget<std::string>(f, "path", "");
        if (!safe(path)) {
          return io.send_message({{"error", "a file outside its bundle"}});
        }
        files.push_back({path, jget<std::uint64_t>(f, "size", 0), {}});
        bytes += files.back().size;
      }
      if (!_receive) {
        return io.send_message({{"error", "this Mac takes no files"}});
      }
      auto dir = _receive(bundle, bytes);
      if (!dir.ok()) {
        return io.send_message({{"error", dir.error().message},
                                {"no_room", dir.code() == Code::Busy}});
      }
      auto in = std::make_shared<Incoming>();
      in->dir = *dir;
      Json need = Json::array();
      if (!fs::exists(*dir / kWhole, ec)) {
        for (const auto& f : files) {
          const fs::path to = *dir / fs::path(f.path);
          if (fs::is_regular_file(to, ec) &&
              fs::file_size(to, ec) == f.size) {
            continue;
          }
          fs::create_directories(to.parent_path(), ec);
          fs::path part = to;
          part += ".part";
          const int out = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC,
                                 0644);
          if (out < 0 ||
              ::ftruncate(out, static_cast<off_t>(f.size)) != 0) {
            if (out >= 0) {
              ::close(out);
            }
            return io.send_message({{"error", std::format(
                "cannot write {}", part.string())}});
          }
          ::close(out);
          if (f.size == 0) {
            fs::rename(part, to, ec);
            continue;
          }
          in->size[f.path] = f.size;
          in->left[f.path] = f.size;
          need.push_back(f.path);
        }
      }
      {
        std::lock_guard lk(_in_mu);
        _incoming[bundle] = in;
      }
      return io.send_message({{"need", std::move(need)}});
    }
    std::shared_ptr<Incoming> in;
    {
      std::lock_guard lk(_in_mu);
      if (auto it = _incoming.find(bundle); it != _incoming.end()) {
        in = it->second;
      }
    }
    if (!in) {
      // A write's bytes follow it: the connection cannot go on.
      io.send_message({{"error", "that bundle was not begun"}});
      return op != "write";
    }
    if (op == "end") {
      bool whole = true;
      {
        std::lock_guard lk(_in_mu);
        for (const auto& [p, n] : in->left) {
          whole = whole && n == 0;
        }
        if (whole) {
          _incoming.erase(bundle);
        }
      }
      if (!whole) {
        return io.send_message({{"error", "the bundle is not whole"}});
      }
      std::ofstream(in->dir / kWhole) << bundle << "\n";
      return io.send_message({{"ok", true}});
    }
    const auto path = jget<std::string>(m, "path", "");
    const auto offset = jget<std::uint64_t>(m, "offset", 0);
    const auto length = jget<std::uint64_t>(m, "length", 0);
    std::uint64_t size = 0;
    {
      std::lock_guard lk(_in_mu);
      if (auto it = in->size.find(path); it != in->size.end()) {
        size = it->second;
      }
    }
    if (size == 0 || offset > size || length > size - offset) {
      io.send_message({{"error", "not a file of the bundle"}});
      return false;
    }
    fs::path part = in->dir / fs::path(path);
    part += ".part";
    const int out = ::open(part.c_str(), O_WRONLY);
    std::vector<std::uint8_t> buf(8u << 20);
    std::uint64_t got = 0;
    bool ok = out >= 0;
    while (got < length) {
      const std::size_t n = static_cast<std::size_t>(
          std::min<std::uint64_t>(buf.size(), length - got));
      if (!io.read_all(buf.data(), n)) {
        if (out >= 0) {
          ::close(out);
        }
        return false;
      }
      if (ok && ::pwrite(out, buf.data(), n,
                         static_cast<off_t>(offset + got)) !=
                    static_cast<ssize_t>(n)) {
        ok = false;
      }
      got += n;
    }
    if (out >= 0) {
      ::close(out);
    }
    if (!ok) {
      return io.send_message({{"error", std::format("cannot write {}",
                                                    part.string())}});
    }
    bool last = false;
    {
      std::lock_guard lk(_in_mu);
      auto& left = in->left[path];
      left -= std::min(left, length);
      last = left == 0;
    }
    if (last) {
      fs::rename(part, in->dir / fs::path(path), ec);
    }
    _received += length;
    return io.send_message({{"ok", true}});
  }

  Key                        _key;
  Resolve                    _resolve;
  Receive                    _receive;
  // Bundles being pushed here, by name: their folder, each file's bytes
  // still to come (several streams write one at once).
  struct Incoming {
    fs::path                             dir;
    std::map<std::string, std::uint64_t> size;
    std::map<std::string, std::uint64_t> left;
  };
  std::mutex                                       _in_mu;
  std::map<std::string, std::shared_ptr<Incoming>> _incoming;
  int                        _listen = -1;
  int                        _port = 0;
  std::atomic<bool>          _stop{false};
  std::atomic<std::uint64_t> _served{0};
  std::atomic<std::uint64_t> _received{0};
  std::thread                _accept;
  std::mutex                 _mu;
  std::set<int>              _open;
  struct Conn {
    std::thread                        thread;
    std::shared_ptr<std::atomic<bool>> done;
  };
  std::vector<Conn>          _threads;
};

// --- The copy -------------------------------------------------------------

// A connection to the member, its key proved both ways.
Result<int>
connect_to(const ShareRoute& r, int port,
           const std::vector<std::uint8_t>& key,
           const std::atomic<bool>& stop)
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return make_error(Code::Network, "no socket");
  }
  struct Closer {
    int  fd;
    bool keep = false;
    ~Closer()
    {
      if (!keep) {
        ::close(fd);
      }
    }
  } c{fd};
  if (!r.interface.empty()) {
    const unsigned idx = if_nametoindex(r.interface.c_str());
    if (idx == 0 || ::setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx,
                                 sizeof idx) != 0) {
      return make_error(Code::Network, std::format(
          "cannot use {} to reach the member", r.interface));
    }
  }
  tune_socket(fd);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, r.address.c_str(), &a.sin_addr) != 1) {
    return make_error(Code::InvalidArgument, std::format(
        "not an address: {}", r.address));
  }
  const int flags = ::fcntl(fd, F_GETFL, 0);
  ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 &&
      errno != EINPROGRESS) {
    return make_error(Code::Network, std::format(
        "{}:{} by {}: {}", r.address, port,
        r.interface.empty() ? "the network" : r.interface,
        std::strerror(errno)));
  }
  pollfd p{fd, POLLOUT, 0};
  int err = 0;
  socklen_t elen = sizeof err;
  if (::poll(&p, 1, 3000) != 1 ||
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 ||
      err != 0) {
    return make_error(Code::Network, std::format(
        "{}:{} by {}: {}", r.address, port,
        r.interface.empty() ? "the network" : r.interface,
        err != 0 ? std::strerror(err) : "no answer"));
  }
  ::fcntl(fd, F_SETFL, flags);
  Io io{fd, &stop};
  std::uint8_t hello[4 + 4 + kNonce];
  std::memcpy(hello, kMagic, 4);
  put_u32(hello + 4, kShareProtocol);
  std::uint8_t* cn = hello + 8;
  if (!random_bytes(cn, kNonce) || !io.write_all(hello, sizeof hello)) {
    return make_error(Code::Network, "the member did not answer");
  }
  std::uint8_t reply[4 + 4 + kNonce + kMac];
  if (!io.read_all(reply, sizeof reply)) {
    return make_error(Code::Network, "the member refused the copy: not "
                                     "the same fleet's key");
  }
  if (std::memcmp(reply, kMagic, 4) != 0 ||
      get_u32(reply + 4) != kShareProtocol) {
    return make_error(Code::Unsupported, "the member's Valtz shares "
                                         "models another way: update both");
  }
  const std::uint8_t* sn = reply + 8;
  const auto want = proof(key, 's', cn, sn);
  if (!same(reply + 8 + kNonce, want.data(), kMac)) {
    return make_error(Code::Network, "the member did not prove the "
                                     "fleet's key");
  }
  const auto mine = proof(key, 'c', cn, sn);
  if (!io.write_all(mine.data(), kMac)) {
    return make_error(Code::Network, "the member's connection was cut");
  }
  c.keep = true;
  return fd;
}

struct Opened {
  std::vector<ShareRoute> routes;
  int                     first = -1;
};

// The routes taken: every Thunderbolt one; else the first network one
// that answers -- and a connection on the first, to talk on.
Result<Opened>
open_routes(const CopyPlan& plan, const std::vector<std::uint8_t>& key,
            const std::atomic<bool>& stop)
{
  Opened o;
  std::string why;
  for (const auto& r : plan.routes) {
    if (r.interface.empty() && !o.routes.empty()) {
      continue;
    }
    auto fd = connect_to(r, plan.port, key, stop);
    if (!fd.ok()) {
      why = fd.error().message;
      if (fd.code() == Code::Unsupported) {
        return fd.error();
      }
      VALTZ_LOG_WARN("fleet", "model copy: {}", why);
      continue;
    }
    if (o.first < 0) {
      o.first = *fd;
    } else {
      ::close(*fd);
    }
    o.routes.push_back(r);
    if (r.interface.empty()) {
      break;
    }
  }
  if (o.first < 0) {
    return make_error(Code::Network, std::format(
        "the member cannot be reached for the copy ({})", why));
  }
  return o;
}

struct Piece {
  std::size_t   file = 0;
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

}

Result<ShareList>
list_model_files(const fs::path& root, const std::string& file)
{
  std::error_code ec;
  ShareList out;
  out.root = root;
  if (!file.empty()) {
    const fs::path f = root / file;
    if (!fs::is_regular_file(f, ec)) {
      return make_error(Code::NotFound, std::format("{} is not there",
                                                    f.string()));
    }
    out.files.push_back({fs::path(file).generic_string(),
                         fs::file_size(f, ec), {}});
    // ...and what its download brings beside it that is not another
    // model's weights: a sampler's manifest next to an adapter (Pika H3
    // Flex's `fl2va/manifest.json`: without it the copy runs the adapter
    // off its sampling contract), its repo's LICENSE and NOTICE -- small
    // files, in its folder and at the repo's top. A weight file there is
    // another model of the repo (lightx2v's three LoRAs share one).
    const auto sidecar = [&](const fs::path& p) {
      static constexpr std::string_view kWeights[] = {
          ".safetensors", ".gguf", ".bin", ".pt", ".pth", ".ckpt",
          ".pkl", ".onnx", ".npz", ".tar", ".zip", ".mlmodel"};
      std::string ext = p.extension().string();
      std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return std::ranges::find(kWeights, ext) == std::end(kWeights) &&
             p.extension() != ".part" &&
             p.filename() != models::ModelStore::kCompressedMarker &&
             p.filename() != ".DS_Store" &&
             fs::file_size(p, ec) <= (16u << 20);
    };
    std::set<std::string> seen = {out.files.front().path};
    for (const fs::path& dir : {f.parent_path(), root}) {
      for (const auto& de : fs::directory_iterator(
               dir, fs::directory_options::skip_permission_denied, ec)) {
        if (!de.is_regular_file(ec) || !sidecar(de.path())) {
          continue;
        }
        const std::string rel =
            de.path().lexically_relative(root).generic_string();
        if (seen.insert(rel).second) {
          out.files.push_back({rel, de.file_size(ec), {}});
        }
      }
    }
    return out;
  }
  if (!fs::is_directory(root, ec)) {
    return make_error(Code::NotFound, std::format("{} is not there",
                                                  root.string()));
  }
  for (auto it = fs::recursive_directory_iterator(
           root, fs::directory_options::follow_directory_symlink |
                     fs::directory_options::skip_permission_denied, ec);
       it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (ec) {
      break;
    }
    if (!it->is_regular_file(ec)) {
      continue;
    }
    const fs::path& p = it->path();
    if (p.extension() == ".part" ||
        p.filename() == models::ModelStore::kCompressedMarker ||
        p.filename() == ".DS_Store") {
      continue;
    }
    out.files.push_back({p.lexically_relative(root).generic_string(),
                         it->file_size(ec), {}});
  }
  std::sort(out.files.begin(), out.files.end(),
            [](const ShareFile& a, const ShareFile& b) {
              return a.path < b.path;
            });
  return out;
}

// This Mac's IPv4 addresses, up, not the loopback: where a member without
// a Thunderbolt cable reaches its model share (link-local last).
Json
share_addresses()
{
  Json routable = Json::array(), local = Json::array();
  ifaddrs* all = nullptr;
  if (::getifaddrs(&all) != 0) {
    return routable;
  }
  for (ifaddrs* a = all; a; a = a->ifa_next) {
    if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET ||
        !(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) {
      continue;
    }
    char buf[INET_ADDRSTRLEN] = {};
    const auto* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
    ::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf);
    const std::string s = buf;
    (s.starts_with("169.254.") ? local : routable).push_back(s);
  }
  ::freeifaddrs(all);
  for (auto& s : local) {
    routable.push_back(std::move(s));
  }
  return routable;
}

Result<std::unique_ptr<ShareServer>>
ShareServer::start(Key key, Resolve resolve, Receive receive)
{
  auto s = std::make_unique<Server>(std::move(key), std::move(resolve),
                                    std::move(receive));
  VALTZ_TRY(s->open());
  return std::unique_ptr<ShareServer>(std::move(s));
}

Json
to_json(const CopyPlan& p)
{
  Json routes = Json::array();
  for (const auto& r : p.routes) {
    routes.push_back({{"address", r.address},
                      {"interface", r.interface},
                      {"gbps", r.gbps}});
  }
  return {{"routes", std::move(routes)}, {"port", p.port}, {"key", p.key},
          {"model", p.model}, {"dest", p.dest.string()},
          {"streams", p.streams}};
}

CopyPlan
copy_plan_from_json(const Json& j)
{
  CopyPlan p;
  for (const auto& r : jget(j, "routes", Json::array())) {
    p.routes.push_back({jget<std::string>(r, "address", ""),
                        jget<std::string>(r, "interface", ""),
                        jget(r, "gbps", 0)});
  }
  p.port = jget(j, "port", 0);
  p.key = jget<std::string>(j, "key", "");
  p.model = jget<std::string>(j, "model", "");
  p.dest = jget<std::string>(j, "dest", "");
  p.streams = std::clamp(jget(j, "streams", 4), 1, 16);
  return p;
}

Result<CopyResult>
copy_model(const CopyPlan& plan,
           const std::function<bool(std::uint64_t, std::uint64_t)>& progress)
{
  const auto started = std::chrono::steady_clock::now();
  const auto key = key_bytes(plan.key);
  if (key.empty() || plan.routes.empty() || plan.port <= 0 ||
      plan.dest.empty() || plan.model.empty()) {
    return make_error(Code::InvalidArgument, "a copy names its member's "
                      "routes, its port, the key and the model");
  }
  std::atomic<bool> stop{false};
  VALTZ_ASSIGN(Opened opened, open_routes(plan, key, stop));
  const std::vector<ShareRoute>& routes = opened.routes;
  const int first = opened.first;
  // What the member has of it.
  Io ctl{first, &stop};
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } ctl_closer{first};
  if (!ctl.send_message({{"op", "list"}, {"model", plan.model}})) {
    return make_error(Code::Network, "the member's connection was cut");
  }
  VALTZ_ASSIGN(Json listed, ctl.read_message());
  if (listed.contains("error")) {
    return make_error(Code::NotFound, std::format(
        "the member does not share it: {}",
        jget<std::string>(listed, "error", "")));
  }
  std::vector<ShareFile> files;
  for (const auto& f : jget(listed, "files", Json::array())) {
    const auto path = jget<std::string>(f, "path", "");
    const fs::path rel(path);
    // Nothing outside the model's folder.
    if (path.empty() || rel.is_absolute() ||
        rel.lexically_normal().string().starts_with("..")) {
      return make_error(Code::Corrupt, std::format(
          "the member listed {}: not a file of the model", path));
    }
    files.push_back({path, jget<std::uint64_t>(f, "size", 0), {}});
  }
  if (files.empty()) {
    return make_error(Code::NotFound, "the member has no files of it");
  }

  // The files to fetch -- those whole here already kept -- each as its
  // `.part`, its whole size made now.
  CopyResult res;
  res.files = files.size();
  std::vector<std::atomic<std::uint64_t>> left(files.size());
  std::vector<Piece> pieces;
  std::error_code ec;
  for (std::size_t i = 0; i < files.size(); ++i) {
    const auto& f = files[i];
    res.total += f.size;
    const fs::path to = plan.dest / fs::path(f.path);
    if (fs::is_regular_file(to, ec) && fs::file_size(to, ec) == f.size) {
      left[i] = 0;
      continue;
    }
    fs::create_directories(to.parent_path(), ec);
    fs::path part = to;
    part += ".part";
    const int out = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0 || ::ftruncate(out, static_cast<off_t>(f.size)) != 0) {
      const std::string e = std::strerror(errno);
      if (out >= 0) {
        ::close(out);
      }
      return make_error(Code::Io, std::format("cannot write {}: {}",
                                              part.string(), e));
    }
    ::close(out);
    left[i] = f.size;
    if (f.size == 0) {
      fs::rename(part, to, ec);
      continue;
    }
    for (std::uint64_t o = 0; o < f.size; o += kPiece) {
      pieces.push_back({i, o, std::min(kPiece, f.size - o)});
    }
  }
  std::uint64_t had = 0;
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (left[i] == 0) {
      had += files[i].size;
    }
  }

  // The streams: each takes the next piece -- one put back by a stream
  // that failed first -- until none is left.
  std::mutex mu;
  std::size_t next = 0;
  std::deque<Piece> again;
  std::atomic<std::uint64_t> done{0};
  std::atomic<int> alive{0};
  std::string failed;
  const auto take = [&](Piece* p) {
    std::lock_guard lk(mu);
    if (!again.empty()) {
      *p = again.front();
      again.pop_front();
      return true;
    }
    if (next < pieces.size()) {
      *p = pieces[next++];
      return true;
    }
    return false;
  };
  const auto give_back = [&](const Piece& p, const std::string& e) {
    std::lock_guard lk(mu);
    again.push_back(p);
    failed = e;
  };
  const auto landed = [&](const Piece& p) {
    if (left[p.file].fetch_sub(p.length) == p.length) {
      const fs::path to = plan.dest / fs::path(files[p.file].path);
      fs::path part = to;
      part += ".part";
      std::error_code e;
      fs::rename(part, to, e);
      if (e) {
        std::lock_guard lk(mu);
        failed = std::format("cannot keep {}: {}", to.string(),
                             e.message());
        stop = true;
      }
    }
  };
  const auto stream = [&](ShareRoute r, int given) {
    int fd = given;
    if (fd < 0) {
      auto c = connect_to(r, plan.port, key, stop);
      if (!c.ok()) {
        std::lock_guard lk(mu);
        failed = c.error().message;
        --alive;
        return;
      }
      fd = *c;
    }
    Io io{fd, &stop};
    std::vector<std::uint8_t> buf(8u << 20);
    Piece p;
    while (!stop && take(&p)) {
      const auto& f = files[p.file];
      bool ok = io.send_message({{"op", "read"}, {"model", plan.model},
                                 {"path", f.path}, {"offset", p.offset},
                                 {"length", p.length}});
      auto head = ok ? io.read_message() : Result<Json>(Json());
      if (!ok || !head.ok() || head->contains("error") ||
          jget<std::uint64_t>(*head, "length", 0) != p.length) {
        give_back(p, head.ok() ? jget<std::string>(*head, "error",
                                                   "the member's "
                                                   "connection was cut")
                               : head.error().message);
        break;
      }
      fs::path part = plan.dest / fs::path(f.path);
      part += ".part";
      const int out = ::open(part.c_str(), O_WRONLY);
      if (out < 0) {
        give_back(p, std::format("cannot write {}", part.string()));
        stop = true;
        break;
      }
      std::uint64_t got = 0;
      while (got < p.length) {
        const std::size_t n = static_cast<std::size_t>(
            std::min<std::uint64_t>(buf.size(), p.length - got));
        if (!io.read_all(buf.data(), n)) {
          ok = false;
          break;
        }
        if (::pwrite(out, buf.data(), n,
                     static_cast<off_t>(p.offset + got)) !=
            static_cast<ssize_t>(n)) {
          ok = false;
          stop = true;
          break;
        }
        got += n;
        done += n;
      }
      ::close(out);
      if (!ok) {
        // What came of it is counted again when it comes again.
        done -= got;
        give_back(p, stop ? std::string("cannot write ") + part.string()
                          : std::string("the member's connection was "
                                        "cut"));
        break;
      }
      landed(p);
    }
    if (fd != given) {
      ::close(fd);
    }
    --alive;
  };
  std::vector<std::thread> threads;
  std::string used;
  for (const auto& r : routes) {
    const int n = std::max(1, plan.streams);
    used += std::format("{}{} x{}", used.empty() ? "" : ", ",
                        r.interface.empty() ? r.address : r.interface, n);
    for (int k = 0; k < n; ++k) {
      ++alive;
      threads.emplace_back(stream, r, -1);
    }
  }
  bool cancelled = false;
  while (alive.load() > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!stop && progress && !progress(had + done.load(), res.total)) {
      cancelled = true;
      stop = true;
    }
  }
  for (auto& t : threads) {
    t.join();
  }
  // Ended with pieces left: every stream lost.
  bool whole = true;
  for (std::size_t i = 0; i < files.size(); ++i) {
    whole = whole && left[i].load() == 0;
  }
  if (cancelled) {
    return make_error(Code::Cancelled, "the copy was stopped");
  }
  if (!whole) {
    return make_error(Code::Network, std::format(
        "the copy did not finish: {}",
        failed.empty() ? std::string("the member's connections were cut")
                       : failed));
  }
  if (progress) {
    progress(res.total, res.total);
  }
  res.bytes = done.load();
  res.routes = used;
  res.seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started)
                    .count();
  return res;
}

Result<CopyResult>
push_files(const CopyPlan& plan, const std::vector<ShareFile>& files,
           const std::function<bool(std::uint64_t, std::uint64_t)>& progress)
{
  const auto started = std::chrono::steady_clock::now();
  const auto key = key_bytes(plan.key);
  if (key.empty() || plan.routes.empty() || plan.port <= 0 ||
      plan.model.empty() || files.empty()) {
    return make_error(Code::InvalidArgument, "a push names its member's "
                      "routes, its port, the key, the bundle and files");
  }
  std::atomic<bool> stop{false};
  VALTZ_ASSIGN(Opened opened, open_routes(plan, key, stop));
  Io ctl{opened.first, &stop};
  struct Closer {
    int fd;
    ~Closer() { ::close(fd); }
  } ctl_closer{opened.first};
  CopyResult res;
  res.files = files.size();
  Json listed = Json::array();
  for (const auto& f : files) {
    listed.push_back({{"path", f.path}, {"size", f.size}});
    res.total += f.size;
  }
  if (!ctl.send_message({{"op", "begin"}, {"bundle", plan.model},
                         {"files", std::move(listed)}})) {
    return make_error(Code::Network, "the member's connection was cut");
  }
  VALTZ_ASSIGN(Json begun, ctl.read_message());
  if (begun.contains("error")) {
    return make_error(jget(begun, "no_room", false) ? Code::Busy
                                                    : Code::Network,
                      jget<std::string>(begun, "error", ""));
  }
  std::set<std::string> need;
  for (const auto& n : jget(begun, "need", Json::array())) {
    if (n.is_string()) {
      need.insert(n.get<std::string>());
    }
  }
  std::vector<Piece> pieces;
  std::uint64_t had = 0;
  for (std::size_t i = 0; i < files.size(); ++i) {
    if (!need.contains(files[i].path)) {
      had += files[i].size;
      continue;
    }
    for (std::uint64_t o = 0; o < files[i].size; o += kPiece) {
      pieces.push_back({i, o, std::min(kPiece, files[i].size - o)});
    }
  }
  std::mutex mu;
  std::size_t next = 0;
  std::deque<Piece> again;
  std::atomic<std::uint64_t> done{0};
  std::atomic<std::uint64_t> landed{0};
  std::atomic<int> alive{0};
  std::string failed;
  const auto take = [&](Piece* p) {
    std::lock_guard lk(mu);
    if (!again.empty()) {
      *p = again.front();
      again.pop_front();
      return true;
    }
    if (next < pieces.size()) {
      *p = pieces[next++];
      return true;
    }
    return false;
  };
  const auto stream = [&](ShareRoute r) {
    auto c = connect_to(r, plan.port, key, stop);
    if (!c.ok()) {
      std::lock_guard lk(mu);
      failed = c.error().message;
      --alive;
      return;
    }
    const int fd = *c;
    Io io{fd, &stop};
    Piece p;
    while (!stop && take(&p)) {
      const auto& f = files[p.file];
      const int in = ::open(f.from.c_str(), O_RDONLY);
      bool ok = in >= 0 &&
                io.send_message({{"op", "write"}, {"bundle", plan.model},
                                 {"path", f.path}, {"offset", p.offset},
                                 {"length", p.length}});
      // The file's pages straight onto the socket.
      off_t at = static_cast<off_t>(p.offset);
      std::uint64_t left = p.length;
      while (ok && left > 0 && !stop) {
        off_t n = static_cast<off_t>(std::min<std::uint64_t>(left,
                                                             16u << 20));
        const int rc = ::sendfile(in, fd, at, &n, nullptr, 0);
        if (n > 0) {
          at += n;
          left -= static_cast<std::uint64_t>(n);
          done += static_cast<std::uint64_t>(n);
        }
        if (rc != 0 && errno != EAGAIN && errno != EINTR) {
          ok = false;
        }
      }
      if (in >= 0) {
        ::close(in);
      }
      auto ack = ok && left == 0 ? io.read_message() : Result<Json>(Json());
      if (!ok || left > 0 || !ack.ok() || ack->contains("error")) {
        done -= p.length - left;
        std::lock_guard lk(mu);
        again.push_back(p);
        failed = in < 0 ? std::format("cannot read {}", f.from.string())
                 : ack.ok() && ack->contains("error")
                     ? jget<std::string>(*ack, "error", "")
                     : std::string("the member's connection was cut");
        break;
      }
      landed += p.length;
    }
    ::close(fd);
    --alive;
  };
  std::vector<std::thread> threads;
  std::string used;
  for (const auto& r : opened.routes) {
    const int n = std::max(1, plan.streams);
    used += std::format("{}{} x{}", used.empty() ? "" : ", ",
                        r.interface.empty() ? r.address : r.interface, n);
    for (int k = 0; k < n; ++k) {
      ++alive;
      threads.emplace_back(stream, r);
    }
  }
  bool cancelled = false;
  while (alive.load() > 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (!stop && progress && !progress(had + done.load(), res.total)) {
      cancelled = true;
      stop = true;
    }
  }
  for (auto& t : threads) {
    t.join();
  }
  if (cancelled) {
    return make_error(Code::Cancelled, "the copy was stopped");
  }
  std::uint64_t needed = 0;
  for (const auto& p : pieces) {
    needed += p.length;
  }
  if (landed.load() != needed) {
    return make_error(Code::Network, std::format(
        "the files did not all go: {}",
        failed.empty() ? std::string("the member's connections were cut")
                       : failed));
  }
  if (!ctl.send_message({{"op", "end"}, {"bundle", plan.model}})) {
    return make_error(Code::Network, "the member's connection was cut");
  }
  VALTZ_ASSIGN(Json ended, ctl.read_message());
  if (ended.contains("error")) {
    return make_error(Code::Network, jget<std::string>(ended, "error", ""));
  }
  if (progress) {
    progress(res.total, res.total);
  }
  res.bytes = landed.load();
  res.routes = used;
  res.seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started)
                    .count();
  return res;
}

}
