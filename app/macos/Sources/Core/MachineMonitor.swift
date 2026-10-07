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
    }

    /// The GPU thermal sample (vpipe's `--gpu-thermal`).
    struct Thermal: Equatable {
        var verdict: String        // unknown | idle | normal | warm | throttled
        var clockMHz: Double?
        var ceilingMHz: Double?
        var activePct: Double?
        var tempC: Double?
    }

    private(set) var load = Load()
    private(set) var thermal: Thermal?
    private(set) var osState = ProcessInfo.processInfo.thermalState

    @ObservationIgnored private let core: CoreService?
    @ObservationIgnored private var loadTask: Task<Void, Never>?
    @ObservationIgnored private var thermalTask: Task<Void, Never>?
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
        if let observer { NotificationCenter.default.removeObserver(observer) }
        observer = nil
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
            total: (j["phys_total_bytes"] as? NSNumber)?.doubleValue)
        if next != load { load = next }
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
