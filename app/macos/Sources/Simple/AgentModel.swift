import Foundation
import SwiftUI

/// THE AGENT in the app (DESIGN §9b): Start with the Agent tab over the
/// prompt asks the agent -- a new session, or the next words to the one
/// listening -- and the card over the prompt, or the Prompt Editor's
/// card, shows it working: its words as they come, each step, the
/// one waiting for the person's word.
extension AppModel {
    /// Start in the Agent tab: the request to a new session -- or, while
    /// one listens (done, failed, asking) or works, the person's next
    /// words to it.
    func startAgent() {
        guard let core, let projectId else { return }
        let words = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !words.isEmpty else { return }
        if let a = agent, a.project == projectId, !a.ended {
            let r = core.agentSay(a.agent, words, stage: agentStage)
            guard r.ok else { flash(r.message, level: .error); return }
            clearAgentPrompt()
            return
        }
        var req: [String: Any] = [
            "project": projectId,
            "prompt": words,
            "row": rowAssetIds,
            "review": agentSettings?.review ?? true,
        ]
        if let c = activeComposition { req["composition"] = c.id }
        req["stage"] = agentStage
        let r = core.agentStart(req)
        guard r.ok, let id = r["agent"] as? String else {
            flash(r.message, level: .error)
            return
        }
        withAnimation(Self.motion) {
            agentWriting = ""
            agent = AgentSession(agent: id, project: projectId,
                                 state: "starting",
                                 review: agentSettings?.review ?? true,
                                 prompt: words, steps: [])
        }
        clearAgentPrompt()
        reloadAgent()
    }

    /// Where the person is as they speak -- "this page", "the current
    /// frame", "the selected layer": the active composition, and when the
    /// stage shows it (no task watched, nothing viewed), its page or the
    /// player's frame, the selected layer and its selected objects. The
    /// core tells the model with the words; it cannot ask the app.
    var agentStage: [String: Any] {
        guard let c = activeComposition else { return [:] }
        var s: [String: Any] = ["composition": c.id]
        guard generationJob == nil, viewedAsset == nil,
              stageStack?.id == c.id else { return s }
        if pagedOnStage { s["page"] = stagePage + 1 }
        if clipOnStage { s["frame"] = videoFrame }
        s["layer"] = activeLayer
        if markupOpen, !markup.selectedIds.isEmpty {
            s["objects"] = Array(markup.selectedIds).sorted()
        }
        return s
    }

    /// The box emptied of the words just sent (its media stay: the row is
    /// the agent's to use) -- the text view too, by the revision: the
    /// model's words alone emptied left them in the view, under the empty
    /// box's grey hint.
    private func clearAgentPrompt() {
        // What it was asked: the agent's box comes back with it after
        // another target's (AppModel.promptHistory).
        agentWordsSent = PromptWords(prompt: prompt, marked: promptMarked,
                                     mentions: promptMentions,
                                     assetId: promptAssetId,
                                     name: promptName)
        promptMarked = ""
        promptMentions = []
        setPrompt("")
    }

    /// The session as the core says it is now.
    func reloadAgent() {
        guard let core, let id = agent?.agent else { return }
        let r = core.agentState(id)
        guard r.ok,
              let next = DTO.decode(AgentSession.self, r["session"])
        else { return }
        let was = agent
        // Its words: the core's for the turn, when a new turn began or the
        // turn ended -- else what the stream has added since.
        if next.state != "thinking" || was?.state != "thinking"
            || (next.writing ?? "").count > agentWriting.count {
            agentWriting = next.writing ?? ""
        }
        withAnimation(Self.motion) { agent = next }
        // The composition it works on is the one the stage shows.
        if let c = next.composition, !c.isEmpty,
           c != was?.composition || activeComposition?.id != c {
            stageAgentComposition(c)
        }
        // What it changed: the list and the stage follow, as after an
        // undo -- and the stage goes to the page the step worked on.
        if next.steps.count != was?.steps.count
            || next.steps.last?.state != was?.steps.last?.state {
            syncAfterHistory()
            if let step = next.steps.last,
               step.state == "done" || step.state == "waiting",
               let page = Self.agentStepPage(step),
               activeComposition?.isPaged == true,
               page - 1 != stagePage {
                goToPage(page - 1)
            }
            // On a timeline: the player where the step put something.
            if let step = next.steps.last, step.state == "done",
               let at = Self.agentStepStart(step),
               activeComposition?.isTimeline == true {
                seekVideo(to: Int((at * stageFrameRate.fps).rounded()))
            }
        }
    }

    /// Where on a timeline a step put something (its result's "start",
    /// seconds), if it did.
    static func agentStepStart(_ step: AgentStep) -> Double? {
        guard let text = step.resultText,
              let data = text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data)
                as? [String: Any] else { return nil }
        return (j["start"] as? Double) ?? (j["start"] as? Int).map(Double.init)
    }

    /// The page a step worked on (its "page" argument, from 1), if any.
    static func agentStepPage(_ step: AgentStep) -> Int? {
        guard let text = step.argsText,
              let data = text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data)
                as? [String: Any] else { return nil }
        if let n = j["page"] as? Int { return n }
        if let d = j["page"] as? Double { return Int(d) }
        return nil
    }

    /// The agent's composition put on the stage -- once the asset list
    /// knows it.
    private func stageAgentComposition(_ id: String, tries: Int = 0) {
        if activeComposition?.id == id { return }
        if let a = assets.first(where: { $0.id == id }) {
            setActive(a)
            return
        }
        guard tries < 10 else { return }
        reloadAssets()
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(200))
            self?.stageAgentComposition(id, tries: tries + 1)
        }
    }

    // ---- the person's word on a step

    func agentApprove(_ step: AgentStep) {
        agentReview(step, "approve")
    }

    func agentSkip(_ step: AgentStep) {
        agentReview(step, "skip", note: agentNote)
        agentNote = ""
    }

    func agentApproveAll(_ step: AgentStep) {
        agentReview(step, "approve_all")
    }

    private func agentReview(_ step: AgentStep, _ decision: String,
                             note: String = "") {
        guard let core, let id = agent?.agent else { return }
        let r = core.agentReview(id, step: step.n, decision: decision,
                                 note: note)
        if !r.ok { flash(r.message, level: .error) }
    }

    /// pause | resume | stop | review | auto
    func agentControl(_ action: String) {
        guard let core, let id = agent?.agent else { return }
        let r = core.agentControl(id, action)
        if !r.ok { flash(r.message, level: .error) }
        reloadAgent()
    }

    /// A finished session put away: the next request starts another.
    func agentDismiss() {
        if let a = agent, !a.ended { agentControl("stop") }
        withAnimation(Self.motion) {
            agent = nil
            agentWriting = ""
        }
    }

    // ---- Settings › Experimental › Agent

    func setAgentSettings(_ s: [String: Any]) {
        guard let core else { return }
        let r = core.setAgentSettings(s)
        if !r.ok { flash(r.message, level: .error) }
        agentSettings = DTO.decode(AgentSettings.self,
                                   core.agentSettingsJSON())
    }

    /// The key into the Keychain ("" removes it); never kept here.
    func setAgentKey(_ provider: String, _ key: String) {
        guard let core else { return }
        let r = core.setAgentKey(provider, key)
        if !r.ok { flash(r.message, level: .error) }
        agentSettings = DTO.decode(AgentSettings.self,
                                   core.agentSettingsJSON())
    }

    // ---- words

    /// What the session is doing, as the card says it.
    var agentStateLabel: String {
        guard let a = agent else { return "" }
        switch a.state {
        case "starting", "thinking": return String(localized: "Thinking…")
        case "review": return String(localized: "Waiting for your word")
        case "acting": return String(localized: "Working…")
        case "waiting": return String(localized: "Waiting for generations…")
        case "compacting":
            return String(localized: "Compacting the conversation…")
        case "paused": return String(localized: "Paused")
        case "asking": return String(localized: "It asks you")
        case "done": return String(localized: "Done")
        case "failed": return String(localized: "Stopped by an error")
        case "stopped": return String(localized: "Stopped")
        default: return a.state
        }
    }

    /// Where its model thinks, in words.
    var agentBackendLabel: String {
        guard let b = agent?.backend else {
            return agentSettings?.backend == "cloud"
                ? String(localized: "the cloud")
                : String(localized: "this Mac's helper")
        }
        let model = b.model ?? ""
        switch b.kind {
        case "cloud": return "\(model) · \(b.where ?? "")"
        case "fleet":
            return String(localized: "\(model) on \(b.where ?? "")")
        default: return String(localized: "\(model) on this Mac")
        }
    }

    /// A tool's name as a step is titled, in the UI's language.
    static func agentToolTitle(_ tool: String) -> String {
        switch tool {
        case "get_project": String(localized: "Look at the Project")
        case "get_composition": String(localized: "Look at a Composition")
        case "view_page": String(localized: "Look at a Page")
        case "view_asset": String(localized: "Look at an Asset")
        case "create_composition": String(localized: "Make a Composition")
        case "add_pages": String(localized: "Add Pages")
        case "remove_page": String(localized: "Remove Page")
        case "add_text": String(localized: "Add Text")
        case "add_shape": String(localized: "Add Shape")
        case "update_object": String(localized: "Change Text or Shape")
        case "move_objects": String(localized: "Move Texts and Shapes")
        case "remove_object": String(localized: "Remove Text or Shape")
        case "arrange_objects": String(localized: "Arrange Texts and Shapes")
        case "draw_strokes": String(localized: "Draw with the Brush")
        case "flow_text": String(localized: "Lay Out Text")
        case "add_page_progress": String(localized: "Add Page Progress")
        case "place_asset": String(localized: "Place Asset")
        case "set_layer_box": String(localized: "Move Layer")
        case "upscale_layer": String(localized: "Upscale Layer")
        case "set_layer_pages": String(localized: "Layer Pages")
        case "arrange_layer": String(localized: "Arrange Layer")
        case "remove_layer": String(localized: "Remove Layer")
        case "rename_layer": String(localized: "Rename Layer")
        case "set_layer_visible": String(localized: "Show or Hide Layer")
        case "adjust_layer": String(localized: "Adjust Layer")
        case "create_prompt": String(localized: "Save Prompt")
        case "enhance_prompt": String(localized: "Enhance Prompt")
        case "generate_image": String(localized: "Generate Picture")
        case "wait_for_generations":
            String(localized: "Wait for Generations")
        case "ask_user": String(localized: "Ask You")
        case "describe_clip": String(localized: "Watch a Clip")
        case "transcribe": String(localized: "Listen to a Sound")
        case "separate_sound": String(localized: "Separate a Song")
        case "view_frames": String(localized: "Look at Frames")
        case "add_clip": String(localized: "Add Clip")
        case "add_sound": String(localized: "Add Sound")
        case "set_layer_time": String(localized: "Trim Layer")
        case "set_layer_sound": String(localized: "Layer Volume")
        case "set_transition": String(localized: "Transition")
        case "set_timeline_length": String(localized: "Timeline Length")
        case "generate_music": String(localized: "Generate Music")
        case "generate_voiceover": String(localized: "Generate Voice-Over")
        case "generate_video": String(localized: "Generate a Shot")
        default: tool
        }
    }

    /// A step's glyph by its state.
    static func agentStepSymbol(_ state: String) -> String {
        switch state {
        case "proposed": "hand.raised"
        case "running": "arrow.triangle.2.circlepath"
        case "waiting": "hourglass"
        case "done": "checkmark.circle"
        case "failed": "exclamationmark.triangle"
        case "skipped": "arrow.uturn.right.circle"
        default: "circle"
        }
    }
}
