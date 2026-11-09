import AppKit
import SwiftUI

/// The agent's TABS (DESIGN §9b): in the Prompt Editor's chat, several
/// conversations at once, down the card's left side as the prompts' tabs
/// hang there. Each has its VALTZ TOOLS, chosen before its first words:
/// Valtz's own -- it changes the project, holding it while it works, and
/// one tab at a time has them (the core refuses a second) -- Look and
/// Add -- the chat guide's few: it looks at the project and adds to its
/// assets -- or none, words alone. Pictures pasted or dropped go with the
/// words in any of them. The tab shown is live in `agent`, `agentWriting`
/// and the box; the others are kept here, their sessions read again as
/// they change.
struct AgentTab: Identifiable {
    let id = UUID()
    /// "full" | "chat" | "procedural" | "none" (AgentRequest::skills).
    var skills: String
    /// Kept while another tab is shown.
    var session: AgentSession?
    var writing = ""
    var words = AppModel.PromptWords()
    var attachments: [PromptAttachment] = []
}

extension AppModel {
    /// The tab shown.
    var shownAgentTab: AgentTab? {
        agentTabs.indices.contains(agentTabIndex)
            ? agentTabs[agentTabIndex] : nil
    }

    /// The shown tab's tools.
    var agentSkills: String {
        shownAgentTab?.skills
            ?? (activeModality == .procedural ? "procedural" : "full")
    }

    /// The tab with Valtz's tools, if any: one at a time.
    var agentEditorTab: Int? {
        agentTabs.firstIndex { $0.skills == "full" }
    }

    /// Each tab's session as it is now: the shown one's live.
    func agentTabSession(_ i: Int) -> AgentSession? {
        i == agentTabIndex ? agent : agentTabs[i].session
    }

    /// A tab's title: what it was first asked, else its tools' name.
    func agentTabTitle(_ i: Int) -> String {
        let asked = agentTabSession(i)?.prompt
            .trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        if !asked.isEmpty { return Self.tabTitle(asked) }
        return Self.agentSkillsName(agentTabs[i].skills)
    }

    /// Tools' names, as the tabs and the status row say them.
    static func agentSkillsName(_ skills: String) -> String {
        switch skills {
        case "chat": String(localized: "Look and Add")
        case "none": String(localized: "No Tools")
        case "procedural": String(localized: "Programs")
        default: String(localized: "Valtz Tools")
        }
    }

    static func agentSkillsSymbol(_ skills: String) -> String {
        switch skills {
        case "chat": "eye"
        case "none": "text.bubble"
        case "procedural": "curlybraces"
        default: "wand.and.stars"
        }
    }

    /// What each is, in a line: the menu's and the welcome's.
    static func agentSkillsHelp(_ skills: String) -> String {
        switch skills {
        case "chat":
            String(localized: "Looks at the project and adds to Assets -- pictures it makes, texts it keeps -- without changing what is there.")
        case "none":
            String(localized: "Words alone: no Valtz tools, so its context is the conversation’s. Pictures you attach are still shown to it.")
        case "procedural":
            String(localized: "Writes programs — JavaScript in a sandboxed page, with no network or files — and draws them as pictures or clips, kept in Assets with their code.")
        default:
            String(localized: "Works on the document with the editor’s tools. One tab at a time has them.")
        }
    }

    /// The shown tab's live state kept with it.
    func stashAgentTab() {
        guard agentTabs.indices.contains(agentTabIndex) else { return }
        agentTabs[agentTabIndex].session = agent
        agentTabs[agentTabIndex].writing = agentWriting
        agentTabs[agentTabIndex].words = PromptWords(
            prompt: prompt, marked: promptMarked, mentions: promptMentions,
            assetId: promptAssetId, name: promptName)
        agentTabs[agentTabIndex].attachments = promptAttachments
    }

    /// Another tab shown: the one left kept, this one's conversation,
    /// words and pictures put back.
    func selectAgentTab(_ i: Int) {
        guard agentTabs.indices.contains(i), i != agentTabIndex else { return }
        stashAgentTab()
        loadAgentTab(i)
    }

    /// Tab `i` shown: its conversation, words and pictures put back.
    private func loadAgentTab(_ i: Int) {
        agentTabIndex = i
        let t = agentTabs[i]
        agent = t.session
        agentWriting = t.writing
        promptAttachments = t.attachments
        setPromptWords(t.words)
        if agent != nil { reloadAgent() }
    }

    /// A new tab, shown: Valtz's tools when no tab has them, else Look
    /// and Add.
    func newAgentTab() {
        stashAgentTab()
        // Procedural's tabs all write programs.
        agentTabs.append(AgentTab(
            skills: activeModality == .procedural ? "procedural"
                    : agentEditorTab == nil ? "full" : "chat"))
        agentTabIndex = agentTabs.count - 1
        agent = nil
        agentWriting = ""
        promptAttachments = []
        setPromptWords(PromptWords())
    }

    /// Out of the chat, back to the prompt box: the Prompt Editor folded,
    /// the Agent target kept.
    func leaveAgentChat() {
        if promptImmersive { exitImmersivePrompt() }
    }

    /// A tab closed: its conversation stopped (its transcript stays in
    /// Assets). The last one closed leaves a fresh one -- and the chat:
    /// back to the prompt box, as its (x) goes.
    func closeAgentTab(_ i: Int) {
        guard agentTabs.indices.contains(i) else { return }
        let last = agentTabs.count == 1
        if let s = agentTabSession(i), !s.ended, let core {
            _ = core.agentControl(s.agent, "stop")
        }
        let shown = i == agentTabIndex
        agentTabs.remove(at: i)
        if agentTabs.isEmpty {
            agentTabs = [AgentTab(skills: activeModality == .procedural
                                              ? "procedural" : "full")]
        }
        if shown {
            // Its neighbour shown.
            loadAgentTab(min(i, agentTabs.count - 1))
        } else if i < agentTabIndex {
            agentTabIndex -= 1
        }
        if last { leaveAgentChat() }
    }

    /// The shown tab's tools, before its first words: Valtz's own only
    /// where no other tab has them.
    func setAgentSkills(_ skills: String) {
        guard agentTabs.indices.contains(agentTabIndex), agent == nil
        else { return }
        if skills == "full", let e = agentEditorTab, e != agentTabIndex {
            flash(String(localized: "Tab \(String(e + 1)) has Valtz’s tools: one agent at a time changes the project."),
                  level: .warning)
            return
        }
        withAnimation(Self.motion) {
            agentTabs[agentTabIndex].skills = skills
        }
    }

    /// A conversation in a tab not shown changed: its session read again
    /// -- the project read again too where it may have changed it.
    func reloadAgentTab(session id: String) {
        guard let core,
              let i = agentTabs.firstIndex(where: {
                  $0.session?.agent == id }),
              i != agentTabIndex else { return }
        let r = core.agentState(id)
        guard r.ok, let next = DTO.decode(AgentSession.self, r["session"])
        else { return }
        let was = agentTabs[i].session
        if next.state != "thinking" || was?.state != "thinking"
            || (next.writing ?? "").count > agentTabs[i].writing.count {
            agentTabs[i].writing = next.writing ?? ""
        }
        withAnimation(Self.motion) { agentTabs[i].session = next }
        if next.steps.count != was?.steps.count
            || next.steps.last?.state != was?.steps.last?.state {
            syncAfterHistory()
        }
    }

    /// A conversation's words as they come, into its tab.
    func agentTabWrote(session id: String, _ text: String) {
        if let i = agentTabs.firstIndex(where: { $0.session?.agent == id }),
           i != agentTabIndex {
            agentTabs[i].writing += text
        }
    }

    /// Every tab's conversation at work stopped: the project closing.
    func stopAgentTabs() {
        guard let core else { return }
        for i in agentTabs.indices {
            if let s = agentTabSession(i), !s.ended {
                _ = core.agentControl(s.agent, "stop")
            }
        }
        reloadAgent()
    }

    /// A conversation at work in any tab.
    var agentsAtWork: Bool {
        agentTabs.indices.contains { i in
            guard let a = agentTabSession(i), a.project == projectId
            else { return false }
            return !a.ended && !a.listening
        }
    }
}

/// The agent's tabs down the chat card's left side: a conversation each,
/// its tools' glyph after its number, a dot while it works; the round +
/// a new one. Laid out as the prompts' tabs are (PromptTabRail).
struct AgentTabRail: View {
    @Bindable var model: AppModel
    /// The card's height.
    let height: CGFloat

    private static let top: CGFloat = 16
    private static let gap: CGFloat = 8

    var body: some View {
        let n = model.agentTabs.count
        let titles = (0..<n).map { model.agentTabTitle($0) }
        let room = max(0, height - Self.top) * 0.8
        let (lengths, scrolls) = PromptTabRail.lengths(
            titles.enumerated().map { i, t in
                PromptTabRail.natural(t, number: i + 1, locked: true)
            },
            room: room - Self.gap * CGFloat(max(0, n - 1)))
        VStack(alignment: .leading, spacing: Self.gap) {
            ScrollView(.vertical, showsIndicators: false) {
                VStack(alignment: .leading, spacing: Self.gap) {
                    ForEach(Array(model.agentTabs.enumerated()),
                            id: \.element.id) { i, tab in
                        let s = model.agentTabSession(i)
                        PromptTabCard(
                            number: i + 1, title: titles[i],
                            length: lengths[i],
                            open: i == model.agentTabIndex,
                            // The last one too: closing it leaves the
                            // chat.
                            closable: true,
                            symbol: AppModel.agentSkillsSymbol(tab.skills),
                            busy: s?.busy == true,
                            words: .chat,
                            select: { act { model.selectAgentTab(i) } },
                            close: { act { model.closeAgentTab(i) } },
                            new: { act { model.newAgentTab() } },
                            rename: nil,
                            duplicate: nil,
                            named: { _ in },
                            tip: "\(titles[i]) — \(AppModel.agentSkillsName(tab.skills))")
                    }
                    if !scrolls { plus }
                }
                .padding(.vertical, PromptTabRail.pad)
            }
            .scrollDisabled(!scrolls)
            .frame(height: scrolls ? room + 2 * PromptTabRail.pad : nil)
            if scrolls { plus }
        }
        .padding(.top, Self.top - PromptTabRail.pad)
        .frame(width: PromptTabRail.pad + PromptTabRail.visible
               + PromptTabRail.tuck, alignment: .leading)
        .animation(AppModel.motion, value: model.agentTabs.map(\.id))
    }

    private func act(_ f: @escaping () -> Void) {
        withAnimation(AppModel.motion) { f() }
    }

    /// The round + after the tabs: a new conversation.
    private var plus: some View {
        Button {
            act { model.newAgentTab() }
        } label: {
            Image(systemName: "plus")
                .font(.system(size: 12, weight: .semibold))
                .foregroundStyle(.secondary)
                .frame(width: 26, height: 26)
                .background(Circle().fill(ComposerStack.trayFill))
                .shadow(color: .black.opacity(0.09), radius: 7, y: 3)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .help("New chat: it looks and adds, or talks without tools, beside the one that edits")
        .accessibilityLabel(Text("New Chat"))
        .padding(.leading, PromptTabRail.pad + (PromptTabRail.visible - 26) / 2)
    }
}
