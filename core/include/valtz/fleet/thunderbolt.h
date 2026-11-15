#ifndef VALTZ_FLEET_THUNDERBOLT_H
#define VALTZ_FLEET_THUNDERBOLT_H

// THUNDERBOLT between fleet members (DESIGN §11c): what a DiT split
// across two Macs exchanges its partial sums over. Each member tells the
// others its Thunderbolt NETWORK -- the ports (macOS's "Thunderbolt N"),
// which have a Mac at their other end, their own addresses or the
// Thunderbolt Bridge's, the RDMA device of each that is up -- and a
// member PROBES the others' addresses from each of its own Thunderbolt
// interfaces: what answers through one is a cable between the two, and
// the ports at its ends are the ones that face each other (RDMA pairs
// them so, whatever their names).
//
// Link-local addresses (169.254/16, the bridge's and an unbridged port's
// alike) change at a reboot or a cable moved: read at run time, never
// kept.

#include "valtz/base/json.h"

#include <map>
#include <string>
#include <vector>

namespace valtz::fleet {

// A Thunderbolt network port of this Mac.
struct TbPort {
  std::string name;      // its interface, "en1"
  std::string mac;       // "36:d6:9f:8a:76:c0"
  bool        active = false;  // a Mac at its other end now
  std::string address;   // its own IPv4; "" in the bridge (or none)
  std::string rdma;      // its RDMA device, up ("rdma_en1"); "" none
  // The CABLE in it, as the Thunderbolt bus tells it: the Domain UUID of
  // this port's own bus and of the Mac at its other end ("" none), and
  // the link's speed (Gb/s, 0 not known: 80 a TB5 cable, 40, 20...). A
  // port facing a member's is the one whose peer is that port's domain
  // and back -- known whatever the bridge or the addresses, so two
  // cables between the same Macs are told apart.
  std::string domain;
  std::string peer;
  int         gbps = 0;
  // A port taken out of the Thunderbolt Bridge is often put in a bridge
  // of ITS OWN by macOS (bridge1, a port alone): that bridge, whose
  // address is `address` -- a socket for this cable is bound to it.
  std::string via;
  bool operator==(const TbPort&) const = default;
};

// This Mac's Thunderbolt network.
struct TbState {
  std::vector<TbPort> ports;
  // macOS's Thunderbolt Bridge, its address and the ports in it; ""
  // none, or none with an address.
  std::string              bridge;
  std::string              bridge_address;
  std::vector<std::string> bridged;
  // macOS has RDMA's verbs, and enabled (rdma_ctl: a device listed).
  bool rdma = false;
  // The fleet's port: a link proved by a connection there where an echo
  // is not answered (a firewall in stealth mode).
  int fleet_port = 0;

  // A port with a Mac at its other end, and an address to reach it by.
  bool linked() const;
  bool operator==(const TbState&) const = default;
};

// Read now: the ports by IOKit (an Ethernet interface whose provider is
// AppleThunderboltIPPort), their addresses and link state, the bridge's
// members and address, each port's RDMA device (librdma, looked up at
// run time: absent before macOS 26.2).
TbState thunderbolt_state();
Json to_json(const TbState&);
TbState tb_state_from_json(const Json&);

// A cable's path between this Mac and a member.
struct TbLink {
  std::string local_interface;   // what a socket binds to: bridge0, en1
  std::string local_address;
  std::string local_port;        // the port it leaves by; "" not known
  std::string remote_interface;  // the member's
  std::string remote_address;
  std::string remote_port;
  double      rtt_ms = 0;
  // The RDMA devices of the two ports, both up; "" where either is not.
  std::string local_rdma;
  std::string remote_rdma;
  // The cable's speed, Gb/s (0 not known): what a split weighs its
  // share of the partials on it by.
  int gbps = 0;
  bool rdma() const { return !local_rdma.empty() && !remote_rdma.empty(); }
};
Json to_json(const TbLink&);
TbLink tb_link_from_json(const Json&);

// One of the member's addresses answering through one of this Mac's
// interfaces.
struct TbAnswer {
  std::string local_interface;
  std::string remote_address;
  double      rtt_ms = 0;
};

// The links those answers make (pure: tested alone): ONE A CABLE where
// the two sides tell their ports' Thunderbolt domains -- each port facing
// the member's whose domain is its peer and back -- its socket the route
// an answer proved to that port or the bridge holding it (two cables in
// two bridges: one route, both cables). Without domains (a member
// before them), a link an answering interface: a port in a bridge faces
// the member's port whose address the bridge LEARNED through it
// (`learned`: a MAC address -> the bridge's port it was seen on), or --
// learned nowhere -- the one port of each side with a Mac at its end.
std::vector<TbLink> tb_links(const TbState& mine, const TbState& theirs,
                             const std::vector<TbAnswer>& answered,
                             const std::map<std::string, std::string>&
                                 learned);

// The links to a member now: from each of this Mac's addressed
// Thunderbolt interfaces an echo -- else a connection to its fleet port
// -- to each of its addresses, all at once, `timeout_ms` at most each
// (a cable answers in a few ms; a pair no cable joins waits it out).
std::vector<TbLink> probe_thunderbolt(const TbState& mine,
                                      const TbState& theirs,
                                      int timeout_ms = 200);

// This Mac's share of a split by the two GPUs (DESIGN §11c): each its
// cores, a GPU before M5's (no matrix cores) a quarter each; the member's
// as it says it is (`machine`: "gpu_cores", "matrix_cores" -- else its
// chip's name).
double gpu_share(int cores, bool matrix, const Json& machine);

// A TCP port free on every one of `addresses` now (a split's rank to
// listen on: bound a moment later, a port taken meanwhile fails the
// run); -1 none.
int free_tcp_port(const std::vector<std::string>& addresses);

// A bridge's ports and the MAC addresses it learned on each, as
// `ifconfig` prints them (pure: tested alone).
struct BridgeTable {
  std::vector<std::string>           members;
  std::map<std::string, std::string> learned;  // MAC -> member port
};
BridgeTable parse_bridge(const std::string& ifconfig_output);
// "36:5c:16:8d:fe:0" -> "36:5c:16:8d:fe:00"; "" if it is not one.
std::string normal_mac(std::string_view);

}

#endif
