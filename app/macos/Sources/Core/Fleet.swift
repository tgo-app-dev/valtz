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
    var members: [FleetMember]?
    var fleets: [FleetSeen]?
    var serving: FleetJob?
    var asking: [FleetJob]?
    /// This Mac as the others see it.
    var me: FleetSelf?

    enum CodingKeys: String, CodingKey {
        case enabled, config, port, browsing, members, fleets, serving,
             asking, me = "self"
    }
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
    var progress: Double?
    var phase: JobPhase?
}

extension AppModel {
    // MARK: The fleet (DESIGN §11)

    /// The core's account of the fleet, read again.
    func reloadFleet() {
        guard let core else { return }
        fleet = DTO.decode(FleetStatus.self, core.fleetStatusJSON())
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
    }

    /// This Mac in its fleet, changed: its name, discoverable or not, how
    /// it takes jobs and when; a fleet made, joined (`fleet` and
    /// `secret`) or left (`fleet` "").
    @discardableResult
    func configureFleet(_ request: [String: Any]) -> Bool {
        guard let core else { return false }
        let r = core.fleetConfigure(request)
        if !r.ok { flash(r.message) }
        reloadFleet()
        return r.ok
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
        default:
            break
        }
    }
}
