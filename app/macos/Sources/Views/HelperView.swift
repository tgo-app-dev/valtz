import SwiftUI

/// Settings › Agentic Helper: the model that writes prompts with the
/// person (Enhance, a song's lyrics, a voice's directions) and reads what
/// they ask for. Which one -- Auto, the tier's pick, or one by name, kept
/// by the core so valtzctl takes it too -- and how it runs: the sampler
/// its model card recommends, and the drafter it decodes ahead with; and
/// how often it looks when it watches a clip (a video summary, DESIGN
/// §4i); how long a conversation it reads -- its context -- and whether
/// the agent compacts one past it (DESIGN §9b); and, for debugging,
/// whether the agent's turns show their speed.
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
                    .settingsMark(String(localized: "Helper"),
                                  String(localized: "Enhance"))
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
                    .settingsMark(String(localized: "Keep Loaded"))
                } header: {
                    Text("Memory")
                        .settingsMark(String(localized: "Memory"))
                } footer: {
                    Text("Kept loaded, the next request starts at once instead of reading the whole model again. A generation that needs the memory unloads it first.")
                        .foregroundStyle(.secondary)
                }
                videoSection(list)
                if let h = list.helper(list.using) {
                    Section("How It Runs") {
                        LabeledContent("Sampling") {
                            Text(sampling(h.sampling))
                                .multilineTextAlignment(.trailing)
                        }
                        .settingsMark(String(localized: "Sampling"))
                        if let d = h.dflash {
                            drafterPicker(list, h, d)
                        } else {
                            LabeledContent("Multi-token prediction") {
                                Text(mtp(h))
                                    .multilineTextAlignment(.trailing)
                            }
                            .settingsMark(String(localized: "Multi-token prediction"))
                        }
                        LabeledContent("Thinking") {
                            Text("Off: it answers straight away")
                        }
                        .settingsMark(String(localized: "Thinking"))
                    }
                }
                contextSection(list)
            } else {
                Section {
                    Text("No helper is known to this build.")
                        .foregroundStyle(.secondary)
                }
            }
            Section {
                Toggle("Show the Helper's Speed", isOn: Binding(
                    get: { model.agentShowsSpeed },
                    set: { model.setAgentShowsSpeed($0) }))
                    .settingsMark(String(localized: "Show the Helper's Speed"))
            } header: {
                Text("Debugging")
                    .settingsMark(String(localized: "Debugging"))
            } footer: {
                Text("Under each of the agent's turns and each suggested prompt, in small type: the tokens it read (prefill) and wrote (decode), how fast, how many tokens a round of its drafter bought and how many of its drafts were kept, and how much of the conversation it went on from. The Agent is in Settings › Experimental.")
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onAppear { model.refreshMachine() }
    }

    /// LONG CONVERSATIONS (DESIGN §9b): how much the helper reads at once
    /// -- its context: Auto, its own as far as this Mac's memory holds it,
    /// or less -- and what the agent does past it: compacts the
    /// conversation (the model's notes on the work so far, and its latest
    /// turns) and goes on, or stops. A cloud model's window too, while
    /// the agent thinks in the cloud.
    @ViewBuilder
    private func contextSection(_ list: HelperList) -> some View {
        let c = list.helper(list.using)?.context
        Section {
            Picker("Context", selection: Binding(
                get: { Self.contextChoice(list.context ?? 0, c) },
                set: { model.setHelperContext($0) })) {
                Text(autoContextLabel(c)).tag(0)
                Divider()
                ForEach(Self.contextSizes(c), id: \.self) { n in
                    Text(tokens(n)).tag(n)
                }
            }
            .settingsMark(String(localized: "Context"))
            if let c {
                LabeledContent("This Mac Gives It") {
                    Text("\(c.here) of its \(c.model) tokens")
                }
                .settingsMark(String(localized: "This Mac Gives It"))
            }
            if let s = model.agentSettings {
                Toggle("Compact Long Conversations", isOn: Binding(
                    get: { s.compact ?? true },
                    set: { model.setAgentSettings(["compact": $0]) }))
                    .settingsMark(String(localized: "Compact Long Conversations"))
                if s.backend == "cloud" {
                    cloudContextPicker(s)
                }
            }
        } header: {
            Text("Long Conversations")
                .settingsMark(String(localized: "Long Conversations"))
        } footer: {
            Text("The helper reads a conversation up to its context. Auto gives it as much of the model’s own as this Mac’s memory holds, its words kept beside the model; less leaves more memory for the pictures. Past it, the agent compacts the conversation: the model writes notes on the work so far, and goes on from them and its latest turns. Switched off, a session stops there. For sessions started after a change; the Agent is in Settings › Experimental.")
                .foregroundStyle(.secondary)
        }
    }

    /// A cloud model's window: its provider's, or as its model has it.
    @ViewBuilder
    private func cloudContextPicker(_ s: AgentSettings) -> some View {
        Picker("Cloud Context", selection: Binding(
            get: { s.cloud.contextTokens ?? 0 },
            set: { model.setAgentSettings(["cloud": ["context_tokens": $0]]) })) {
            Text(providerContextLabel(s.cloud.provider)).tag(0)
            Divider()
            ForEach([32_768, 128_000, 200_000, 400_000, 1_000_000],
                    id: \.self) { n in
                Text(tokens(n)).tag(n)
            }
            if let n = s.cloud.contextTokens, n > 0,
               ![32_768, 128_000, 200_000, 400_000, 1_000_000].contains(n) {
                Text(tokens(n)).tag(n)
            }
        }
        .settingsMark(String(localized: "Cloud Context"))
    }

    private func providerContextLabel(_ provider: String) -> String {
        switch provider {
        case "anthropic":
            return String(localized: "The provider’s (\(tokens(200_000)))")
        case "openai":
            return String(localized: "The provider’s (\(tokens(128_000)))")
        default:
            return String(localized: "Found when the server refuses")
        }
    }

    private func autoContextLabel(_ c: HelperList.Context?) -> String {
        guard let c else { return String(localized: "Auto") }
        return String(localized: "Auto (\(tokens(c.auto)))")
    }

    private func tokens(_ n: Int) -> String {
        String(localized: "\(n) tokens")
    }

    /// The sizes offered under Auto: less than this Mac's memory holds.
    private static func contextSizes(_ c: HelperList.Context?) -> [Int] {
        let all = [32_768, 65_536, 131_072, 262_144]
        guard let c else { return all }
        return all.filter { $0 < c.auto }
    }

    /// The offered size nearest to what is set; Auto for none, or for one
    /// no less than Auto (valtzctl may set any).
    private static func contextChoice(_ set: Int,
                                      _ c: HelperList.Context?) -> Int {
        guard set > 0 else { return 0 }
        if let c, set >= c.auto { return 0 }
        let offered = contextSizes(c)
        return offered.min { abs($0 - set) < abs($1 - set) } ?? 0
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
        .settingsMark(String(localized: "Drafter"),
                      String(localized: "Multi-token prediction"))
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

    /// How often a video summary looks at the clip: Auto -- one frame a
    /// second, or one every two on a Mac whose memory moves less than
    /// 200 GB/s -- or as chosen.
    @ViewBuilder
    private func videoSection(_ list: HelperList) -> some View {
        Section {
            Picker("Frames", selection: Binding(
                get: { Self.everyChoice(list.videoEvery ?? 0) },
                set: { model.setVideoEvery($0) })) {
                Text(autoEveryLabel(list)).tag(0.0)
                Divider()
                Text("One a second").tag(1.0)
                Text("One every 2 seconds").tag(2.0)
                Text("One every 3 seconds").tag(3.0)
                Text("One every 5 seconds").tag(5.0)
                Text("One every 10 seconds").tag(10.0)
            }
            .settingsMark(String(localized: "Frames"))
        } header: {
            Text("Video Summary")
                .settingsMark(String(localized: "Video Summary"))
        } footer: {
            VStack(alignment: .leading, spacing: 4) {
                Text("A clip is read a frame at a time, 576 × 320 at most, cut into scenes where the picture changes, and each scene told in a few sentences, then the whole. More frames see more and take longer: each is about 180 of the model's words to read.")
                if let bw = list.memoryBandwidthGbs, bw > 0 {
                    Text("Auto reads one a second, or one every 2 seconds on a Mac whose memory moves less than 200 GB/s. This Mac's moves \(String(Int(bw.rounded()))) GB/s.")
                }
            }
            .foregroundStyle(.secondary)
        }
    }

    /// What Auto comes to here, by the memory's bandwidth (the core's
    /// rule: Controller::video_every).
    private func autoEveryLabel(_ list: HelperList) -> String {
        let bw = list.memoryBandwidthGbs ?? 0
        return bw > 0 && bw < 200
            ? String(localized: "Auto (one every 2 seconds)")
            : String(localized: "Auto (one a second)")
    }

    /// The offered interval nearest to what is set (valtzctl may set any).
    private static func everyChoice(_ s: Double) -> Double {
        guard s > 0 else { return 0 }
        let offered: [Double] = [1, 2, 3, 5, 10]
        return offered.min { abs($0 - s) < abs($1 - s) } ?? 1
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

/// Settings › Experimental › Agent (DESIGN §9b): where the agent's model
/// thinks -- the helper (this Mac's, or a fleet member's, as Offload
/// Policies send agent turns) or a cloud API -- whether each step it takes
/// waits for the person, and a cloud's provider, model and key (into the
/// Keychain, never kept here).
struct AgentSettingsSection: View {
    @Bindable var model: AppModel
    /// The key typed, until it is saved: never written anywhere else.
    @State private var key = ""

    var body: some View {
        if let s = model.agentSettings {
            Section {
                Picker("Thinks On", selection: Binding(
                    get: { s.backend },
                    set: { model.setAgentSettings(["backend": $0]) })) {
                    Text("Agentic Helper").tag("helper")
                    Text("Cloud").tag("cloud")
                }
                .settingsMark(String(localized: "Thinks On"),
                              String(localized: "Cloud"))
                Toggle("Ask before each step", isOn: Binding(
                    get: { s.review },
                    set: { model.setAgentSettings(["review": $0]) }))
                    .settingsMark(String(localized: "Ask before each step"))
                if s.backend == "helper" {
                    Picker("A member keeps its context", selection: Binding(
                        get: { s.lease },
                        set: { model.setAgentSettings(["lease": $0]) })) {
                        Text("5 minutes").tag(300.0)
                        Text("10 minutes").tag(600.0)
                        Text("30 minutes").tag(1800.0)
                        Text("1 hour").tag(3600.0)
                    }
                    .settingsMark(String(localized: "A member keeps its context"))
                } else {
                    cloudRows(s)
                }
            } header: {
                Text("Agent")
                    .settingsMark(String(localized: "Agent"))
            } footer: {
                Text(s.backend == "cloud"
                     ? "The agent's turns go to the provider: your requests, its steps and the names and words in the project. Pictures it looks at go too."
                     : "Its turns run on this Mac's helper -- or on a fleet member's, as Offload Policies send Agent Turns: a Mac with the memory for the model, beside one with the GPU for the pictures. A member keeps the conversation in memory between turns, as long as set here.")
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private func cloudRows(_ s: AgentSettings) -> some View {
        Picker("Provider", selection: Binding(
            get: { s.cloud.provider },
            set: { model.setAgentSettings(["cloud": ["provider": $0,
                                                     "model": ""]]) })) {
            Text("Anthropic").tag("anthropic")
            Text("OpenAI").tag("openai")
            Text("OpenAI-compatible server").tag("openai-compatible")
        }
        .settingsMark(String(localized: "Provider"))
        TextField("Model", text: Binding(
            get: { s.cloud.model },
            set: { model.setAgentSettings(["cloud": ["model": $0]]) }))
            .settingsMark(String(localized: "Model"))
        if s.cloud.provider == "openai-compatible" {
            TextField("Server", text: Binding(
                get: { s.cloud.baseUrl },
                set: { model.setAgentSettings(["cloud": ["base_url": $0]]) }),
                prompt: Text(verbatim: "http://localhost:1234/v1"))
                .settingsMark(String(localized: "Server"))
        }
        LabeledContent("API Key") {
            HStack {
                SecureField("API Key", text: $key)
                    .labelsHidden()
                    .frame(maxWidth: 220)
                Button("Save") {
                    model.setAgentKey(s.cloud.provider, key)
                    key = ""
                }
                .disabled(key.isEmpty)
                if s.keys[s.cloud.provider] == true {
                    Button("Remove") {
                        model.setAgentKey(s.cloud.provider, "")
                    }
                }
            }
        }
        .settingsMark(String(localized: "API Key"))
        Text(s.keys[s.cloud.provider] == true
             ? "A key is kept in your Keychain."
             : "No key is kept yet.")
            .font(.caption)
            .foregroundStyle(.secondary)
    }
}
