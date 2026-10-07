import SwiftUI

/// Settings › Agentic Helper: the model that writes prompts with the
/// person (Enhance, a song's lyrics, a voice's directions) and reads what
/// they ask for. Which one -- Auto, the tier's pick, or one by name, kept
/// by the core so valtzctl takes it too -- and how it runs: the sampler
/// its model card recommends, and the drafter it decodes ahead with.
/// More of the helper's settings will gather here.
struct HelperView: View {
    @Bindable var model: AppModel

    var body: some View {
        Form {
            Section { SettingsHeader(page: .helper) }
            if let list = model.helpers {
                Section("Model") {
                    Picker("Helper", selection: Binding(
                        get: { list.choice },
                        set: { model.chooseHelper($0) })) {
                        Text(autoLabel(list)).tag("")
                        Divider()
                        ForEach(list.models) { h in
                            Text(label(h)).tag(h.id)
                        }
                    }
                    inUse(list)
                }
                Section {
                    Picker("Keep Loaded", selection: Binding(
                        get: { Self.keepChoice(list.keepLoaded ?? 600) },
                        set: { model.setHelperKeepLoaded($0) })) {
                        Text("Unload after each request").tag(0.0)
                        Text("5 minutes").tag(300.0)
                        Text("10 minutes").tag(600.0)
                        Text("30 minutes").tag(1800.0)
                        Text("1 hour").tag(3600.0)
                    }
                } header: {
                    Text("Memory")
                } footer: {
                    Text("Kept loaded, the next request starts at once instead of reading the whole model again. A generation that needs the memory unloads it first.")
                        .foregroundStyle(.secondary)
                }
                if let h = list.helper(list.using) {
                    Section("How It Runs") {
                        LabeledContent("Sampling") {
                            Text(sampling(h.sampling))
                                .multilineTextAlignment(.trailing)
                        }
                        if let d = h.dflash {
                            drafterPicker(list, h, d)
                        } else {
                            LabeledContent("Multi-token prediction") {
                                Text(mtp(h))
                                    .multilineTextAlignment(.trailing)
                            }
                        }
                        LabeledContent("Thinking") {
                            Text("Off: it answers straight away")
                        }
                    }
                }
            } else {
                Section {
                    Text("No helper is known to this build.")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .onAppear { model.refreshMachine() }
    }

    /// What it drafts with, where a DFlash 2 drafter is offered: its MTP
    /// head (the default), or the drafter at 8 or 4 bits -- more memory
    /// beside the model, so never chosen for the person.
    @ViewBuilder
    private func drafterPicker(_ list: HelperList, _ h: HelperList.Helper,
                               _ d: HelperList.Drafter) -> some View {
        let dflash = list.drafter == "dflash"
        let tag = dflash ? "dflash:\(list.drafterBits ?? 8)" : "mtp"
        Picker("Drafter", selection: Binding(
            get: { tag },
            set: { v in
                let p = v.split(separator: ":")
                model.setHelperDrafter(String(p[0]),
                                       bits: p.count > 1 ? Int(p[1]) ?? 8
                                                         : 8)
            })) {
            Text("MTP head").tag("mtp")
            Text("DFlash 2 · 8-bit (about 2.2 GB more)").tag("dflash:8")
            Text("DFlash 2 · 4-bit (about 1.3 GB more)").tag("dflash:4")
        }
        if dflash && d.state != "installed" {
            LabeledContent {
                Button("Open Capabilities") {
                    model.openCapabilities(family: "helpers")
                }
            } label: {
                Text("\(d.name) is not downloaded: the MTP head drafts until it is.")
                    .foregroundStyle(.secondary)
            }
        } else if dflash {
            Text("It drafts a block of words at a time, which the model checks in one pass. It holds more memory beside the model; on a Mac without much to spare, the MTP head is as fast.")
                .foregroundStyle(.secondary)
        }
    }

    /// The offered time nearest to what is kept (valtzctl may set any).
    private static func keepChoice(_ s: Double) -> Double {
        let offered: [Double] = [0, 300, 600, 1800, 3600]
        return offered.min { abs($0 - s) < abs($1 - s) } ?? 600
    }

    private func autoLabel(_ list: HelperList) -> String {
        if let h = list.helper(list.auto) {
            return String(localized: "Auto (\(h.name))")
        }
        return String(localized: "Auto")
    }

    /// A helper's name, and what keeps it from running here.
    private func label(_ h: HelperList.Helper) -> String {
        if h.state != "installed" {
            return String(localized: "\(h.name) — not downloaded")
        }
        if !h.fits {
            return String(localized: "\(h.name) — needs \(String(h.minRamGb)) GB")
        }
        return h.name
    }

    /// Which one runs, when that is not plain from the picker: Auto's pick,
    /// or Auto's while the chosen one is not here.
    @ViewBuilder
    private func inUse(_ list: HelperList) -> some View {
        let using = list.helper(list.using)?.name ?? list.using
        if list.choice.isEmpty {
            if list.using.isEmpty {
                Text("No helper fits this Mac.")
                    .foregroundStyle(.secondary)
            } else {
                Text("Auto takes the best helper this Mac keeps in memory: \(using).")
                    .foregroundStyle(.secondary)
            }
        } else if let chosen = list.helper(list.choice) {
            if list.using != list.choice {
                LabeledContent {
                    Button("Open Capabilities") {
                        model.openCapabilities(family: "helpers")
                    }
                } label: {
                    Text("\(chosen.name) is not downloaded: \(using) runs until it is.")
                        .foregroundStyle(.secondary)
                }
            } else if !chosen.fits {
                Text("\(chosen.name) is offered from \(String(chosen.minRamGb)) GB of memory. On this Mac its weights may not all stay in memory, and it can write very slowly.")
                    .foregroundStyle(.orange)
            }
        }
    }

    /// The sampler, as its model card gives it.
    private func sampling(_ s: HelperList.Sampling) -> String {
        guard let t = s.temperature else {
            return String(localized: "Greedy: the likeliest word each time")
        }
        var parts = [String(localized: "Temperature \(number(t))")]
        if let p = s.topP, p < 1 {
            parts.append(String(localized: "top-p \(number(p))"))
        }
        if let k = s.topK, k > 0 {
            parts.append(String(localized: "top-k \(String(k))"))
        }
        if let m = s.minP, m > 0 {
            parts.append(String(localized: "min-p \(number(m))"))
        }
        if let pp = s.presencePenalty, pp != 0 {
            parts.append(String(localized: "presence penalty \(number(pp))"))
        }
        if let rp = s.repetitionPenalty, rp != 1 {
            parts.append(String(localized: "repetition penalty \(number(rp))"))
        }
        return parts.joined(separator: " · ")
    }

    private func mtp(_ h: HelperList.Helper) -> String {
        guard h.mtp else { return String(localized: "None") }
        guard let d = h.drafter else {
            return String(localized: "Its own head drafts ahead")
        }
        return d.state == "installed"
            ? String(localized: "\(d.name) drafts ahead")
            : String(localized: "\(d.name) is not downloaded: one word at a time")
    }

    /// 0.7, 1.5: as typed, not as the locale groups numbers.
    private func number(_ v: Double) -> String {
        var s = String(format: "%.2f", v)
        while s.hasSuffix("0") { s.removeLast() }
        if s.hasSuffix(".") { s.removeLast() }
        return s
    }
}
