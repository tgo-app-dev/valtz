import SwiftUI

/// THE AGENT AS A CHAT (DESIGN §9b, §10c): the Prompt Editor's card in the
/// Agent tab, laid out as a language model's chat rather than a text
/// editor -- the conversation from the top, the person's words in bubbles
/// on the right (with the pictures they attached), the agent's words on
/// the left and its steps under the turn that asked for them, each one
/// opened for what it was asked and what came back; a STATUS ROW that
/// stays above the input -- what it is doing, where its model thinks, how
/// full that model's context is (a ring), its controls -- and the input
/// itself, a field at the card's foot (ComposerStack.chatInput) that a
/// picture is pasted or dropped into, Return sending.
struct AgentChat: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            // Its top row: the hold's note, while there is one, and the
            // way out of the chat at the right.
            HStack(alignment: .top, spacing: 8) {
                AgentHoldNote(model: model)
                Spacer(minLength: 0)
                AgentChatClose(model: model)
            }
            .padding(.leading, 18)
            .padding(.trailing, 8)
            .padding(.top, 6)
            if let a = model.agent {
                AgentConversation(model: model, shown: a)
            } else {
                AgentChatWelcome(model: model)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // Pictures dropped anywhere on it go into the input's row.
        .onDrop(of: DroppedMedia.dropTypes,
                delegate: PromptRowDrop(model: model))
        .onGeometryChange(for: CGRect.self) {
            $0.frame(in: .global)
        } action: { model.agentChatFrame = $0 }
    }
}

/// The chat's (x): out of it, back to the prompt box -- the Prompt
/// Editor folded, the Agent target kept (⇧⌘E too). Its conversations go
/// on; the tabs are there when it opens again.
private struct AgentChatClose: View {
    @Bindable var model: AppModel

    var body: some View {
        Button {
            withAnimation(AppModel.motion) { model.leaveAgentChat() }
        } label: {
            Image(systemName: "xmark.circle.fill")
                .font(.system(size: 16))
                .frame(width: 26, height: 26)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.tertiary)
        .help("Leave the chat: back to the prompt box (⇧⌘E)")
        .accessibilityLabel(Text("Leave Chat"))
        .shortcutHint("⇧⌘E")
    }
}

/// No session yet: what the agent does, and that pictures go in with the
/// words.
private struct AgentChatWelcome: View {
    @Bindable var model: AppModel

    var body: some View {
        let skills = model.agentSkills
        VStack(spacing: 10) {
            Spacer(minLength: 20)
            Image(systemName: AppModel.agentSkillsSymbol(skills))
                .font(.system(size: 30, weight: .light))
                .foregroundStyle(.tertiary)
            switch skills {
            case "chat":
                Text("Chat: Look and Add")
                    .font(.title3.weight(.semibold))
                Text("It looks at your project — pictures, pages, clips, sounds — and adds to Assets: pictures it makes, texts it keeps. It changes nothing that is there: the tab with Valtz’s tools does. Paste or drop pictures here to talk about them.")
                    .modifier(WelcomeText())
            case "none":
                Text("Chat Without Tools")
                    .font(.title3.weight(.semibold))
                Text("Words alone: no Valtz tools, so its context is the conversation’s. Paste or drop pictures here: they are shown to it with your words.")
                    .modifier(WelcomeText())
            default:
                Text("Ask the Agent")
                    .font(.title3.weight(.semibold))
                Text("It works on the document with the editor’s tools — pages, texts, pictures, clips and music — step by step, as you watch. Paste or drop pictures here to show it what you mean.")
                    .modifier(WelcomeText())
                Text("Works on \(model.activeComposition?.name ?? String(localized: "a new composition"))")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
            }
            Spacer(minLength: 20)
        }
        .padding(.horizontal, 24)
        .frame(maxWidth: .infinity)
    }
}

/// The welcome's words: centred, wrapped.
private struct WelcomeText: ViewModifier {
    func body(content: Content) -> some View {
        content
            .font(.callout)
            .foregroundStyle(.secondary)
            .multilineTextAlignment(.center)
            .frame(maxWidth: 420)
            .fixedSize(horizontal: false, vertical: true)
    }
}

/// The tab's Valtz tools: chosen before its first words -- Valtz's own
/// (one tab at a time: offered greyed while another has them), Look and
/// Add, or none -- and said after.
private struct AgentToolsChoice: View {
    @Bindable var model: AppModel

    var body: some View {
        let skills = model.agent?.skills ?? model.agentSkills
        if model.agent == nil {
            Picker(selection: Binding(
                get: { model.agentSkills },
                set: { model.setAgentSkills($0) })) {
                ForEach(["full", "chat", "none"], id: \.self) { k in
                    Label(AppModel.agentSkillsName(k),
                          systemImage: AppModel.agentSkillsSymbol(k))
                        .tag(k)
                        .selectionDisabled(k == "full"
                            && model.agentEditorTab.map {
                                $0 != model.agentTabIndex } == true)
                }
            } label: {
                Text("Tools")
            }
            .labelsHidden()
            .pickerStyle(.menu)
            .fixedSize()
            .help(AppModel.agentSkillsHelp(skills))
        } else {
            Label(AppModel.agentSkillsName(skills),
                  systemImage: AppModel.agentSkillsSymbol(skills))
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .help(AppModel.agentSkillsHelp(skills))
        }
    }
}

/// The conversation, newest at the foot: kept there as it grows.
private struct AgentConversation: View {
    @Bindable var model: AppModel
    let shown: AgentSession

    /// The model's (AgentCard.session).
    var session: AgentSession { model.agent ?? shown }

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 14) {
                    ForEach(Array(entries.enumerated()), id: \.offset) {
                        _, e in entry(e)
                    }
                    tail
                    Color.clear.frame(height: 1).id("end")
                }
                .padding(.horizontal, 18)
                .padding(.vertical, 14)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .onAppear { proxy.scrollTo("end", anchor: .bottom) }
            .onChange(of: scrollKey) {
                withAnimation(.smooth(duration: 0.2)) {
                    proxy.scrollTo("end", anchor: .bottom)
                }
            }
        }
    }

    /// What the chat lists, in order.
    private enum Entry {
        case person(String, attached: [String], where: String?)
        case words(String)
        case steps([AgentStep])
        case question(String)
        case note(String)
        case compaction(Int)
        case speed(AgentTurnStats)
    }

    private var entries: [Entry] {
        var out: [Entry] = []
        var next = 0
        let steps = session.steps
        func takeSteps(_ n: Int) {
            var run: [AgentStep] = []
            for _ in 0..<n where next < steps.count {
                let s = steps[next]
                next += 1
                // Its question is a bubble of the agent's, the answer the
                // person's.
                if s.tool == "ask_user" {
                    if !run.isEmpty { out.append(.steps(run)); run = [] }
                    out.append(.question(s.question ?? ""))
                    if let a = Self.answer(s) {
                        out.append(.person(a, attached: [], where: nil))
                    }
                    continue
                }
                run.append(s)
            }
            if !run.isEmpty { out.append(.steps(run)) }
        }
        for m in session.messages ?? [] {
            switch m.role {
            case "user":
                if m.said == nil, m.content.hasPrefix("[Valtz]") {
                    out.append(.note(String(m.content.dropFirst(8))))
                } else {
                    out.append(.person(
                        m.said ?? Self.personWords(m.content),
                        attached: m.attached ?? [],
                        where: Self.whereSaid(m.content)))
                }
            case "compaction":
                out.append(.compaction(m.compacted ?? 0))
            case "assistant":
                let w = m.content.trimmingCharacters(
                    in: .whitespacesAndNewlines)
                if !w.isEmpty { out.append(.words(w)) }
                takeSteps(m.calls?.count ?? 0)
                if model.agentShowsSpeed, let st = m.stats, !st.isEmpty {
                    out.append(.speed(st))
                }
            default:
                break
            }
        }
        // Steps not yet in a message the core has written back.
        takeSteps(steps.count - next)
        return out
    }

    /// What is being written now, the step waiting, an error: last.
    @ViewBuilder
    private var tail: some View {
        let words = model.agentWriting.trimmingCharacters(
            in: .whitespacesAndNewlines)
        if session.state == "thinking" || session.state == "starting" {
            if words.isEmpty {
                ThinkingDots()
            } else {
                Text(agentMarkdown(words))
                    .font(.body)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        if let step = session.proposed {
            AgentReviewButtons(model: model, step: step)
                .padding(10)
                .background(Color.accentColor.opacity(0.08),
                            in: RoundedRectangle(cornerRadius: 10))
        }
        if let e = session.errorText, session.state == "failed" {
            VStack(alignment: .leading, spacing: 6) {
                Label(e, systemImage: "exclamationmark.triangle")
                    .font(.callout)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
                Button("Try Again") { model.agentControl("resume") }
                    .controlSize(.small)
            }
        }
        if session.state == "asking" {
            Text("Answer in the box below.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var scrollKey: String {
        "\(model.agentWriting.count)|\(session.steps.count)|"
            + "\(session.steps.last?.state ?? "")|\(session.state)|"
            + "\(session.messages?.count ?? 0)"
    }

    @ViewBuilder
    private func entry(_ e: Entry) -> some View {
        switch e {
        case .person(let t, let attached, let place):
            PersonBubble(model: model, words: t, attached: attached,
                         place: place)
        case .words(let t):
            Text(agentMarkdown(t))
                .font(.body)
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
        case .steps(let steps):
            AgentStepGroup(steps: steps)
        case .question(let q):
            Label {
                Text(q)
                    .font(.body.weight(.medium))
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            } icon: {
                Image(systemName: "questionmark.bubble")
                    .foregroundStyle(Color.accentColor)
            }
        case .note(let t):
            Text(t.trimmingCharacters(in: .whitespacesAndNewlines))
                .font(.caption)
                .foregroundStyle(.tertiary)
                .lineLimit(3)
                .frame(maxWidth: .infinity, alignment: .center)
                .multilineTextAlignment(.center)
        case .compaction(let n):
            HStack(spacing: 8) {
                VStack { Divider() }
                Text("Compacted: the \(n) messages above summarized for the model")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
                    .layoutPriority(1)
                VStack { Divider() }
            }
        case .speed(let st):
            AgentSpeedText(stats: st)
                .padding(.top, -8)
        }
    }

    /// An ask's answer, as the person typed it.
    static func answer(_ s: AgentStep) -> String? {
        guard let text = s.resultText, let data = text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data)
                as? [String: Any],
              let a = j["answer"] as? String, !a.isEmpty else { return nil }
        return personWords(a)
    }

    /// Where the core tells where the person is, after their words.
    private static let whereOpen = "\n\n[Where the person is as they say this: "

    /// The person's words in a message saved before they were kept
    /// apart: its content up to where they were, and Valtz's notes.
    static func personWords(_ content: String) -> String {
        var t = content
        for cut in [whereOpen, "\n[Valtz]", "\n[The person attached:"] {
            if let r = t.range(of: cut) { t = String(t[..<r.lowerBound]) }
        }
        return t
    }

    /// Where they were as they said it ("“Deck” (composition …), on page
    /// 2"), nil where nothing was told.
    static func whereSaid(_ content: String) -> String? {
        guard let r = content.range(of: whereOpen) else { return nil }
        let rest = content[r.upperBound...]
        let end = rest.range(of: ". \"This page\"")?.lowerBound
            ?? rest.firstIndex(of: "]") ?? rest.endIndex
        let w = String(rest[..<end])
        return w.isEmpty ? nil : w
    }
}

/// The person's words: a bubble on the right, the pictures they attached
/// over it, where they were under it.
private struct PersonBubble: View {
    @Bindable var model: AppModel
    let words: String
    let attached: [String]
    let place: String?

    var body: some View {
        VStack(alignment: .trailing, spacing: 6) {
            if !attached.isEmpty {
                HStack(spacing: 6) {
                    ForEach(attached, id: \.self) { id in
                        AttachedThumb(model: model, asset: id)
                    }
                }
            }
            Text(words.trimmingCharacters(in: .whitespacesAndNewlines))
                .font(.body)
                .textSelection(.enabled)
                .padding(.horizontal, 12)
                .padding(.vertical, 8)
                .background(Color.accentColor.opacity(0.16),
                            in: RoundedRectangle(cornerRadius: 16,
                                                 style: .continuous))
                .fixedSize(horizontal: false, vertical: true)
            if let place {
                Label(place, systemImage: "location")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(place)
            }
        }
        .frame(maxWidth: .infinity, alignment: .trailing)
        .padding(.leading, 60)
    }
}

/// An asset attached to the person's words, as Assets draws it.
private struct AttachedThumb: View {
    @Bindable var model: AppModel
    let asset: String
    @State private var image: CGImage?

    var body: some View {
        let a = model.asset(id: asset)
        RoundedRectangle(cornerRadius: 8, style: .continuous)
            .fill(Color.primary.opacity(0.08))
            .overlay {
                if let image {
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                } else {
                    Image(systemName: a?.kind == "audio" ? "waveform"
                          : a?.kind == "video" ? "film" : "photo")
                        .foregroundStyle(.tertiary)
                }
            }
            .frame(width: 64, height: 64)
            .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            .help(a?.name ?? String(localized: "An asset since removed"))
            .task(id: asset) {
                guard let core = model.core, let project = model.projectId,
                      a?.kind != "audio", a?.kind != "text" else { return }
                let id = asset
                image = await Task.detached(priority: .utility) {
                    core.thumbnail(project: project, asset: id,
                                   maxPixels: 128)
                        .flatMap { AppModel.loadImage($0) }
                }.value
            }
    }
}

/// A turn's steps, one box: each row its glyph, title and how it went,
/// opened for what it was asked and what came back.
private struct AgentStepGroup: View {
    let steps: [AgentStep]

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            ForEach(steps) { s in
                AgentStepDisclosure(step: s)
                if s.n != steps.last?.n {
                    Divider().padding(.leading, 30)
                }
            }
        }
        .padding(.vertical, 2)
        .background(Color.primary.opacity(0.04),
                    in: RoundedRectangle(cornerRadius: 10,
                                         style: .continuous))
        .overlay(RoundedRectangle(cornerRadius: 10, style: .continuous)
            .strokeBorder(Color.primary.opacity(0.07), lineWidth: 0.5))
    }
}

private struct AgentStepDisclosure: View {
    let step: AgentStep
    @State private var open = false

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Button {
                withAnimation(.smooth(duration: 0.2)) { open.toggle() }
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: AppModel.agentStepSymbol(step.state))
                        .foregroundStyle(tint)
                        .frame(width: 16)
                        .symbolEffect(.pulse, isActive: step.state == "running"
                                      || step.state == "waiting")
                    Text(AppModel.agentToolTitle(step.tool))
                        .font(.callout.weight(.medium))
                        .foregroundStyle(.primary)
                    if let e = step.error, !e.isEmpty, !open {
                        Text(e)
                            .font(.caption)
                            .foregroundStyle(.orange)
                            .lineLimit(1)
                            .truncationMode(.tail)
                    }
                    Spacer(minLength: 6)
                    Image(systemName: "chevron.right")
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.tertiary)
                        .rotationEffect(.degrees(open ? 90 : 0))
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            if open {
                VStack(alignment: .leading, spacing: 4) {
                    if let a = step.argsText, a != "{}" {
                        Text(a)
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    if let e = step.error, !e.isEmpty {
                        Text(e)
                            .font(.caption)
                            .foregroundStyle(.orange)
                            .textSelection(.enabled)
                            .fixedSize(horizontal: false, vertical: true)
                    } else if let r = step.resultText {
                        Text(r)
                            .font(.caption.monospaced())
                            .foregroundStyle(.tertiary)
                            .lineLimit(12)
                            .textSelection(.enabled)
                    }
                }
                .padding(.leading, 24)
                .transition(.opacity)
            }
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 7)
    }

    private var tint: Color {
        switch step.state {
        case "proposed": .accentColor
        case "failed": .orange
        case "skipped": .secondary
        case "done": .green
        default: .secondary
        }
    }
}

/// Three dots breathing: its words are on their way.
private struct ThinkingDots: View {
    var body: some View {
        TimelineView(.animation(minimumInterval: 1 / 15)) { t in
            let phase = t.date.timeIntervalSinceReferenceDate * 3
            HStack(spacing: 5) {
                ForEach(0..<3) { i in
                    Circle()
                        .fill(Color.secondary)
                        .frame(width: 6, height: 6)
                        .opacity(0.3 + 0.7 * max(0, sin(phase - Double(i))))
                }
            }
        }
        .frame(height: 16)
        .accessibilityLabel(Text("Thinking…"))
    }
}

// MARK: The status row

/// Above the input, always: what the session is doing, where its model
/// thinks, its controls, its transcript, and how full its model's
/// context is.
struct AgentStatusRow: View {
    @Bindable var model: AppModel

    static let height: CGFloat = 32

    var body: some View {
        let a = model.agent
        HStack(spacing: 8) {
            if let a {
                Label(model.agentStateLabel, systemImage: stateSymbol(a))
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(a.state == "failed" ? Color.orange
                                     : .secondary)
                    .lineLimit(1)
                if a.busy {
                    ProgressView().controlSize(.mini)
                }
            } else {
                Label("Ready", systemImage: "circle.dotted")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
            }
            AgentToolsChoice(model: model)
            Text(model.agentBackendLabel)
                .font(.caption)
                .foregroundStyle(.tertiary)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer(minLength: 8)
            controls(a)
            ContextRing(context: model.agentContext,
                        compactions: a?.compactions ?? 0)
        }
        .controlSize(.small)
        .frame(height: Self.height)
    }

    @ViewBuilder
    private func controls(_ a: AgentSession?) -> some View {
        // No tools, no steps to ask about.
        if (a?.skills ?? model.agentSkills) != "none" {
            askFirst(a)
        }
        sessionControls(a)
    }

    private func askFirst(_ a: AgentSession?) -> some View {
        Toggle("Ask First", isOn: Binding(
            get: { a?.review ?? model.agentSettings?.review ?? true },
            set: { on in
                if a != nil && a?.ended == false {
                    model.agentControl(on ? "review" : "auto")
                } else {
                    model.setAgentSettings(["review": on])
                }
            }))
            .toggleStyle(.checkbox)
            .help("Each step that changes the document waits for your word")
    }

    @ViewBuilder
    private func sessionControls(_ a: AgentSession?) -> some View {
        if let a {
            if a.state == "paused" {
                Button("Resume") { model.agentControl("resume") }
            } else if !a.ended && !a.listening {
                Button("Pause") { model.agentControl("pause") }
                    .help("It stops after the step it is on")
            }
            if a.ended || a.listening {
                Button("New Chat") { model.agentDismiss() }
                    .help("Put this conversation away: the next words start a new one. Its transcript stays in Assets.")
            } else {
                Button("Stop") { model.agentControl("stop") }
                    .help("End the session; what it made stays")
            }
            if a.transcript != nil {
                Button {
                    model.openAgentTranscript()
                } label: {
                    Image(systemName: "doc.text")
                        .frame(width: 20, height: 18)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.borderless)
                .help("Its transcript, in Assets: the conversation and each step, to review")
                .accessibilityLabel(Text("Transcript"))
            }
        }
    }

    private func stateSymbol(_ a: AgentSession) -> String {
        switch a.state {
        case "review", "asking": "hand.raised"
        case "paused": "pause.circle"
        case "done": "checkmark.circle"
        case "failed": "exclamationmark.triangle"
        case "stopped": "stop.circle"
        default: "wand.and.stars"
        }
    }
}

/// How full the model's context is: a ring filling clockwise, its share
/// beside it -- the accent's, orange from 70%, red near where the
/// conversation is compacted (or, compaction off, where it stops).
struct ContextRing: View {
    let context: AgentContext?
    var compactions = 0

    var body: some View {
        let f = context?.fraction
        HStack(spacing: 5) {
            ZStack {
                Circle()
                    .stroke(Color.primary.opacity(0.14), lineWidth: 2.5)
                Circle()
                    .trim(from: 0, to: f ?? 0)
                    .stroke(tint(f ?? 0),
                            style: StrokeStyle(lineWidth: 2.5,
                                               lineCap: .round))
                    .rotationEffect(.degrees(-90))
                    .animation(.smooth(duration: 0.4), value: f)
                // Where it is compacted: a tick on the ring.
                if let at = context?.compactAt, at > 0 {
                    Capsule()
                        .fill(Color.primary.opacity(0.35))
                        .frame(width: 1.5, height: 4)
                        .offset(y: -6.5)
                        .rotationEffect(.degrees(Double(at) * 3.6))
                }
            }
            .frame(width: 15, height: 15)
            Text(verbatim: f.map { "\(Int(($0 * 100).rounded()))%" } ?? "–")
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(minWidth: 30, alignment: .leading)
        }
        .help(help)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text("Context"))
        .accessibilityValue(Text(help))
    }

    private func tint(_ f: Double) -> Color {
        let at = Double(context?.compactAt ?? 90) / 100
        let near = at > 0 ? at - 0.05 : 0.95
        if f >= near { return .red }
        if f >= 0.7 { return .orange }
        return .accentColor
    }

    private var help: String {
        guard let c = context, let limit = c.limit, limit > 0 else {
            return String(localized: "Context: this model’s size is not known.")
        }
        let used = c.used ?? 0
        let pct = Int(((c.fraction ?? 0) * 100).rounded())
        var t = used == 0
            ? String(localized: "Context: \(limit) tokens. Nothing said yet.")
            : String(localized: "Context: about \(used) of \(limit) tokens (\(pct)%).")
        if let at = c.compactAt, at > 0 {
            t += " " + String(localized: "Past \(at)%, the conversation is compacted to go on.")
        } else {
            t += " " + String(localized: "Compaction is off: it stops when the context is full.")
        }
        if compactions == 1 {
            t += " " + String(localized: "Compacted once so far.")
        } else if compactions > 1 {
            t += " " + String(localized: "Compacted \(compactions) times so far.")
        }
        return t
    }
}
