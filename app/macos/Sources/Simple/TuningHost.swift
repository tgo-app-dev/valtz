import Foundation

/// What Favor's Custom panel (TuningPanel) edits: the generation card's
/// values (AppModel, kept per model) or an Atlas generate node's
/// (AtlasNodeTuning, kept with the node). The panel draws `tuningInfo`
/// and asks for each change; the host has the core settle it with the
/// rest -- its rules: HyperFlow takes a LoRA slot, VDN replaces Sol-Attn
/// -- and keeps what comes back.
@MainActor
protocol TuningHost: AnyObject {
    /// The options, settled with the values -- nil: none to show.
    var tuningInfo: TuningInfo? { get }
    /// Whose they are, as the panel's title says it.
    var tuningTitle: String { get }
    /// The refine's group shows (a clip regenerated past 768P).
    var tuningRegenerates: Bool { get }
    var tuningCore: CoreService? { get }
    /// The values given to the core to settle, then kept. With
    /// `stepsFollow` the steps are left to the preset's count for what
    /// changed (an adapter's own).
    func settleTuning(_ t: Tuning, stepsFollow: Bool)
    /// Every value as preset `p` has it, the weights dropped before kept,
    /// off.
    func resetTuning(to p: Preference)
    /// A word to the person (weights that are none).
    func tuningWarn(_ text: String)
}

extension TuningHost {
    /// One option changed in the panel. The core settles it with the
    /// rest (its rules: HyperFlow takes a LoRA slot, not the Turbo LoRA's,
    /// and its own steps; VDN replaces Sol-Attn), and that is what is
    /// kept. An option that decides the steps lets them follow it.
    func setTuning(_ key: String, flag: Bool? = nil, number: Double? = nil,
                   choice: String? = nil, schedule: String? = nil) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        if let flag { t.flags[key] = flag }
        if let number {
            t.numbers[key] = number
            t.schedules[key] = nil
        }
        if let choice { t.choices[key] = choice }
        // A value a step ("1,1.5,1.75") in the number's place.
        if let schedule {
            t.schedules[key] = schedule
            t.numbers[key] = nil
        }
        let o = info.options.first { $0.key == key }
        settleTuning(t, stepsFollow: o?.fixedSteps != nil)
    }

    // LoRAs and checkpoints (Custom's list; core models/tuning.h).

    /// Weights dropped on the panel, .safetensors files or folders: each
    /// filed by what it is (the core reads it) -- a LoRA in the LoRA list,
    /// a DiT or VAE checkpoint in its part's.
    @discardableResult
    func addWeights(_ urls: [URL]) -> Bool {
        guard let core = tuningCore else { return false }
        var loras: [URL] = []
        var parts: [String: [URL]] = [:]
        var refmods = 0
        for url in urls {
            switch core.checkpointKind(url) {
            case "lora": loras.append(url)
            case "dit": parts["dits", default: []].append(url)
            case "vae": parts["vaes", default: []].append(url)
            // Reference latents, not weights: the prompt row's.
            case "refmod": refmods += 1
            default: break
            }
        }
        var added = false
        if !loras.isEmpty { added = addLoRAs(loras) || added }
        if !parts.isEmpty, let info = tuningInfo {
            var t = info.tuning
            for (key, urls) in parts {
                var cs = t.checkpoints[key] ?? []
                for url in urls {
                    let path = url.standardizedFileURL.path
                    guard !cs.contains(where: { $0.path == path }) else {
                        continue
                    }
                    // On, when no other is -- one whose file is gone
                    // runs nothing: dropping one is choosing it.
                    cs.append(TuningCheckpoint(
                        path: path,
                        on: !cs.contains { $0.on && !$0.missing }))
                    added = true
                }
                t.checkpoints[key] = cs
            }
            settleTuning(t, stepsFollow: false)
        }
        if !added && refmods > 0 {
            tuningWarn(String(localized: "A RefMod is a reference: drop it in the prompt's row"))
        } else if !added && !urls.isEmpty {
            tuningWarn(String(localized: "Not weights Valtz can use here"))
        }
        return added
    }

    /// A DiT or VAE checkpoint turned on (the others off: one replaces
    /// the model's part) or off (the model's own again).
    func setCheckpoint(_ list: String, _ path: String, on: Bool) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        var cs = t.checkpoints[list] ?? []
        for i in cs.indices {
            cs[i].on = cs[i].path == path ? on : (on ? false : cs[i].on)
        }
        t.checkpoints[list] = cs
        settleTuning(t, stepsFollow: false)
    }

    func removeCheckpoint(_ list: String, _ path: String) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        t.checkpoints[list]?.removeAll { $0.path == path }
        settleTuning(t, stepsFollow: false)
    }

    /// LoRAs dropped on the panel, files or folders (the core loads a
    /// folder's weights) not in the list yet: on while a slot is free.
    @discardableResult
    func addLoRAs(_ urls: [URL]) -> Bool {
        guard let info = tuningInfo else { return false }
        var t = info.tuning
        var added = false
        for url in urls {
            let path = url.standardizedFileURL.path
            guard !t.loras.contains(where: { $0.path == path }) else { continue }
            let on = t.loras.filter { $0.on && !$0.missing }.count
                < loraSlots(t, info)
            t.loras.append(TuningLoRA(path: path, scale: 1, on: on))
            added = true
        }
        if added { settleTuning(t, stepsFollow: false) }
        return added
    }

    /// How many LoRAs run at once: vpipe's slots (eight), less HyperFlow's,
    /// Pika H3 Flex's or TaoMate's adapter.
    func loraSlots(_ t: Tuning, _ info: TuningInfo) -> Int {
        (info.loraOption?.slots ?? 8)
            - (t.flags["hyperflow"] == true || t.flags["pika_flex"] == true
                || t.flags["taomate"] == true ? 1 : 0)
    }

    /// One LoRA changed: its strength, or on. Turned on with the slots
    /// full, it takes one of another's -- a LoRA of one's own before the
    /// Turbo LoRA. The Turbo LoRA turned on or off takes the steps its
    /// preset runs with it, or without.
    func setLoRA(_ path: String, scale: Double? = nil, on: Bool? = nil) {
        guard let info = tuningInfo,
              let i = info.tuning.loras.firstIndex(where: { $0.path == path })
        else { return }
        var t = info.tuning
        if let scale { t.loras[i].scale = scale }
        if let on {
            // (One whose file is gone takes no slot.)
            let others = t.loras.indices.filter {
                $0 != i && t.loras[$0].on && !t.loras[$0].missing
            }
            if on, others.count >= loraSlots(t, info) {
                let own = others.last { t.loras[$0].path != info.turboLoRA }
                if let j = own ?? others.last { t.loras[j].on = false }
            }
            t.loras[i].on = on
        }
        settleTuning(t, stepsFollow: on != nil && path == info.turboLoRA)
    }

    /// The ACCELERATION LoRA Custom runs (the panel's quick pick: the
    /// model's few-step adapters, as Capabilities lists them), or none:
    /// the others listed are turned off, the one picked goes first, on,
    /// with the steps and shifts it was distilled for -- HyperFlow's, Pika
    /// H3 Flex's and TaoMate's adapters, which would take its slot, off.
    /// None: the model's own steps and shifts.
    func setFastLoRA(_ id: String?) {
        guard let info = tuningInfo, let opt = info.loraOption else { return }
        var t = info.tuning
        let fast = Set(opt.fast.map(\.path).filter { !$0.isEmpty }
                       + [info.turboLoRA].filter { !$0.isEmpty })
        for i in t.loras.indices where fast.contains(t.loras[i].path) {
            t.loras[i].on = false
        }
        let chosen = opt.fast.first { $0.id == id && $0.installed }
        if let f = chosen {
            var l = TuningLoRA(path: f.path, on: true)
            if let i = t.loras.firstIndex(where: { $0.path == f.path }) {
                l = t.loras.remove(at: i)
                l.on = true
            }
            t.loras.insert(l, at: 0)
            for k in ["hyperflow", "pika_flex", "taomate", "taomate_lora"]
                where t.flags[k] == true {
                t.flags[k] = false
            }
            t.numbers["steps"] = Double(f.steps)
        } else if let off = opt.stepsOff {
            t.numbers["steps"] = Double(off)
        }
        // Its shifts, or the model's own.
        for key in ["video_shift", "audio_shift"]
            where t.numbers[key] != nil {
            if let v = chosen?.config[key] ?? opt.shiftOff[key] {
                t.numbers[key] = v
            }
        }
        settleTuning(t, stepsFollow: false)
    }

    func removeLoRA(_ path: String) {
        guard let info = tuningInfo else { return }
        var t = info.tuning
        t.loras.removeAll { $0.path == path }
        settleTuning(t, stepsFollow: path == info.turboLoRA)
    }
}
