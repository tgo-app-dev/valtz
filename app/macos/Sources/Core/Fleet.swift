import Foundation
import SwiftUI

// THE FLEET (DESIGN §11): this Mac's place in a fleet of Valtz Macs on
// its network, as the core keeps it (Controller::fleet_status) -- the
// Settings › Fleet page, the job this Mac serves for a member, and the
// offers it is asked to answer.

struct FleetStatus: Decodable, Sendable, Equatable {
    var enabled: Bool
    var config: FleetConfig?
    var port: Int?
    var browsing: Bool?
    /// macOS's Local Network permission, as browsing found it: allowed,
    /// denied, or "" -- not known (nothing browsed since launch).
    var localNetwork: String?
    var members: [FleetMember]?
    var fleets: [FleetSeen]?
    var serving: FleetJob?
    var asking: [FleetJob]?
    /// This Mac as the others see it.
    var me: FleetSelf?
    /// The fleet's tasks, each with the models a job of it names.
    var tasks: [FleetTask]?
    /// This Mac's offload policies and what it withholds: read from the
    /// raw JSON (FleetPolicies), not decoded.
    var policies = FleetPolicies()
    /// Thunderbolt between the members and the pair (DESIGN §11c): read
    /// from the raw JSON too -- its links are keyed by member ids, its
    /// reasons are core messages.
    var thunderbolt = FleetThunderbolt()
    var pair: FleetPair?

    enum CodingKeys: String, CodingKey {
        case enabled, config, port, browsing, localNetwork, members, fleets,
             serving, asking, tasks, me = "self"
    }
}

/// THUNDERBOLT between fleet members (DESIGN §11c): this Mac's ports,
/// the cables found to each member -- the ports facing each other at its
/// ends, RDMA where both have it up.
struct FleetThunderbolt: Equatable, Sendable {
    struct Port: Equatable, Sendable {
        var name: String
        var active: Bool
        var address: String
        var rdma: String
    }
    struct Link: Equatable, Sendable {
        var localInterface: String
        var localAddress: String
        var localPort: String
        var remoteAddress: String
        var remotePort: String
        var rttMs: Double
        var localRdma: String
        var remoteRdma: String
        var rdma: Bool { !localRdma.isEmpty && !remoteRdma.isEmpty }
    }
    var ports: [Port] = []
    var bridge = ""
    var bridgeAddress = ""
    var bridged: [String] = []
    /// macOS has RDMA here, enabled.
    var rdma = false
    /// A DiT may be split here (vpipe and the plugin have it).
    var splits = false
    /// Member id -> the cables to it.
    var links: [String: [Link]] = [:]

    init() {}

    init(_ j: [String: Any]) {
        let me = j["self"] as? [String: Any] ?? [:]
        ports = (me["ports"] as? [[String: Any]] ?? []).map {
            Port(name: $0["name"] as? String ?? "",
                 active: $0["active"] as? Bool ?? false,
                 address: $0["address"] as? String ?? "",
                 rdma: $0["rdma"] as? String ?? "")
        }
        bridge = me["bridge"] as? String ?? ""
        bridgeAddress = me["bridge_address"] as? String ?? ""
        bridged = me["bridged"] as? [String] ?? []
        rdma = me["rdma"] as? Bool ?? false
        splits = j["splits"] as? Bool ?? false
        for (id, ls) in j["links"] as? [String: Any] ?? [:] {
            links[id] = (ls as? [[String: Any]] ?? []).map {
                Link(localInterface: $0["local_interface"] as? String ?? "",
                     localAddress: $0["local_address"] as? String ?? "",
                     localPort: $0["local_port"] as? String ?? "",
                     remoteAddress: $0["remote_address"] as? String ?? "",
                     remotePort: $0["remote_port"] as? String ?? "",
                     rttMs: $0["rtt_ms"] as? Double ?? 0,
                     localRdma: $0["local_rdma"] as? String ?? "",
                     remoteRdma: $0["remote_rdma"] as? String ?? "")
            }
        }
    }
}

/// The member this Mac is PAIRED with over Thunderbolt: its clips that
/// may be split run half there.
struct FleetPair: Equatable, Sendable {
    var member: String
    var name: String
    var connected: Bool
    var links: Int
    var rdma: Bool
    /// Ready to split now; else `why` not, localized.
    var ready: Bool
    var why: String
    /// MiniMax H3's heads each Mac takes: here, there.
    var heads: [Int]
    /// This Mac's share, by the two GPUs (`autoShare`) or as set.
    var share: Double
    var autoShare: Bool
    /// Its settings, from the configuration.
    var useRdma: Bool
    var setShare: Double

    init?(_ j: [String: Any]?, config c: [String: Any]) {
        guard let j, let member = j["member"] as? String, !member.isEmpty
        else { return nil }
        self.member = member
        name = j["name"] as? String ?? member
        connected = j["connected"] as? Bool ?? false
        links = j["links"] as? Int ?? 0
        rdma = j["rdma"] as? Bool ?? false
        ready = j["ready"] as? Bool ?? false
        why = (j["why"] as? [String: Any]).map(L10n.coreMessage) ?? ""
        heads = j["heads"] as? [Int] ?? []
        share = j["share"] as? Double ?? 0
        autoShare = j["auto_share"] as? Bool ?? true
        let pc = c["pair"] as? [String: Any] ?? [:]
        useRdma = pc["rdma"] as? Bool ?? true
        setShare = pc["share"] as? Double ?? 0
    }
}

/// A task of the fleet -- an engine op a member runs for another -- and
/// the catalog models a job of it names.
struct FleetTask: Decodable, Sendable, Equatable, Identifiable {
    struct Model: Decodable, Sendable, Equatable, Identifiable {
        var id: String
        var name: String
        var family: String
        var installed: Bool
    }
    var op: String
    /// This Mac's engine runs it.
    var runs: Bool
    var models: [Model]
    var id: String { op }
}

/// Where this Mac's own jobs go -- per task, per model -- and what it
/// does not offer the fleet. Kept apart from the decoded status: its
/// tables are keyed by model ids, which the snake-case decoding would
/// rewrite at an underscore.
struct FleetPolicies: Equatable, Sendable {
    var tasks: [String: String] = [:]
    var models: [String: String] = [:]
    var withheldTasks: Set<String> = []
    var withheldModels: Set<String> = []

    init() {}

    init(config c: [String: Any]) {
        let off = c["offload"] as? [String: Any] ?? [:]
        tasks = off["tasks"] as? [String: String] ?? [:]
        models = off["models"] as? [String: String] ?? [:]
        let held = c["withheld"] as? [String: Any] ?? [:]
        withheldTasks = Set(held["tasks"] as? [String] ?? [])
        withheldModels = Set(held["models"] as? [String] ?? [])
    }

    /// A task's policy (local first unless set).
    func task(_ op: String) -> String { tasks[op] ?? "local-first" }
}

struct FleetConfig: Decodable, Sendable, Equatable {
    var memberId: String
    var memberName: String
    var fleet: String
    var secretSet: Bool
    var discoverable: Bool
    var accept: String
    var schedule: FleetSchedule
    var joined: Bool
    var accepting: Bool
    /// A job naming Custom's weights sent too, its files copied to the
    /// member first (DESIGN §11d).
    var sendWeights: Bool?
}

/// When this Mac takes fleet jobs: `days` (bit 0 Monday .. bit 6
/// Sunday), from minute `from` to minute `to` of the day -- `to` before
/// `from` runs past midnight.
struct FleetSchedule: Codable, Sendable, Equatable {
    var on: Bool
    var days: Int
    var from: Int
    var to: Int

    var json: [String: Any] {
        ["on": on, "days": days, "from": from, "to": to]
    }
}

struct FleetMember: Decodable, Sendable, Equatable, Identifiable {
    var id: String
    var name: String
    /// connected | found (not reached yet) | refused (another secret).
    var state: String
    var me: FleetSelf?

    enum CodingKeys: String, CodingKey {
        case id, name, state, me = "self"
    }
}

/// What a member says it is.
struct FleetSelf: Decodable, Sendable, Equatable {
    struct Machine: Decodable, Sendable, Equatable {
        var chip: String?
        var ramGb: Int?
        var gpuCores: Int?
    }
    var ops: [String]?
    var installed: [String]?
    var features: [String]?
    var assistant: String?
    var busy: Bool?
    var serving: Bool?
    var accepting: Bool?
    var accept: String?
    var machine: Machine?
    var engine: String?
    var version: String?
    /// A DiT may be split there (DESIGN §11c).
    var splits: Bool?
}

/// A fleet found on the network, by its discoverable members.
struct FleetSeen: Decodable, Sendable, Equatable, Identifiable {
    var fleet: String
    var members: Int
    var id: String { fleet }
}

/// A job a member sent: served here, or waiting for an answer.
struct FleetJob: Decodable, Sendable, Equatable, Identifiable {
    var job: String
    var from: String
    var title: String
    var op: String
    var model: String?
    var id: String { job }
}

/// The job this Mac is working on for a member, as its events tell it.
struct FleetServing: Equatable {
    var job: String
    var from: String
    var title: String
    var op: String
    /// Its half of a clip split with the member (DESIGN §11c).
    var split = false
    var progress: Double?
    var phase: JobPhase?
}

extension AppModel {
    // MARK: The fleet (DESIGN §11)

    /// The core's account of the fleet, read again.
    func reloadFleet() {
        guard let core else { return }
        let data = core.fleetStatusJSON()
        fleet = DTO.decode(FleetStatus.self, data)
        if let raw = try? JSONSerialization.jsonObject(with: data)
            as? [String: Any],
           let c = raw["config"] as? [String: Any] {
            fleet?.policies = FleetPolicies(config: c)
            fleet?.thunderbolt = FleetThunderbolt(
                raw["thunderbolt"] as? [String: Any] ?? [:])
            fleet?.pair = FleetPair(raw["pair"] as? [String: Any], config: c)
        }
        // An offer waiting is asked for; one gone is not.
        let asking = fleet?.asking ?? []
        fleetAsks = fleetAsks.filter { a in asking.contains { $0.job == a.job } }
        for a in asking where !fleetAsks.contains(where: { $0.job == a.job }) {
            fleetAsks.append(a)
        }
    }

    /// The members changed: what runs here through them too.
    func fleetChanged() {
        reloadFleet()
        refreshRunnable()
        // The members' models: Capabilities' Copy from … follows them.
        if let core {
            capabilityTree = DTO.decode(CapabilityTree.self,
                                        core.capabilityTreeJSON())
        }
    }

    /// This Mac in its fleet, changed: its name, discoverable or not, how
    /// it takes jobs and when; a fleet made, joined (`fleet` and
    /// `secret`) or left (`fleet` "").
    @discardableResult
    func configureFleet(_ request: [String: Any]) -> Bool {
        guard let core else { return false }
        let r = core.fleetConfigure(request)
        if !r.ok { flash(r.message, level: .error) }
        reloadFleet()
        return r.ok
    }

    /// Where a task's jobs go: never | local-first | fleet-first |
    /// always.
    func setFleetOffload(task op: String, _ policy: String) {
        configureFleet(["offload": ["tasks": [op: policy]]])
    }

    /// Where a model's jobs go -- "" its task's policy.
    func setFleetOffload(model id: String, _ policy: String) {
        configureFleet(["offload": ["models": [id: policy]]])
    }

    /// A task offered to the fleet, or withheld.
    func setFleetShared(task op: String, _ on: Bool) {
        configureFleet(["share": ["tasks": [op: on]]])
    }

    /// A model offered to the fleet, or withheld.
    func setFleetShared(model id: String, _ on: Bool) {
        configureFleet(["share": ["models": [id: on]]])
    }

    /// Paired with a member over Thunderbolt (its id), or not ("").
    func pairFleet(_ member: String) {
        configureFleet(["pair": member])
    }

    /// How the pair splits: RDMA or TCP; this Mac's share (0: by the two
    /// GPUs).
    func setPair(rdma: Bool? = nil, share: Double? = nil) {
        var r: [String: Any] = [:]
        if let rdma { r["pair_rdma"] = rdma }
        if let share { r["pair_share"] = share }
        configureFleet(r)
    }

    /// The network looked over for fleets to join (the Fleet page open).
    func browseFleets(_ on: Bool) {
        core?.fleetBrowse(on)
    }

    /// A member's offer, answered.
    func answerFleet(_ job: FleetJob, accept: Bool) {
        core?.fleetAnswer(job: job.job, accept: accept)
        fleetAsks.removeAll { $0.job == job.job }
    }

    /// The fleet's events: the members, an offer to answer, the job
    /// served here.
    func handleFleet(_ ev: CoreEvent) {
        let p = ev.payload
        switch ev.kind {
        case "fleet.changed":
            fleetChanged()
        case "fleet.ask":
            reloadFleet()
        case "fleet.serving":
            let state = p["state"] as? String ?? ""
            guard let job = ev.job else { return }
            if state == "queued" || state == "running" {
                let was = fleetServing?.job == job ? fleetServing : nil
                withAnimation(Self.motion) {
                    fleetServing = FleetServing(
                        job: job, from: p["from"] as? String ?? "",
                        title: p["title"] as? String ?? "",
                        op: p["op"] as? String ?? "",
                        split: p["split"] as? Bool ?? false,
                        progress: was?.progress, phase: was?.phase)
                }
            } else if fleetServing?.job == job {
                withAnimation(Self.motion) { fleetServing = nil }
            }
            reloadFleet()
        case "fleet.progress":
            guard let job = ev.job, fleetServing?.job == job else { return }
            if let prog = p["progress"] as? Double, prog >= 0 {
                fleetServing?.progress = prog
            }
            if let ph = p["phase"] as? [String: Any],
               let phase = JobPhase(ph) {
                fleetServing?.phase = phase
            }
        case "job.runner":
            reloadTasks()
            // A clip split with the paired member, or why not this time
            // (DESIGN §11c).
            if let with = p["split"] as? String {
                flash(String(localized: "Split with \(with) over Thunderbolt"),
                      level: .info)
            } else if let why = p["unsplit"] as? [String: Any] {
                flash(String(localized: "Not split: \(L10n.coreMessage(why))"),
                      level: .info)
            }
        default:
            break
        }
    }
}
