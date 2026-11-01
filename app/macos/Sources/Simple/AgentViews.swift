import SwiftUI

/// The session's state, where its model thinks, and its controls -- on
/// one row, or, short of room (the right column), the controls under the
/// state.
struct AgentHeader: View {
    @Bindable var model: AppModel
    let shown: AgentSession

    /// The session as the model has it now. A copy handed down is the
    /// parent's as it last drew, and the parent does not always draw
    /// again when the session changes: the header read "Stopped" off the
    /// model beside an older copy's Pause and Stop (2026-10-29).
    var session: AgentSession { model.agent ?? shown }

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 8) {
                state
                Spacer(minLength: 8)
                controls
            }
            VStack(alignment: .leading, spacing: 6) {
                HStack(spacing: 8) { state }
                HStack(spacing: 8) {
                    Spacer(minLength: 0)
                    controls
                }
            }
        }
        .controlSize(.small)
    }

    @ViewBuilder
    private var state: some View {
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
    }

    @ViewBuilder
    private var controls: some View {
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
}

/// While it works the project is the agent's (AppModel.agentHolds):
/// said, and -- the person looking elsewhere -- Follow, the stage back on
/// what it works on (AppModel.followAgent).
struct AgentHoldNote: View {
    @Bindable var model: AppModel

    var body: some View {
        if model.agentHolds {
            HStack(spacing: 8) {
                Label("Read-only while it works: look around freely; changes wait until it is done, or stopped.",
                      systemImage: "lock")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                Spacer(minLength: 8)
                if !model.agentFollows {
                    Button {
                        model.followAgent()
                    } label: {
                        Label("Follow", systemImage: "eye")
                    }
                    .controlSize(.small)
                    .help("Show what the agent is working on, and follow it again")
                }
            }
            .transition(.opacity)
        }
    }
}

/// Run, Skip (with a note), Run All -- for the step waiting: on one row,
/// or, short of room, the note over the buttons.
struct AgentReviewButtons: View {
    @Bindable var model: AppModel
    let step: AgentStep

    var body: some View {
        ViewThatFits(in: .horizontal) {
            HStack(spacing: 8) {
                note.frame(minWidth: 140, maxWidth: 220)
                Spacer(minLength: 8)
                buttons
            }
            VStack(alignment: .trailing, spacing: 6) {
                note
                HStack(spacing: 8) { buttons }
            }
        }
        .controlSize(.small)
    }

    private var note: some View {
        TextField("Note for Skip (optional)", text: $model.agentNote)
            .textFieldStyle(.roundedBorder)
    }

    @ViewBuilder
    private var buttons: some View {
        Button("Skip") { model.agentSkip(step) }
            .help("Not this one: it is told, with your note")
        Button("Run All") { model.agentApproveAll(step) }
            .help("This one and every step after it, without asking")
        Button("Run") { model.agentApprove(step) }
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.defaultAction)
    }
}

/// One step: its glyph by state, its title, what it was asked to do, how
/// it went.
struct AgentStepRow: View {
    let step: AgentStep
    var compact = false
    /// Its texts cut to a few lines; else whole (the card's scrolling
    /// rows).
    var limited = true

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
                        .lineLimit(limited ? (compact ? 2 : 6) : nil)
                        .textSelection(.enabled)
                }
                if let e = step.error, !e.isEmpty {
                    Text(e)
                        .font(.caption)
                        .foregroundStyle(.orange)
                        .lineLimit(limited ? (compact ? 2 : 4) : nil)
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

/// The agent at work (DESIGN §9b) -- the right column's Agent panel, and
/// the Prompt Editor's card while the agent works (DESIGN §10c), its
/// whole width, the box a few lines under it: its state and controls,
/// the hold note, then the request and each turn -- its words, its
/// steps, its speed while that is shown -- as they came, the step
/// waiting with its buttons, the words being written last.
struct AgentPane: View {
    @Bindable var model: AppModel
    let shown: AgentSession
    /// Its rows' leading inset: the prompt's text's in the Prompt
    /// Editor, the column's margin in the right column.
    var inset: CGFloat = PromptEditor.inset.width + 5

    /// The model's (AgentHeader.session).
    var session: AgentSession { model.agent ?? shown }

    var body: some View {
        VStack(spacing: 0) {
            AgentHeader(model: model, shown: session)
                .padding(.leading, inset)
                .padding(.trailing, 12)
                .padding(.vertical, VZ.Space.xs)
                .frame(minHeight: PaneHeader.height)
            AgentHoldNote(model: model)
                .padding(.leading, inset)
                .padding(.trailing, 12)
                .padding(.bottom, 6)
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
                    .padding(.leading, inset)
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
            case "compaction":
                // Where the conversation was compacted to go on within
                // its model's context: what is above, summarized.
                let n = m.compacted ?? 0
                out.append(.note(String(localized: "Compacted to stay within the model’s context: the \(n) messages before summarized.")))
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
