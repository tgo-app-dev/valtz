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
            Text("DFlash 2 · 8-bit (about \(held(d, 8)) more)")
                .tag("dflash:8")
            Text("DFlash 2 · 4-bit (about \(held(d, 4)) more)")
                .tag("dflash:4")
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

    /// What the drafter holds beside the model at `bits`, as the catalog
    /// has it -- the 27B's 2.2 GB at 8, Flash-Next's 0.6 -- "1.3 GB".
    private func held(_ d: HelperList.Drafter, _ bits: Int) -> String {
        let gb = d.heldGb?[String(bits)]
            ?? (d.diskGb ?? 0) * (bits == 8 ? 0.58 : 0.34)
        return gb.formatted(.number.precision(.fractionLength(0...2)))
            + " GB"
    }

    /// How often a video summary looks at the clip -- Auto by its length,
    /// up to eight frames a second for a short one, or as chosen -- and
    /// how large each frame is read (DESIGN §4i).
    @ViewBuilder
    private func videoSection(_ list: HelperList) -> some View {
        Section {
            Picker("Frames", selection: Binding(
                get: { Self.everyChoice(list.videoEvery ?? 0) },
                set: { model.setVideoEvery($0) })) {
                Text("Auto (by the clip's length)").tag(0.0)
                Divider()
                Text("8 a second").tag(0.125)
                Text("4 a second").tag(0.25)
                Text("2 a second").tag(0.5)
                Text("One a second").tag(1.0)
                Text("One every 2 seconds").tag(2.0)
                Text("One every 3 seconds").tag(3.0)
                Text("One every 5 seconds").tag(5.0)
                Text("One every 10 seconds").tag(10.0)
            }
            .settingsMark(String(localized: "Frames"))
            Picker("Frame Size", selection: Binding(
                get: { list.videoSize ?? "standard" },
                set: { model.setVideoSize($0) })) {
                Text("Standard (576 × 320)").tag("standard")
                Text("High (864 × 480)").tag("high")
                Text("Highest (1152 × 640)").tag("highest")
            }
            .settingsMark(String(localized: "Frame Size"))
        } header: {
            Text("Video Summary")
                .settingsMark(String(localized: "Video Summary"))
        } footer: {
            VStack(alignment: .leading, spacing: 4) {
                Text("A clip is read a frame at a time, landscape or portrait, upright, cut into scenes where the picture changes, and each scene told in a few sentences, then the whole. More frames, and larger ones, see more and take longer: a frame is about 180 of the model's words to read at Standard, 400 at High, 720 at Highest.")
                Text(autoEveryNote(list))
            }
            .foregroundStyle(.secondary)
        }
    }

    /// What Auto does here, by the memory's bandwidth (the core's rule:
    /// Controller::video_every).
    private func autoEveryNote(_ list: HelperList) -> String {
        let bw = list.memoryBandwidthGbs ?? 0
        guard bw > 0 && bw < 200 else {
            return String(localized: "Auto reads about 80 frames of a short clip: 8 a second up to 10 seconds, 4 up to 20, 2 up to 40, then one a second.")
        }
        return String(localized: "This Mac's memory moves \(String(Int(bw.rounded()))) GB/s, under 200, so Auto reads about 40 frames of a short clip: 8 a second up to 5 seconds, 4 up to 10, 2 up to 20, 1 up to 40, then one every 2 seconds.")
    }

    /// The offered interval nearest to what is set (valtzctl may set any),
    /// by ratio: 0.2 s is 4 a second's.
    private static func everyChoice(_ s: Double) -> Double {
        guard s > 0 else { return 0 }
        let offered: [Double] = [0.125, 0.25, 0.5, 1, 2, 3, 5, 10]
        return offered.min { abs(log($0 / s)) < abs(log($1 / s)) } ?? 1
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
/// Policies send agent turns) or a cloud API -- which model it is here
/// (the Agentic Helper's, or one of its own: DeepSeek V4.1 Flash) and the
/// most that model may hold, whether each step it takes waits for the
/// person, and a cloud's provider, model and key (into the Keychain,
/// never kept here).
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
                    modelRows(s)
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

    /// The model it thinks with here -- the Agentic Helper's, or a
    /// helper of its own -- and the most that model may hold: its expert
    /// cache sized within it (a Mixture of Experts too large for the Mac
    /// keeps the experts it can and reads the rest from disk), the rest of
    /// the memory left to the pictures and clips.
    @ViewBuilder
    private func modelRows(_ s: AgentSettings) -> some View {
        let list = model.helpers
        Picker("Model", selection: Binding(
            get: { s.model ?? "" },
            set: { model.setAgentSettings(["model": $0]) })) {
            Text(sameLabel(list)).tag("")
            Divider()
            ForEach(list?.models ?? []) { h in
                Text(label(h)).tag(h.id)
            }
            if let id = s.model, !id.isEmpty,
               list?.helper(id) == nil {
                Text(verbatim: id).tag(id)
            }
        }
        .settingsMark(String(localized: "Model"),
                      String(localized: "DeepSeek"))
        if let id = s.model, !id.isEmpty, let using = s.using, using != id,
           let chosen = list?.helper(id) {
            LabeledContent {
                Button("Open Capabilities") {
                    model.openCapabilities(family: "helpers")
                }
            } label: {
                Text("\(chosen.name) is not downloaded: \(name(using, list)) thinks until it is.")
                    .foregroundStyle(.secondary)
            }
        }
        Picker("Memory", selection: Binding(
            get: { s.memoryGb ?? 0 },
            set: { model.setAgentSettings(["memory_gb": $0]) })) {
            Text("No Limit").tag(0)
            Divider()
            ForEach(Self.memorySizes, id: \.self) { gb in
                Text("\(String(gb)) GB").tag(gb)
            }
            if let gb = s.memoryGb, gb > 0, !Self.memorySizes.contains(gb) {
                Text("\(String(gb)) GB").tag(gb)
            }
        }
        .settingsMark(String(localized: "Memory"),
                      String(localized: "Agent Memory"))
        Text(memoryNote(s))
            .font(.caption)
            .foregroundStyle(.secondary)
    }

    private static let memorySizes = [64, 128, 192]

    private func sameLabel(_ list: HelperList?) -> String {
        guard let list, let h = list.helper(list.using) else {
            return String(localized: "Agentic Helper")
        }
        return String(localized: "Agentic Helper (\(h.name))")
    }

    private func name(_ id: String, _ list: HelperList?) -> String {
        list?.helper(id)?.name ?? id
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

    /// What the limit does on this Mac.
    private func memoryNote(_ s: AgentSettings) -> String {
        let ram = model.hardware?.ramGb ?? 0
        if let gb = s.memoryGb, gb > 0, ram > 0, gb >= ram {
            return String(localized: "This Mac has \(String(ram)) GB, so the limit changes nothing here: the model sizes itself to the Mac.")
        }
        return String(localized: "The most the agent's model holds: its weights, its expert cache and its conversation. A model whose experts run from a cache -- DeepSeek V4.1 Flash, Qwen3.8 Flash-Next -- keeps what fits and reads the rest from disk, more slowly; a smaller model loads whole. The rest of the memory is left to pictures, clips and sounds. For the model's next load: one kept loaded keeps its size until it unloads.")
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
