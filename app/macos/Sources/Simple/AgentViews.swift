import SwiftUI

/// THE AGENT at work (DESIGN §9b), over the prompt: what it is doing, its
/// words as they come, the step waiting for the person's word (Run, Skip
/// -- a note told to it -- Run All), and Pause, Stop. The Prompt Editor
/// shows all of it (AgentPane).
struct AgentCard: View {
    @Bindable var model: AppModel
    let session: AgentSession

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            AgentHeader(model: model, session: session)
            let words = model.agentWriting.trimmingCharacters(
                in: .whitespacesAndNewlines)
            if !words.isEmpty {
                Text(agentMarkdown(words))
                    .font(.callout)
                    .lineLimit(4)
                    .truncationMode(.head)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let e = session.errorText, session.state == "failed" {
                Label(e, systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundStyle(.orange)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let step = session.proposed {
                AgentStepRow(step: step, compact: true)
                AgentReviewButtons(model: model, step: step)
            } else if let step = session.steps.last {
                AgentStepRow(step: step, compact: true)
            }
            if session.state == "asking",
               let q = session.steps.last?.question {
                Label(q, systemImage: "questionmark.bubble")
                    .font(.callout.weight(.medium))
                    .fixedSize(horizontal: false, vertical: true)
                Text("Answer in the box below.")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            // The last turn's speed, for debugging.
            if model.agentShowsSpeed, let st = session.stats, !st.isEmpty {
                AgentSpeedText(stats: st)
            }
        }
        .padding(16)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 18,
                                                    style: .continuous))
    }
}

/// The session's state, where its model thinks, and its controls.
struct AgentHeader: View {
    @Bindable var model: AppModel
    let session: AgentSession

    var body: some View {
        HStack(spacing: 8) {
            Label(model.agentStateLabel, systemImage: "wand.and.stars")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .lineLimit(1)
            if session.busy {
                ProgressView().controlSize(.mini)
            }
            Text(model.agentBackendLabel)
                .font(.caption)
                .foregroundStyle(.tertiary)
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer(minLength: 8)
            Toggle("Ask First", isOn: Binding(
                get: { session.review },
                set: { model.agentControl($0 ? "review" : "auto") }))
                .toggleStyle(.checkbox)
                .help("Each step that changes the document waits for your word")
                .disabled(session.ended)
            if session.state == "paused" {
                Button("Resume") { model.agentControl("resume") }
            } else if !session.ended && !session.listening {
                Button("Pause") { model.agentControl("pause") }
                    .help("It stops after the step it is on")
            }
            if session.ended || session.state == "done" {
                Button("Close") { model.agentDismiss() }
                    .help("Put it away: the next request starts anew")
            } else {
                Button("Stop") { model.agentControl("stop") }
                    .help("End the session; what it made stays")
            }
        }
        .controlSize(.small)
    }
}

/// Run, Skip (with a note), Run All -- for the step waiting.
struct AgentReviewButtons: View {
    @Bindable var model: AppModel
    let step: AgentStep

    var body: some View {
        HStack(spacing: 8) {
            TextField("Note for Skip (optional)", text: $model.agentNote)
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 220)
            Spacer(minLength: 8)
            Button("Skip") { model.agentSkip(step) }
                .help("Not this one: it is told, with your note")
            Button("Run All") { model.agentApproveAll(step) }
                .help("This one and every step after it, without asking")
            Button("Run") { model.agentApprove(step) }
                .buttonStyle(.borderedProminent)
                .keyboardShortcut(.defaultAction)
        }
        .controlSize(.small)
    }
}

/// One step: its glyph by state, its title, what it was asked to do, how
/// it went.
struct AgentStepRow: View {
    let step: AgentStep
    var compact = false

    var body: some View {
        HStack(alignment: .firstTextBaseline, spacing: 8) {
            Image(systemName: AppModel.agentStepSymbol(step.state))
                .foregroundStyle(tint)
                .frame(width: 16)
            VStack(alignment: .leading, spacing: 2) {
                Text(AppModel.agentToolTitle(step.tool))
                    .font(.callout.weight(.medium))
                if let a = step.argsText, a != "{}" {
                    Text(a)
                        .font(.caption.monospaced())
                        .foregroundStyle(.secondary)
                        .lineLimit(compact ? 2 : 6)
                        .textSelection(.enabled)
                }
                if let e = step.error, !e.isEmpty {
                    Text(e)
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .lineLimit(compact ? 2 : 4)
                } else if !compact, let r = step.resultText {
                    Text(r)
                        .font(.caption.monospaced())
                        .foregroundStyle(.tertiary)
                        .lineLimit(3)
                        .textSelection(.enabled)
                }
            }
        }
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

/// A turn's speed in small type -- what it read and wrote, how fast, the
/// conversation it went on from (Settings › Agentic Helper › Show Speed):
/// for the person debugging the agent.
struct AgentSpeedText: View {
    let stats: AgentTurnStats

    var body: some View {
        Text(stats.line)
            .font(.system(size: 9).monospacedDigit())
            .foregroundStyle(.tertiary)
            .lineLimit(2)
            .textSelection(.enabled)
            .fixedSize(horizontal: false, vertical: true)
    }
}

/// The Prompt Editor's right half while the agent works (DESIGN §10c):
/// the request, then each turn -- its words, its steps, its speed while
/// that is shown -- as they came, the step waiting with its buttons, the
/// words being written last.
struct AgentPane: View {
    @Bindable var model: AppModel
    let session: AgentSession

    var body: some View {
        VStack(spacing: 0) {
            AgentHeader(model: model, session: session)
                .padding(.leading, PromptEditor.inset.width + 5)
                .padding(.trailing, 12)
                .frame(height: PaneHeader.height)
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        ForEach(Array(entries.enumerated()), id: \.offset) {
                            _, e in entry(e)
                        }
                        let words = model.agentWriting.trimmingCharacters(
                            in: .whitespacesAndNewlines)
                        if session.state == "thinking", !words.isEmpty {
                            Text(agentMarkdown(words))
                                .font(.callout)
                                .foregroundStyle(.secondary)
                                .textSelection(.enabled)
                        }
                        if let step = session.proposed {
                            AgentReviewButtons(model: model, step: step)
                        }
                        if let e = session.errorText,
                           session.state == "failed" {
                            Label(e, systemImage: "exclamationmark.triangle")
                                .font(.callout)
                                .foregroundStyle(.orange)
                            Button("Try Again") {
                                model.agentControl("resume")
                            }
                            .controlSize(.small)
                        }
                        Color.clear.frame(height: 1).id("end")
                    }
                    .padding(.leading, PromptEditor.inset.width + 5)
                    .padding(.trailing, 16)
                    .padding(.vertical, 8)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .onChange(of: session.steps.count) {
                    withAnimation { proxy.scrollTo("end", anchor: .bottom) }
                }
                .onChange(of: model.agentWriting) {
                    proxy.scrollTo("end", anchor: .bottom)
                }
            }
        }
    }

    /// What the pane lists: the person's words, the model's, its steps.
    private enum Entry {
        case person(String)
        case words(String)
        case note(String)
        case step(AgentStep)
        case speed(AgentTurnStats)
    }

    private var entries: [Entry] {
        var out: [Entry] = []
        var next = 0
        for m in session.messages ?? [] {
            switch m.role {
            case "user":
                if m.content.hasPrefix("[Valtz]") {
                    out.append(.note(String(m.content.dropFirst(8))))
                } else {
                    out.append(.person(m.content))
                }
            case "assistant":
                let w = m.content.trimmingCharacters(
                    in: .whitespacesAndNewlines)
                if !w.isEmpty { out.append(.words(w)) }
                for _ in m.calls ?? [] where next < session.steps.count {
                    out.append(.step(session.steps[next]))
                    next += 1
                }
                if model.agentShowsSpeed, let st = m.stats, !st.isEmpty {
                    out.append(.speed(st))
                }
            default:
                break
            }
        }
        // Steps not yet in a message the core has written back.
        while next < session.steps.count {
            out.append(.step(session.steps[next]))
            next += 1
        }
        return out
    }

    @ViewBuilder
    private func entry(_ e: Entry) -> some View {
        switch e {
        case .person(let t):
            Text(t)
                .font(.callout.weight(.medium))
                .padding(10)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(.quaternary.opacity(0.6),
                            in: RoundedRectangle(cornerRadius: 10))
                .textSelection(.enabled)
        case .words(let t):
            Text(agentMarkdown(t))
                .font(.callout)
                .textSelection(.enabled)
        case .note(let t):
            Text(t.trimmingCharacters(in: .whitespacesAndNewlines))
                .font(.caption)
                .foregroundStyle(.tertiary)
                .lineLimit(4)
        case .step(let s):
            AgentStepRow(step: s)
        case .speed(let st):
            AgentSpeedText(stats: st)
                .padding(.top, -6)
        }
    }
}

/// The generation card's rows for the Agent tab: whether each step is
/// asked for, where its model thinks, what it works on.
struct AgentSettingsRows: View {
    @Bindable var model: AppModel

    var body: some View {
        let s = model.agentSettings
        LabeledContent("Steps") {
            Picker("Steps", selection: Binding(
                get: { s?.review ?? true },
                set: { model.setAgentSettings(["review": $0]) })) {
                Text("Ask First").tag(true)
                Text("Run on Their Own").tag(false)
            }
            .labelsHidden()
            .pickerStyle(.segmented)
            .fixedSize()
        }
        LabeledContent("Thinks On") {
            Picker("Thinks On", selection: Binding(
                get: { s?.backend ?? "helper" },
                set: { model.setAgentSettings(["backend": $0]) })) {
                Text("Agentic Helper").tag("helper")
                Text("Cloud").tag("cloud")
            }
            .labelsHidden()
            .fixedSize()
        }
        LabeledContent("Works On") {
            Text(model.activeComposition?.name
                 ?? String(localized: "A new composition"))
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
    }
}

/// The model's words as it wrote them -- inline Markdown (bold, italic,
/// code) drawn, line breaks kept; anything it cannot read, as text.
func agentMarkdown(_ s: String) -> AttributedString {
    (try? AttributedString(
        markdown: s,
        options: .init(interpretedSyntax: .inlineOnlyPreservingWhitespace)))
        ?? AttributedString(s)
}
