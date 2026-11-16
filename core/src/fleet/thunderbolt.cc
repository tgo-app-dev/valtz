// This Mac's Thunderbolt network and the links to a member over it
// (thunderbolt.h, DESIGN §11c).

#include "valtz/fleet/thunderbolt.h"

#include "valtz/base/log.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_media.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <format>
#include <functional>
#include <set>
#include <sstream>
#include <thread>

namespace valtz::fleet {

namespace {

// An IORegistry entry's string property; "" none.
std::string
string_property(io_registry_entry_t e, CFStringRef key)
{
  std::string out;
  if (CFTypeRef v = IORegistryEntryCreateCFProperty(e, key,
                                                    kCFAllocatorDefault, 0)) {
    char buf[128]{};
    if (CFGetTypeID(v) == CFStringGetTypeID() &&
        CFStringGetCString(static_cast<CFStringRef>(v), buf, sizeof buf,
                           kCFStringEncodingUTF8)) {
      out = buf;
    }
    CFRelease(v);
  }
  return out;
}

// An IORegistry entry's integer property; -1 none.
long long
number_property(io_registry_entry_t e, CFStringRef key)
{
  long long out = -1;
  if (CFTypeRef v = IORegistryEntryCreateCFProperty(e, key,
                                                    kCFAllocatorDefault, 0)) {
    if (CFGetTypeID(v) == CFNumberGetTypeID()) {
      CFNumberGetValue(static_cast<CFNumberRef>(v), kCFNumberLongLongType,
                       &out);
    }
    CFRelease(v);
  }
  return out;
}

// The cable in a Thunderbolt IP port: up from its Ethernet interface to
// the bus's local node (its Domain UUID) and controller, then through the
// controller's tree -- the Mac at the other end is a node there with a
// Domain UUID of its own, and the lane adapter that links to it its
// "Link Bandwidth" in 100 Mb/s (system_profiler's "Speed: 80 Gb/s": 800;
// a 20 Gb/s cable 200). Not its "Current Link Speed": read on two Mac
// Studios it said 2 on the 80 Gb/s cable and 8 on the 20 -- and 8 on a
// port with no cable at all.
struct Cable {
  std::string domain, peer;
  int gbps = 0;
};

Cable
cable_of(io_object_t ethernet)
{
  Cable c;
  io_registry_entry_t e = ethernet;
  IOObjectRetain(e);
  io_registry_entry_t controller = 0;
  for (int depth = 0; depth < 8 && e != 0; ++depth) {
    io_registry_entry_t parent = 0;
    const kern_return_t kr =
        IORegistryEntryGetParentEntry(e, kIOServicePlane, &parent);
    IOObjectRelease(e);
    e = kr == KERN_SUCCESS ? parent : 0;
    if (e == 0) {
      break;
    }
    io_name_t cls{};
    IOObjectGetClass(e, cls);
    const std::string_view k(cls);
    if (k.find("LocalNode") != std::string_view::npos) {
      c.domain = string_property(e, CFSTR("Domain UUID"));
    } else if (k.find("ThunderboltController") != std::string_view::npos) {
      controller = e;
      e = 0;
      break;
    }
  }
  if (e != 0) {
    IOObjectRelease(e);
  }
  if (controller == 0) {
    return c;
  }
  io_iterator_t it = 0;
  if (IORegistryEntryCreateIterator(controller, kIOServicePlane,
                                    kIORegistryIterateRecursively,
                                    &it) == KERN_SUCCESS) {
    for (io_object_t n = IOIteratorNext(it); n != 0; n = IOIteratorNext(it)) {
      const std::string d = string_property(n, CFSTR("Domain UUID"));
      if (!d.empty() && d != c.domain && c.peer.empty()) {
        c.peer = d;
      }
      io_name_t cls{};
      IOObjectGetClass(n, cls);
      if (std::string_view(cls) == "IOThunderboltPort" &&
          number_property(n, CFSTR("Adapter Type")) == 1) {
        const long long bw = number_property(n, CFSTR("Link Bandwidth"));
        if (bw > 0) {
          c.gbps = std::max(c.gbps, static_cast<int>(bw / 10));
        }
      }
      IOObjectRelease(n);
    }
    IOObjectRelease(it);
  }
  IOObjectRelease(controller);
  if (c.peer.empty()) {
    c.gbps = 0;
  }
  return c;
}

struct Found {
  std::string name;
  Cable       cable;
};

// The Ethernet interfaces whose provider is a Thunderbolt IP port, by
// BSD name, each with its cable.
std::vector<Found>
thunderbolt_interfaces()
{
  std::vector<Found> out;
  io_iterator_t it = 0;
  if (IOServiceGetMatchingServices(kIOMainPortDefault,
                                   IOServiceMatching("IOEthernetInterface"),
                                   &it) != KERN_SUCCESS) {
    return out;
  }
  for (io_object_t s = IOIteratorNext(it); s != 0; s = IOIteratorNext(it)) {
    io_registry_entry_t parent = 0;
    bool tb = false;
    if (IORegistryEntryGetParentEntry(s, kIOServicePlane, &parent) ==
        KERN_SUCCESS) {
      io_name_t cls{};
      if (IOObjectGetClass(parent, cls) == KERN_SUCCESS) {
        tb = std::string_view(cls).find("Thunderbolt") !=
             std::string_view::npos;
      }
      IOObjectRelease(parent);
    }
    if (tb) {
      const std::string name = string_property(s, CFSTR("BSD Name"));
      if (!name.empty()) {
        out.push_back({name, cable_of(s)});
      }
    }
    IOObjectRelease(s);
  }
  IOObjectRelease(it);
  std::ranges::sort(out, {}, &Found::name);
  return out;
}

// A cable in it, a Mac at its other end.
bool
link_active(const std::string& name)
{
  const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (fd < 0) {
    return false;
  }
  ifmediareq mr{};
  std::strncpy(mr.ifm_name, name.c_str(), sizeof mr.ifm_name - 1);
  const bool ok = ::ioctl(fd, SIOCGIFMEDIA, &mr) == 0;
  ::close(fd);
  return ok && (mr.ifm_status & IFM_AVALID) && (mr.ifm_status & IFM_ACTIVE);
}

// Each interface's IPv4 and link address.
struct IfAddrs {
  std::map<std::string, std::string> ipv4;
  std::map<std::string, std::string> mac;
};

IfAddrs
interface_addresses()
{
  IfAddrs out;
  ifaddrs* head = nullptr;
  if (::getifaddrs(&head) != 0) {
    return out;
  }
  for (ifaddrs* a = head; a != nullptr; a = a->ifa_next) {
    if (a->ifa_addr == nullptr) {
      continue;
    }
    const std::string name = a->ifa_name;
    if (a->ifa_addr->sa_family == AF_INET && !out.ipv4.contains(name)) {
      char buf[INET_ADDRSTRLEN]{};
      const auto* in = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
      if (::inet_ntop(AF_INET, &in->sin_addr, buf, sizeof buf)) {
        out.ipv4[name] = buf;
      }
    } else if (a->ifa_addr->sa_family == AF_LINK) {
      const auto* dl = reinterpret_cast<const sockaddr_dl*>(a->ifa_addr);
      if (dl->sdl_alen == 6) {
        const auto* b =
            reinterpret_cast<const unsigned char*>(LLADDR(dl));
        out.mac[name] = std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:"
                                    "{:02x}", b[0], b[1], b[2], b[3], b[4],
                                    b[5]);
      }
    }
  }
  ::freeifaddrs(head);
  return out;
}

// What `ifconfig <name>` prints (a bridge's members and learned
// addresses have no public interface).
std::string
ifconfig_of(const std::string& name)
{
  std::string out;
  const std::string cmd = "/sbin/ifconfig " + name + " 2>/dev/null";
  if (FILE* p = ::popen(cmd.c_str(), "r")) {
    std::array<char, 4096> buf{};
    std::size_t n = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), p)) > 0) {
      out.append(buf.data(), n);
    }
    ::pclose(p);
  }
  return out;
}

// RDMA's verbs, looked up at run time (macOS 26.2 and later; the plugin
// links them weakly, tp-rdma.cc): the devices whose port is up.
struct Verbs {
  using List = void** (*)(int*);
  using Free = void (*)(void**);
  using Name = const char* (*)(void*);
  using Open = void* (*)(void*);
  using Close = int (*)(void*);
  using Port = int (*)(void*, std::uint8_t, void*);
  List list = nullptr;
  Free free = nullptr;
  Name name = nullptr;
  Open open = nullptr;
  Close close = nullptr;
  Port port = nullptr;
  bool ok() const { return list && free && name && open && close && port; }
};

const Verbs&
verbs()
{
  static const Verbs v = [] {
    Verbs v;
    void* h = ::dlopen("/usr/lib/librdma.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (h == nullptr) {
      return v;
    }
    v.list = reinterpret_cast<Verbs::List>(
        ::dlsym(h, "ibv_get_device_list"));
    v.free = reinterpret_cast<Verbs::Free>(
        ::dlsym(h, "ibv_free_device_list"));
    v.name = reinterpret_cast<Verbs::Name>(
        ::dlsym(h, "ibv_get_device_name"));
    v.open = reinterpret_cast<Verbs::Open>(::dlsym(h, "ibv_open_device"));
    v.close = reinterpret_cast<Verbs::Close>(
        ::dlsym(h, "ibv_close_device"));
    v.port = reinterpret_cast<Verbs::Port>(::dlsym(h, "ibv_query_port"));
    return v;
  }();
  return v;
}

// Every RDMA device listed (`any`), and those whose port 1 is up.
std::set<std::string>
rdma_devices_up(bool& any)
{
  std::set<std::string> out;
  any = false;
  const Verbs& v = verbs();
  if (!v.ok()) {
    return out;
  }
  int n = 0;
  void** list = v.list(&n);
  any = list != nullptr && n > 0;
  for (int i = 0; i < n && list != nullptr; ++i) {
    void* ctx = v.open(list[i]);
    if (ctx == nullptr) {
      continue;
    }
    // ibv_port_attr's first field is its state (IBV_PORT_ACTIVE, 4); its
    // compat query fills the struct, which is well under 256 bytes.
    alignas(8) std::array<std::uint8_t, 256> attr{};
    if (v.port(ctx, 1, attr.data()) == 0) {
      int state = 0;
      std::memcpy(&state, attr.data(), sizeof state);
      if (state == 4) {
        out.insert(v.name(list[i]));
      }
    }
    v.close(ctx);
  }
  if (list != nullptr) {
    v.free(list);
  }
  return out;
}

std::uint16_t
icmp_checksum(const std::uint8_t* p, std::size_t n)
{
  std::uint32_t sum = 0;
  for (std::size_t i = 0; i + 1 < n; i += 2) {
    sum += static_cast<std::uint32_t>(p[i] << 8 | p[i + 1]);
  }
  if (n & 1) {
    sum += static_cast<std::uint32_t>(p[n - 1] << 8);
  }
  while (sum >> 16) {
    sum = (sum & 0xffff) + (sum >> 16);
  }
  return static_cast<std::uint16_t>(~sum);
}

// An echo to `address` leaving by `interface`: its round trip in ms, or
// negative. Unprivileged (a datagram ICMP socket).
double
echo(const std::string& interface, const std::string& address,
     int timeout_ms, std::uint16_t seq)
{
  sockaddr_in to{};
  to.sin_len = sizeof to;
  to.sin_family = AF_INET;
  if (::inet_pton(AF_INET, address.c_str(), &to.sin_addr) != 1) {
    return -1;
  }
  const unsigned idx = ::if_nametoindex(interface.c_str());
  const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);
  if (fd < 0 || idx == 0) {
    if (fd >= 0) {
      ::close(fd);
    }
    return -1;
  }
  ::setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx);
  const std::uint16_t id = static_cast<std::uint16_t>(::getpid());
  std::array<std::uint8_t, 16> pkt{};
  pkt[0] = 8;  // echo request
  pkt[4] = static_cast<std::uint8_t>(id >> 8);
  pkt[5] = static_cast<std::uint8_t>(id);
  pkt[6] = static_cast<std::uint8_t>(seq >> 8);
  pkt[7] = static_cast<std::uint8_t>(seq);
  std::memcpy(pkt.data() + 8, "valtz-tb", 8);
  const std::uint16_t ck = icmp_checksum(pkt.data(), pkt.size());
  pkt[2] = static_cast<std::uint8_t>(ck >> 8);
  pkt[3] = static_cast<std::uint8_t>(ck);
  const auto t0 = std::chrono::steady_clock::now();
  if (::sendto(fd, pkt.data(), pkt.size(), 0,
               reinterpret_cast<const sockaddr*>(&to), sizeof to) < 0) {
    ::close(fd);
    return -1;
  }
  double rtt = -1;
  for (;;) {
    const auto left = timeout_ms -
                      std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    if (left <= 0) {
      break;
    }
    pollfd pf{fd, POLLIN, 0};
    if (::poll(&pf, 1, static_cast<int>(left)) <= 0) {
      break;
    }
    std::array<std::uint8_t, 512> in{};
    sockaddr_in from{};
    socklen_t fl = sizeof from;
    const ssize_t n = ::recvfrom(fd, in.data(), in.size(), 0,
                                 reinterpret_cast<sockaddr*>(&from), &fl);
    if (n <= 0) {
      break;
    }
    // macOS hands the IP header over with the message.
    std::size_t at = 0;
    if ((in[0] >> 4) == 4) {
      at = static_cast<std::size_t>(in[0] & 15) * 4;
    }
    if (static_cast<std::size_t>(n) < at + 8 ||
        from.sin_addr.s_addr != to.sin_addr.s_addr) {
      continue;
    }
    const std::uint8_t* r = in.data() + at;
    const std::uint16_t rseq = static_cast<std::uint16_t>(r[6] << 8 | r[7]);
    if (r[0] == 0 && rseq == seq) {
      rtt = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - t0)
                .count();
      break;
    }
  }
  ::close(fd);
  return rtt;
}

// A connection to `address`:`port` leaving by `interface`, made and let
// go: its time in ms, or negative.
double
reach(const std::string& interface, const std::string& address, int port,
      int timeout_ms)
{
  sockaddr_in to{};
  to.sin_len = sizeof to;
  to.sin_family = AF_INET;
  to.sin_port = htons(static_cast<std::uint16_t>(port));
  const unsigned idx = ::if_nametoindex(interface.c_str());
  if (port <= 0 || idx == 0 ||
      ::inet_pton(AF_INET, address.c_str(), &to.sin_addr) != 1) {
    return -1;
  }
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  ::setsockopt(fd, IPPROTO_IP, IP_BOUND_IF, &idx, sizeof idx);
  ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
  const auto t0 = std::chrono::steady_clock::now();
  double ms = -1;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&to), sizeof to) ==
          0 ||
      errno == EINPROGRESS) {
    pollfd pf{fd, POLLOUT, 0};
    int err = 1;
    socklen_t el = sizeof err;
    if (::poll(&pf, 1, timeout_ms) == 1 &&
        ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0) {
      ms = std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0)
               .count();
    }
  }
  ::close(fd);
  return ms;
}

// The interface of `s` that owns `address`: a port, or the bridge.
std::string
owner_of(const TbState& s, const std::string& address)
{
  if (!s.bridge_address.empty() && s.bridge_address == address) {
    return s.bridge;
  }
  for (const auto& p : s.ports) {
    if (!p.address.empty() && p.address == address) {
      return p.name;
    }
  }
  return {};
}

// A port by its interface -- or the bridge of its own it is in.
const TbPort*
port_of(const TbState& s, const std::string& name)
{
  for (const auto& p : s.ports) {
    if (p.name == name || (!p.via.empty() && p.via == name)) {
      return &p;
    }
  }
  return nullptr;
}

// The one bridged port of `s` with a Mac at its end; "" none or several.
std::string
lone_bridged_active(const TbState& s)
{
  std::string one;
  for (const auto& p : s.ports) {
    const bool in = std::ranges::find(s.bridged, p.name) != s.bridged.end();
    if (in && p.active) {
      if (!one.empty()) {
        return {};
      }
      one = p.name;
    }
  }
  return one;
}

}  // namespace

bool
TbState::linked() const
{
  for (const auto& p : ports) {
    const bool in = std::ranges::find(bridged, p.name) != bridged.end();
    if (p.active && (!p.address.empty() || (in && !bridge_address.empty()))) {
      return true;
    }
  }
  return false;
}

std::string
normal_mac(std::string_view s)
{
  std::array<unsigned, 6> b{};
  std::size_t at = 0;
  for (int i = 0; i < 6; ++i) {
    std::size_t end = s.find(':', at);
    if (i == 5) {
      end = s.size();
    }
    if (end == std::string_view::npos || end == at || end - at > 2) {
      return {};
    }
    unsigned v = 0;
    for (std::size_t k = at; k < end; ++k) {
      const char c = s[k];
      const int d = c >= '0' && c <= '9'   ? c - '0'
                    : c >= 'a' && c <= 'f' ? c - 'a' + 10
                    : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                           : -1;
      if (d < 0) {
        return {};
      }
      v = v * 16 + static_cast<unsigned>(d);
    }
    b[static_cast<std::size_t>(i)] = v;
    at = end + 1;
  }
  return std::format("{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}", b[0], b[1],
                     b[2], b[3], b[4], b[5]);
}

BridgeTable
parse_bridge(const std::string& text)
{
  BridgeTable t;
  std::istringstream in(text);
  std::string line;
  bool cache = false;
  while (std::getline(in, line)) {
    std::istringstream words(line);
    std::string first;
    words >> first;
    if (first == "member:") {
      std::string name;
      words >> name;
      if (!name.empty()) {
        t.members.push_back(name);
      }
      cache = false;
      continue;
    }
    if (first == "Address" && line.find("cache:") != std::string::npos) {
      cache = true;
      continue;
    }
    if (!cache) {
      continue;
    }
    // "36:5c:16:8d:fe:0 Vlan1 en1 1194 flags=0<>"
    std::string vlan, port;
    words >> vlan >> port;
    const std::string mac = normal_mac(first);
    if (mac.empty() || port.empty()) {
      cache = !first.empty() && first.find(':') != std::string::npos;
      continue;
    }
    t.learned[mac] = port;
  }
  return t;
}

TbState
thunderbolt_state()
{
  TbState s;
  const auto found = thunderbolt_interfaces();
  if (found.empty()) {
    return s;
  }
  std::vector<std::string> names;
  for (const auto& f : found) {
    names.push_back(f.name);
  }
  const IfAddrs addrs = interface_addresses();
  bool any_rdma = false;
  const std::set<std::string> up = rdma_devices_up(any_rdma);
  s.rdma = any_rdma;
  for (const auto& f : found) {
    const std::string& n = f.name;
    TbPort p;
    p.name = n;
    p.domain = f.cable.domain;
    p.peer = f.cable.peer;
    p.gbps = f.cable.gbps;
    if (auto it = addrs.mac.find(n); it != addrs.mac.end()) {
      p.mac = it->second;
    }
    if (auto it = addrs.ipv4.find(n); it != addrs.ipv4.end()) {
      p.address = it->second;
    }
    p.active = link_active(n);
    if (up.contains("rdma_" + n)) {
      p.rdma = "rdma_" + n;
    }
    s.ports.push_back(std::move(p));
  }
  // The bridges holding Thunderbolt ports: macOS's Thunderbolt Bridge
  // (bridge0 as a rule) THE bridge -- the one with the most of them --
  // and any other's port reached by that bridge's address (`via`): a
  // port taken out of the Thunderbolt Bridge lands in one of its own.
  struct Held {
    std::string              name, ip;
    std::vector<std::string> ours;
  };
  std::vector<Held> bridges;
  for (const auto& [name, ip] : addrs.ipv4) {
    if (!name.starts_with("bridge")) {
      continue;
    }
    const BridgeTable t = parse_bridge(ifconfig_of(name));
    Held h{name, ip, {}};
    for (const auto& m : t.members) {
      if (std::ranges::find(names, m) != names.end()) {
        h.ours.push_back(m);
      }
    }
    if (!h.ours.empty()) {
      bridges.push_back(std::move(h));
    }
  }
  std::ranges::stable_sort(bridges, [](const Held& a, const Held& b) {
    return a.ours.size() > b.ours.size();
  });
  for (std::size_t i = 0; i < bridges.size(); ++i) {
    if (i == 0) {
      s.bridge = bridges[i].name;
      s.bridge_address = bridges[i].ip;
      s.bridged = bridges[i].ours;
      continue;
    }
    for (auto& p : s.ports) {
      if (std::ranges::find(bridges[i].ours, p.name) !=
              bridges[i].ours.end() &&
          p.address.empty()) {
        p.address = bridges[i].ip;
        p.via = bridges[i].name;
      }
    }
  }
  return s;
}

Json
to_json(const TbState& s)
{
  Json ports = Json::array();
  for (const auto& p : s.ports) {
    ports.push_back({{"name", p.name},
                     {"mac", p.mac},
                     {"active", p.active},
                     {"address", p.address},
                     {"via", p.via},
                     {"rdma", p.rdma},
                     {"domain", p.domain},
                     {"peer", p.peer},
                     {"gbps", p.gbps}});
  }
  return {{"ports", std::move(ports)},
          {"bridge", s.bridge},
          {"bridge_address", s.bridge_address},
          {"bridged", s.bridged},
          {"rdma", s.rdma},
          {"fleet_port", s.fleet_port}};
}

TbState
tb_state_from_json(const Json& j)
{
  TbState s;
  for (const auto& p : jget(j, "ports", Json::array())) {
    TbPort port;
    port.name = jget<std::string>(p, "name", "");
    port.mac = normal_mac(jget<std::string>(p, "mac", ""));
    port.active = jget(p, "active", false);
    port.address = jget<std::string>(p, "address", "");
    port.via = jget<std::string>(p, "via", "");
    port.rdma = jget<std::string>(p, "rdma", "");
    port.domain = jget<std::string>(p, "domain", "");
    port.peer = jget<std::string>(p, "peer", "");
    port.gbps = jget(p, "gbps", 0);
    if (!port.name.empty()) {
      s.ports.push_back(std::move(port));
    }
  }
  s.bridge = jget<std::string>(j, "bridge", "");
  s.bridge_address = jget<std::string>(j, "bridge_address", "");
  for (const auto& b : jget(j, "bridged", Json::array())) {
    if (b.is_string()) {
      s.bridged.push_back(b.get<std::string>());
    }
  }
  s.rdma = jget(j, "rdma", false);
  s.fleet_port = jget(j, "fleet_port", 0);
  return s;
}

Json
to_json(const TbLink& l)
{
  return {{"local_interface", l.local_interface},
          {"local_address", l.local_address},
          {"local_port", l.local_port},
          {"remote_interface", l.remote_interface},
          {"remote_address", l.remote_address},
          {"remote_port", l.remote_port},
          {"rtt_ms", l.rtt_ms},
          {"local_rdma", l.local_rdma},
          {"remote_rdma", l.remote_rdma},
          {"gbps", l.gbps}};
}

TbLink
tb_link_from_json(const Json& j)
{
  TbLink l;
  l.local_interface = jget<std::string>(j, "local_interface", "");
  l.local_address = jget<std::string>(j, "local_address", "");
  l.local_port = jget<std::string>(j, "local_port", "");
  l.remote_interface = jget<std::string>(j, "remote_interface", "");
  l.remote_address = jget<std::string>(j, "remote_address", "");
  l.remote_port = jget<std::string>(j, "remote_port", "");
  l.rtt_ms = jget(j, "rtt_ms", 0.0);
  l.local_rdma = jget<std::string>(j, "local_rdma", "");
  l.remote_rdma = jget<std::string>(j, "remote_rdma", "");
  l.gbps = jget(j, "gbps", 0);
  return l;
}

namespace {

bool
bridged_in(const TbState& s, const std::string& port)
{
  return !s.bridge.empty() &&
         std::ranges::find(s.bridged, port) != s.bridged.end();
}

// The links one a cable, by the ports' Thunderbolt domains; empty where
// either side tells none (or no cable answers).
std::vector<TbLink>
cables(const TbState& mine, const TbState& theirs,
       const std::vector<TbAnswer>& answered)
{
  std::vector<TbLink> out;
  for (const auto& lp : mine.ports) {
    if (!lp.active || lp.domain.empty() || lp.peer.empty()) {
      continue;
    }
    const TbPort* rp = nullptr;
    for (const auto& p : theirs.ports) {
      if (p.domain == lp.peer && (p.peer.empty() || p.peer == lp.domain)) {
        rp = &p;
        break;
      }
    }
    if (rp == nullptr) {
      continue;
    }
    // Its socket's route: an answer from this port, or the bridge holding
    // it, to the member's port, or the bridge holding that -- the
    // fastest, the answers coming sorted.
    const TbAnswer* route = nullptr;
    for (const auto& a : answered) {
      const bool here = a.local_interface == lp.name ||
                        (!lp.via.empty() && a.local_interface == lp.via) ||
                        (bridged_in(mine, lp.name) &&
                         a.local_interface == mine.bridge);
      // There: that port, the member's bridge holding it, or a bridge of
      // its own it shares with another cable's port -- whose address
      // either port owns (two cables in one bridge are one address).
      const std::string rif = owner_of(theirs, a.remote_address);
      const bool there = rif == rp->name ||
                         (bridged_in(theirs, rp->name) &&
                          rif == theirs.bridge) ||
                         (!rp->via.empty() && !rp->address.empty() &&
                          a.remote_address == rp->address);
      if (here && there) {
        route = &a;
        break;
      }
    }
    if (route == nullptr) {
      continue;
    }
    TbLink l;
    l.local_interface = route->local_interface;
    l.local_address = route->local_interface == mine.bridge
                          ? mine.bridge_address
                          : lp.address;
    l.local_port = lp.name;
    l.remote_interface = owner_of(theirs, route->remote_address);
    if (l.remote_interface != rp->name && !rp->via.empty() &&
        route->remote_address == rp->address) {
      l.remote_interface = rp->via;   // its bridge, another port's too
    }
    l.remote_address = route->remote_address;
    l.remote_port = rp->name;
    l.rtt_ms = route->rtt_ms;
    if (!lp.rdma.empty() && !rp->rdma.empty()) {
      l.local_rdma = lp.rdma;
      l.remote_rdma = rp->rdma;
    }
    l.gbps = rp->gbps > 0 && lp.gbps > 0 ? std::min(lp.gbps, rp->gbps)
                                         : std::max(lp.gbps, rp->gbps);
    out.push_back(std::move(l));
  }
  // The fastest cable first: a split's first link carries its setup.
  std::ranges::stable_sort(out, std::greater<>{}, &TbLink::gbps);
  return out;
}

}  // namespace

std::vector<TbLink>
tb_links(const TbState& mine, const TbState& theirs,
         const std::vector<TbAnswer>& answered,
         const std::map<std::string, std::string>& learned)
{
  if (auto c = cables(mine, theirs, answered); !c.empty()) {
    return c;
  }
  std::vector<TbLink> out;
  std::set<std::string> used_local, used_remote;
  for (const auto& a : answered) {
    // Each interface and each address one link: the first answer.
    if (used_local.contains(a.local_interface) ||
        used_remote.contains(a.remote_address)) {
      continue;
    }
    const std::string rif = owner_of(theirs, a.remote_address);
    if (rif.empty()) {
      continue;
    }
    TbLink l;
    l.local_interface = a.local_interface;
    l.remote_interface = rif;
    l.remote_address = a.remote_address;
    l.rtt_ms = a.rtt_ms;
    const bool local_bridge = !mine.bridge.empty() &&
                              a.local_interface == mine.bridge;
    const bool remote_bridge = !theirs.bridge.empty() &&
                               rif == theirs.bridge;
    l.local_address = local_bridge ? mine.bridge_address : [&] {
      const TbPort* p = port_of(mine, a.local_interface);
      return p ? p->address : std::string();
    }();
    // The ports at the two ends: an unbridged interface is its port; in
    // a bridge, the port that learned the other's port's address -- or
    // the lone one with a cable in it.
    if (!local_bridge) {
      const TbPort* p = port_of(mine, a.local_interface);
      l.local_port = p ? p->name : a.local_interface;
    }
    if (!remote_bridge) {
      l.remote_port = rif;
    }
    if (local_bridge) {
      for (const auto& rp : theirs.ports) {
        if (!rp.active || rp.mac.empty() ||
            (!l.remote_port.empty() && rp.name != l.remote_port)) {
          continue;
        }
        if (auto it = learned.find(rp.mac); it != learned.end() &&
            std::ranges::find(mine.bridged, it->second) !=
                mine.bridged.end()) {
          l.local_port = it->second;
          if (l.remote_port.empty()) {
            l.remote_port = rp.name;
          }
          break;
        }
      }
      if (l.local_port.empty()) {
        l.local_port = lone_bridged_active(mine);
      }
    }
    if (l.remote_port.empty() && remote_bridge) {
      l.remote_port = lone_bridged_active(theirs);
    }
    const TbPort* lp = port_of(mine, l.local_port);
    const TbPort* rp = port_of(theirs, l.remote_port);
    if (lp && rp && !lp->rdma.empty() && !rp->rdma.empty()) {
      l.local_rdma = lp->rdma;
      l.remote_rdma = rp->rdma;
    }
    used_local.insert(l.local_interface);
    used_remote.insert(l.remote_address);
    out.push_back(std::move(l));
  }
  return out;
}

int
free_tcp_port(const std::vector<std::string>& addresses)
{
  auto bind_at = [](const std::string& a, int port) -> int {
    sockaddr_in sa{};
    sa.sin_len = sizeof sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(static_cast<std::uint16_t>(port));
    if (::inet_pton(AF_INET, a.c_str(), &sa.sin_addr) != 1) {
      return -1;
    }
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) != 0) {
      ::close(fd);
      return -1;
    }
    socklen_t l = sizeof sa;
    ::getsockname(fd, reinterpret_cast<sockaddr*>(&sa), &l);
    ::close(fd);
    return ntohs(sa.sin_port);
  };
  for (int attempt = 0; attempt < 8 && !addresses.empty(); ++attempt) {
    const int port = bind_at(addresses.front(), 0);
    if (port <= 0) {
      return -1;
    }
    bool all = true;
    for (std::size_t i = 1; i < addresses.size() && all; ++i) {
      all = bind_at(addresses[i], port) == port;
    }
    if (all) {
      return port;
    }
  }
  return -1;
}

// This Mac's share of a split by the two GPUs: each its cores, a GPU
// before M5's a quarter of one (no matrix cores: a DiT's GEMMs run about
// four times slower a core). A member that does not say whether it has
// them is read by its chip's name.
double
gpu_share(int cores, bool matrix, const Json& machine)
{
  auto weight = [](int n, bool m) {
    return std::max(1, n) * (m ? 1.0 : 0.25);
  };
  bool theirs_matrix = false;
  if (machine.contains("matrix_cores")) {
    theirs_matrix = jget(machine, "matrix_cores", false);
  } else {
    const auto chip = jget<std::string>(machine, "chip", "");
    const auto at = chip.find(" M");
    theirs_matrix = at != std::string::npos &&
                    std::atoi(chip.c_str() + at + 2) >= 5;
  }
  const double mine = weight(cores, matrix);
  const double theirs =
      weight(jget(machine, "gpu_cores", cores), theirs_matrix);
  return mine / (mine + theirs);
}

std::pair<int, int>
split_rows(int heads, int ffn, double frac, int head_grain, int ffn_grain)
{
  const int g = std::max(1, head_grain);
  const int fg = std::max(1, ffn_grain);
  // What does not divide evenly goes to the FASTER Mac: rounded up for
  // the larger share, down for the smaller (H3's 56 heads in fours at
  // 2:1 are 40:16, not 36:20 -- the slower Mac given 7% more than its
  // part set the pace, the faster one waiting at every exchange). An
  // even split rounds to nearest; an exact one stays exact.
  const auto whole = [frac](double x) {
    constexpr double kEps = 1e-9;
    return frac > 0.5   ? static_cast<int>(std::ceil(x - kEps))
           : frac < 0.5 ? static_cast<int>(std::floor(x + kEps))
                        : static_cast<int>(std::lround(x));
  };
  const int h0 =
      std::clamp(whole(heads * frac / g) * g, g, heads - g);
  const int f0 =
      ffn % heads == 0 && (ffn / heads) % fg == 0
          ? ffn / heads * h0
          : std::clamp(whole(static_cast<double>(ffn) * h0 / heads / fg) *
                           fg,
                       fg, ffn - fg);
  return {h0, f0};
}

std::vector<TbLink>
probe_thunderbolt(const TbState& mine, const TbState& theirs,
                  int timeout_ms)
{
  // This Mac's interfaces with an address on Thunderbolt, the member's
  // addresses there.
  std::vector<std::string> locals;
  if (!mine.bridge.empty() && !mine.bridge_address.empty()) {
    locals.push_back(mine.bridge);
  }
  for (const auto& p : mine.ports) {
    if (!p.address.empty() && p.active) {
      // A port in a bridge of its own: that bridge holds the address.
      locals.push_back(p.via.empty() ? p.name : p.via);
    }
  }
  std::vector<std::string> remotes;
  if (!theirs.bridge_address.empty()) {
    remotes.push_back(theirs.bridge_address);
  }
  for (const auto& p : theirs.ports) {
    if (!p.address.empty() && p.active) {
      remotes.push_back(p.address);
    }
  }
  if (locals.empty() || remotes.empty()) {
    return {};
  }
  struct Try {
    std::string local, remote;
    double rtt = -1;
  };
  std::vector<Try> tries;
  for (const auto& l : locals) {
    for (const auto& r : remotes) {
      tries.push_back({l, r});
    }
  }
  std::vector<std::thread> threads;
  for (std::size_t i = 0; i < tries.size(); ++i) {
    threads.emplace_back([&, i] {
      Try& t = tries[i];
      t.rtt = echo(t.local, t.remote, timeout_ms,
                   static_cast<std::uint16_t>(0x7400 + i));
      if (t.rtt < 0) {
        t.rtt = reach(t.local, t.remote, theirs.fleet_port, timeout_ms);
      }
    });
  }
  for (auto& t : threads) {
    t.join();
  }
  auto answers = [&] {
    std::vector<TbAnswer> a;
    for (const auto& t : tries) {
      if (t.rtt >= 0) {
        a.push_back({t.local, t.remote, t.rtt});
      }
    }
    return a;
  };
  std::vector<TbAnswer> answered = answers();
  // A cable the buses pair that no answer routed -- an echo lost to an
  // address not learned yet, now and then -- asked once more: the pairs
  // that did not answer, again, before a split goes on fewer cables.
  std::size_t paired = 0;
  for (const auto& lp : mine.ports) {
    paired += lp.active && !lp.peer.empty() &&
              std::ranges::any_of(theirs.ports, [&](const TbPort& p) {
                return p.domain == lp.peer;
              });
  }
  if (paired > 0 && cables(mine, theirs, answered).size() < paired) {
    threads.clear();
    for (std::size_t i = 0; i < tries.size(); ++i) {
      if (tries[i].rtt >= 0) {
        continue;
      }
      threads.emplace_back([&, i] {
        Try& t = tries[i];
        t.rtt = echo(t.local, t.remote, timeout_ms,
                     static_cast<std::uint16_t>(0x7600 + i));
      });
    }
    for (auto& t : threads) {
      t.join();
    }
    answered = answers();
  }
  // The fastest first: a cable answers in well under a millisecond.
  std::ranges::stable_sort(answered, {}, &TbAnswer::rtt_ms);
  std::map<std::string, std::string> learned;
  if (!mine.bridge.empty()) {
    learned = parse_bridge(ifconfig_of(mine.bridge)).learned;
  }
  return tb_links(mine, theirs, answered, learned);
}

}
