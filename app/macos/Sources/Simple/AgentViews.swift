import SwiftUI

/// THE AGENT at work (DESIGN §9b), over the prompt: what it is doing, its
/// words as they come, the step waiting for the person's word (Run, Skip
/// -- a note told to it -- Run All), and Pause, Stop. The Prompt Editor
/// shows all of it, as a chat (AgentChat).
struct AgentCard: View {
    @Bindable var model: AppModel
    /// The session as the card was made with it: shown only while the
    /// card goes (the model's let go).
    let shown: AgentSession

    /// The session as the model has it now. A copy handed down is the
    /// parent's as it last drew, and the parent does not always draw
    /// again when the session changes: the header read "Stopped" off the
    /// model beside an older copy's Pause and Stop (2026-10-29).
    var session: AgentSession { model.agent ?? shown }

    /// The rows the agent's output has: what it says and the step it is
    /// on, however long, scroll within them.
    static let outputHeight: CGFloat = 96
    /// The review buttons' row.
    static let buttonsHeight: CGFloat = 24

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            AgentHeader(model: model, shown: session)
            AgentHoldNote(model: model)
            // What it is doing -- its words as they come, the step it is on
            // with what it was asked, an error, a question -- in rows of
            // their own, the newest in view; longer, they scroll. Sized by
            // what came, the card moved the stage at every word and step.
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        output
                        Color.clear.frame(height: 1).id("end")
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(height: Self.outputHeight)
                .onAppear { proxy.scrollTo("end", anchor: .bottom) }
                .onChange(of: scrollKey) {
                    proxy.scrollTo("end", anchor: .bottom)
                }
            }
            // Its buttons' row kept while it asks for a word on each step,
            // so the card is one height as steps come and go.
            if let step = session.proposed {
                AgentReviewButtons(model: model, step: step)
                    .frame(height: Self.buttonsHeight)
            } else if session.review && !session.listening {
                Color.clear.frame(height: Self.buttonsHeight)
            }
        }
        .padding(16)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 18,
                                                    style: .continuous))
    }
}

extension AgentCard {
    /// The card's output, whole.
    @ViewBuilder
    private var output: some View {
        let words = model.agentWriting.trimmingCharacters(
            in: .whitespacesAndNewlines)
        if !words.isEmpty {
            Text(agentMarkdown(words))
                .font(.callout)
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
        if let e = session.errorText, session.state == "failed" {
            Label(e, systemImage: "exclamationmark.triangle")
                .font(.caption)
                .foregroundStyle(.orange)
                .fixedSize(horizontal: false, vertical: true)
        }
        if let step = session.proposed ?? session.steps.last {
            AgentStepRow(step: step, compact: true, limited: false)
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
        // The last reply cut off at its limit, once it has rested.
        if let n = session.stats?.cutOff, !session.busy {
            AgentCutOffText(limit: n)
        }
        // The last turn's speed, for debugging.
        if model.agentShowsSpeed, let st = session.stats, !st.isEmpty {
            AgentSpeedText(stats: st)
        }
    }

    /// What changes the output: the newest is brought into view.
    private var scrollKey: String {
        "\(model.agentWriting.count)|\(session.steps.count)|"
            + "\(session.steps.last?.state ?? "")|\(session.state)|"
            + "\(session.proposed?.n ?? -1)"
    }
}

/// The session's state, where its model thinks, and its controls.
struct AgentHeader: View {
    @Bindable var model: AppModel
    let shown: AgentSession

    /// The model's, as its state label reads it (AgentCard.session).
    var session: AgentSession { model.agent ?? shown }

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
            if session.transcript != nil {
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
            ContextRing(context: model.agentContext,
                        compactions: session.compactions ?? 0)
        }
        .controlSize(.small)
    }
}

/// While it works the project is the agent's (AppModel.agentHolds):
/// said, and -- the person looking elsewhere -- Follow, the stage back on
/// what it works on (AppModel.followAgent).
struct AgentHoldNote: View {
    @Bindable var model: AppModel

    var body: some View {
        if model.document.agent {
            HStack(spacing: 8) {
                // A chat tab beside the agent at work: what holds it.
                if model.agentChat, model.agentSkills != "full" {
                    Label("The tab with Valtz’s tools is working: the project is read-only until it is done. This chat still looks and adds.",
                          systemImage: "lock")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                } else {
                    Label("Read-only while it works: look around freely; changes wait until it is done, or stopped.",
                          systemImage: "lock")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                }
                Spacer(minLength: 8)
                // Follow follows the session shown: an editor's. Always
                // there while it works, lit while the stage follows: a
                // click that stopped it -- on the button, or anywhere else
                // the stage was moved -- is undone by the same button.
                if model.agentSkills == "full" {
                    if model.agentFollows {
                        Button {
                            model.unfollowAgent()
                        } label: {
                            Label("Following", systemImage: "eye.fill")
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)
                        .help("The stage follows the agent: what it works on, and a clip it makes playing its preview as it is made. Click to stay where you are; click again to follow it")
                    } else {
                        Button {
                            model.followAgent()
                        } label: {
                            Label("Follow", systemImage: "eye")
                        }
                        .controlSize(.small)
                        .help("Show what the agent is working on, and follow it again")
                    }
                }
            }
            .transition(.opacity)
        }
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

/// Under a reply cut off at its limit (the core's stats "cut_off"): it
/// did not end there, and the person can ask for the rest.
struct AgentCutOffText: View {
    let limit: Int

    var body: some View {
        Label {
            if limit > 0 {
                Text("Cut off: the reply reached its limit of \(limit) tokens. Ask it to go on for the rest.")
            } else {
                Text("Cut off: the reply reached its limit. Ask it to go on for the rest.")
            }
        } icon: {
            Image(systemName: "scissors")
        }
        .font(.caption)
        .foregroundStyle(.orange)
        .fixedSize(horizontal: false, vertical: true)
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
