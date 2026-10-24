import Foundation
import SwiftUI

/// The machine's live load, and whether it is being held back: the status
/// bar's (View › Show Status Bar). Read the way vpipe's own front ends
/// read them, through the core (Engine::machine_status / gpu_thermal --
/// vpipe's SystemMonitor):
///
/// * the web UI's status bar: the ANE's utilisation (its power over a
///   per-chip ceiling, or the share of time its fabric sat above idle --
///   whichever is live on this release), the GPU's (IOAccelerator), and
///   memory (this process's physical footprint -- the engine runs in it,
///   models and all -- and the whole Mac's use, as Activity Monitor's
///   Memory Used counts it, of the Mac's), polled every second;
/// * vpipe's FLOP/s meter: the arithmetic the GPU and the ANE achieve
///   (TFLOP/s, as the models that report it credit it), the highest this
///   run has seen -- and, in a fleet with a member connected, the same
///   for the whole fleet, each member telling its own;
/// * the macOS app's thermal row: a 600 ms sample of the GPU's clock
///   residency every 8 seconds. Busy AND clamped below its rated clock
///   is throttling; a low clock while idle is just DVFS. Where the GPU is
///   too quiet to judge, the OS thermal state stands in -- the better
///   answer for a machine that is warm and doing nothing.
///
/// It polls only while the bar is shown (start / stop).
@MainActor @Observable
final class MachineMonitor {
    struct Load: Equatable {
        var ane: Double?           // %, estimated
        var aneWatts: Double?      // what the estimate came from, where
        var aneBandwidth: Double?  //   live: its power, or its fabric GB/s
        var gpu: Double?           // %
        var footprint: Double?     // bytes, this process
        var systemUsed: Double?    // bytes, the whole Mac's use
        var total: Double?         // bytes, the Mac's
        var flops: Flops?          // this Mac's arithmetic
        var fleet: Flops?          // the fleet's, with a member connected
    }

    /// Arithmetic achieved: the GPU's and the ANE's TFLOP/s, and the
    /// highest the two have reached together -- the meter's full scale.
    struct Flops: Equatable {
        var gpu: Double
        var ane: Double
        var peak: Double
        /// A fleet's: each member's as it told them (nil: it does not).
        var members: [Member] = []

        struct Member: Equatable {
            var name: String
            var isSelf: Bool
            var gpu: Double?
            var ane: Double?
        }

        var total: Double { gpu + ane }
    }

    /// The GPU thermal sample (vpipe's `--gpu-thermal`).
    struct Thermal: Equatable {
        var verdict: String        // unknown | idle | normal | warm | throttled
        var clockMHz: Double?
        var ceilingMHz: Double?
        var activePct: Double?
        var tempC: Double?
    }

    /// What the engine holds in memory (Controller::memory_held): the
    /// RAM popover's, read while it is open (startMemory / stopMemory).
    struct Memory: Equatable {
        struct Item: Equatable, Identifiable {
            /// "helper", "dit", "text-encoder", "vae" (kept warm), or
            /// "weights" (a checkpoint resident, not kept).
            var part: String
            var name: String
            var model: String
            var bytes: Double
            var kvBytes: Double
            /// Kept warm: how long it stays unless used again.
            var seconds: Double?
            var warm: Bool
            /// Its size from its files: vpipe counts none of it.
            var estimated: Bool
            var parked: Bool
            var id: String { "\(part):\(model):\(name)" }
        }
        var items: [Item] = []
        /// This process's Metal allocations: the items and the scratch
        /// they work in.
        var gpuAllocated: Double?
        /// What letting idle models go would give back now.
        var pooled: Double = 0
    }

    private(set) var load = Load()
    private(set) var memory: Memory?
    private(set) var thermal: Thermal?
    private(set) var osState = ProcessInfo.processInfo.thermalState

    @ObservationIgnored private let core: CoreService?
    @ObservationIgnored private var loadTask: Task<Void, Never>?
    @ObservationIgnored private var thermalTask: Task<Void, Never>?
    @ObservationIgnored private var memoryTask: Task<Void, Never>?
    @ObservationIgnored private var observer: NSObjectProtocol?

    /// The web UI's pace; the macOS app's (each thermal sample costs a
    /// 600 ms window of IOReport and SMC reads, no GPU work).
    private static let loadInterval: Duration = .seconds(1)
    private static let thermalInterval: Duration = .seconds(8)
    nonisolated private static let thermalWindowMs = 600

    init(core: CoreService?) {
        self.core = core
    }

    var running: Bool { loadTask != nil }

    func start() {
        guard loadTask == nil, let core else { return }
        osState = ProcessInfo.processInfo.thermalState
        observer = NotificationCenter.default.addObserver(
            forName: ProcessInfo.thermalStateDidChangeNotification,
            object: nil, queue: .main) { [weak self] _ in
                let now = ProcessInfo.processInfo.thermalState
                Task { @MainActor in self?.osState = now }
            }
        loadTask = Task { [weak self] in
            while !Task.isCancelled {
                let d = await Task.detached(priority: .utility) {
                    core.machineStatusJSON()
                }.value
                if Task.isCancelled { return }
                self?.take(load: d)
                try? await Task.sleep(for: Self.loadInterval)
            }
        }
        thermalTask = Task { [weak self] in
            while !Task.isCancelled {
                let d = await Task.detached(priority: .utility) {
                    core.gpuThermalJSON(windowMs: Self.thermalWindowMs)
                }.value
                if Task.isCancelled { return }
                self?.take(thermal: d)
                try? await Task.sleep(for: Self.thermalInterval)
            }
        }
    }

    func stop() {
        loadTask?.cancel()
        thermalTask?.cancel()
        loadTask = nil
        thermalTask = nil
        stopMemory()
        if let observer { NotificationCenter.default.removeObserver(observer) }
        observer = nil
    }

    /// The popover open: what the engine holds, every two seconds -- a
    /// model kept warm counts down, one let go leaves the list.
    func startMemory() {
        guard memoryTask == nil, let core else { return }
        memoryTask = Task { [weak self] in
            while !Task.isCancelled {
                let d = await Task.detached(priority: .utility) {
                    core.memoryHeldJSON()
                }.value
                if Task.isCancelled { return }
                self?.take(memory: d)
                try? await Task.sleep(for: .seconds(2))
            }
        }
    }

    func stopMemory() {
        memoryTask?.cancel()
        memoryTask = nil
    }

    private func take(memory d: Data) {
        guard let j = try? JSONSerialization.jsonObject(with: d)
                as? [String: Any] else { return }
        func num(_ v: Any?) -> Double? { (v as? NSNumber)?.doubleValue }
        let items = (j["items"] as? [[String: Any]] ?? []).map { i in
            Memory.Item(part: i["part"] as? String ?? "",
                        name: i["name"] as? String ?? "",
                        model: i["model"] as? String ?? "",
                        bytes: num(i["bytes"]) ?? 0,
                        kvBytes: num(i["kv_bytes"]) ?? 0,
                        seconds: (i["warm"] as? Bool ?? false)
                            ? num(i["seconds"]) : nil,
                        warm: i["warm"] as? Bool ?? false,
                        estimated: i["estimated"] as? Bool ?? false,
                        parked: i["parked"] as? Bool ?? false)
        }
        let next = Memory(items: items, gpuAllocated: num(j["gpu_allocated"]),
                          pooled: num(j["pooled_bytes"]) ?? 0)
        if next != memory { memory = next }
    }

    private func take(load d: Data) {
        guard let j = try? JSONSerialization.jsonObject(with: d)
                as? [String: Any] else { return }
        let next = Load(
            ane: j["ane_util_pct"] as? Double,
            aneWatts: j["ane_power_w"] as? Double,
            aneBandwidth: j["ane_bw_gbps"] as? Double,
            gpu: j["gpu_util_pct"] as? Double,
            footprint: (j["phys_footprint_bytes"] as? NSNumber)?.doubleValue,
            systemUsed: (j["sys_used_bytes"] as? NSNumber)?.doubleValue,
            total: (j["phys_total_bytes"] as? NSNumber)?.doubleValue,
            flops: Self.flops(j),
            fleet: (j["fleet"] as? [String: Any]).flatMap(Self.flops))
        if next != load { load = next }
    }

    private static func flops(_ j: [String: Any]) -> Flops? {
        func num(_ v: Any?) -> Double? { (v as? NSNumber)?.doubleValue }
        guard let gpu = num(j["gpu_tflops"]) else { return nil }
        let ane = num(j["ane_tflops"]) ?? 0
        let members = (j["members"] as? [[String: Any]] ?? []).map {
            Flops.Member(name: $0["name"] as? String ?? "",
                         isSelf: $0["self"] as? Bool ?? false,
                         gpu: num($0["gpu_tflops"]),
                         ane: num($0["ane_tflops"]))
        }
        return Flops(gpu: gpu, ane: ane,
                     peak: max(num(j["peak_tflops"]) ?? 0, gpu + ane),
                     members: members)
    }

    private func take(thermal d: Data) {
        guard let j = try? JSONSerialization.jsonObject(with: d)
                as? [String: Any],
              let verdict = j["verdict"] as? String else {
            thermal = nil
            return
        }
        thermal = Thermal(verdict: verdict,
                          clockMHz: j["clock_mhz"] as? Double,
                          ceilingMHz: j["ceiling_mhz"] as? Double,
                          activePct: j["gpu_active_pct"] as? Double,
                          tempC: j["temp_c"] as? Double)
    }

    // MARK: - The thermal row, as the vpipe app presents it
    //
    // The GPU verdict wins when it has one -- it is the specific claim;
    // the OS state is the fallback for a quiet machine.

    private var gpuVerdict: String? {
        guard let v = thermal?.verdict else { return nil }
        return ["throttled", "warm", "normal"].contains(v) ? v : nil
    }

    var thermalLabel: String {
        switch gpuVerdict {
        case "throttled": String(localized: "GPU Throttled")
        case "warm": String(localized: "Warm")
        case "normal": String(localized: "Normal")
        default: osLabel
        }
    }

    private var osLabel: String {
        switch osState {
        case .nominal: String(localized: "Normal")
        case .fair: String(localized: "Warm")
        case .serious: String(localized: "Throttling")
        case .critical: String(localized: "Critical")
        @unknown default: String(localized: "Unknown")
        }
    }

    var thermalAlerting: Bool {
        gpuVerdict == "throttled" || gpuVerdict == "warm"
            || (gpuVerdict == nil && osState != .nominal)
    }

    var thermalSymbol: String {
        switch gpuVerdict {
        case "throttled": "gauge.with.dots.needle.33percent"
        case "warm": "thermometer.high"
        case "normal": "thermometer.low"
        default:
            switch osState {
            case .nominal: "thermometer.low"
            case .fair: "thermometer.medium"
            case .serious: "thermometer.high"
            case .critical: "thermometer.sun.fill"
            @unknown default: "thermometer.medium"
            }
        }
    }

    var thermalTint: Color {
        switch gpuVerdict {
        case "throttled": .orange
        case "warm": .yellow
        case "normal": .secondary
        default:
            switch osState {
            case .nominal: .secondary
            case .fair: .yellow
            case .serious: .orange
            case .critical: .red
            @unknown default: .secondary
            }
        }
    }

    /// Shown on hover, with the numbers on purpose: "Throttled" is a
    /// claim about the person's hardware, which they should be able to
    /// check rather than take.
    var thermalHelp: String {
        guard let t = thermal, let v = gpuVerdict else {
            return osHelp + "\n\n" + (thermal == nil
                ? String(localized: "Measuring the GPU clock…")
                : String(localized: "The GPU is idle, so its clock says nothing about throttling; showing the system thermal state instead."))
        }
        var s: String
        switch v {
        case "throttled":
            s = String(localized: "The GPU is busy but running below the clock it is rated for — steps take longer, and nothing in the generation is at fault. Better airflow under the Mac helps more than anything in software.")
        case "warm":
            s = String(localized: "Running hot but still at full clock. A fanless Mac usually starts slowing down if the load continues.")
        default:
            s = String(localized: "The GPU is running at its full clock.")
        }
        var facts: [String] = []
        if let c = t.clockMHz, let top = t.ceilingMHz {
            let clock = String(Int(c.rounded()))
            let ceiling = String(Int(top.rounded()))
            facts.append(String(localized: "GPU \(clock) of \(ceiling) MHz"))
        }
        if let a = t.activePct {
            let pct = (a / 100).formatted(.percent.precision(.fractionLength(0)))
            facts.append(String(localized: "\(pct) busy"))
        }
        if let c = t.tempC {
            let temp = String(Int(c.rounded()))
            facts.append(String(localized: "\(temp) °C"))
        }
        if !facts.isEmpty { s += "\n\n" + facts.joined(separator: " · ") }
        return s
    }

    private var osHelp: String {
        switch osState {
        case .nominal:
            String(localized: "Thermals are normal; performance is unrestricted.")
        case .fair:
            String(localized: "Warming up. Fans (where present) have ramped; a fanless Mac may already be slowing down.")
        case .serious:
            String(localized: "The system is throttling — generation steps will take longer.")
        case .critical:
            String(localized: "The system is throttling hard and may suspend work. Let it cool before starting another run.")
        @unknown default:
            String(localized: "Thermal state unavailable.")
        }
    }
}
