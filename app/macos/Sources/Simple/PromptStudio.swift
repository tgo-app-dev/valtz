import AppKit
import SwiftUI

/// The Prompt Editor's TABS (DESIGN §10c), the model's side: the prompts
/// open at once -- the open one the prompt itself, the others waiting in
/// `promptTabs` -- going in and out of the editor, choosing, adding,
/// duplicating, closing and naming them. Their bar is Dock/PromptTabBar.

// MARK: - The tabs

extension AppModel {
    /// Into the Prompt Editor: the prompt becomes a tab -- the first, or
    /// the one it was when last here (the others as they were left). The
    /// dock grows to the column, the same text view in it (the keyboard
    /// stays where it was); the stage goes small in the strip over it: no
    /// markup, no crop editing, fitted once it has its room; the
    /// inspector offers Layers and Information.
    func enterImmersivePrompt() {
        guard !refusedForColorTarget() else { return }
        if promptTabs.isEmpty {
            promptTabs = [PromptTab()]
            activePromptTab = 0
        }
        // A tab only looked at last time: the box gets a tab of its own.
        if promptTabs.indices.contains(activePromptTab),
           promptTabs[activePromptTab].source != nil {
            promptTabs.insert(PromptTab(), at: 0)
            activePromptTab = 0
        }
        stashPromptTab()
        boxTab = promptTabs[activePromptTab]
        editorEdited = false
        markupOpen = false
        showsTuning = false
        stage.fitRequest += 1
        promptImmersive = true
        timelineCutting = false
        refitStage()
    }

    /// Back to the stage: the prompt in the box is the tab last open (the
    /// others kept for next time).
    func exitImmersivePrompt() {
        stashPromptTab()
        // From a tab only looked at (do-not-apply): the box as it was --
        // in its own tab when that still holds it, else a tab again.
        if tabDoNotApply(activePromptTab), let box = boxTab {
            if let i = promptTabs.firstIndex(where: {
                $0.id == box.id && $0.marked == box.marked }) {
                activePromptTab = i
            } else {
                var t = PromptTab()
                t.marked = box.marked
                t.mentions = box.mentions
                t.assetId = box.assetId
                t.enhanced = box.enhanced
                t.name = box.name
                promptTabs.insert(t, at: 0)
                activePromptTab = 0
            }
            loadPromptTab(promptTabs[activePromptTab])
        }
        boxTab = nil
        // The dock back to the height it had (the chrome's, or its
        // content's); the stage fitted to its room again once it has it.
        stage.fitRequest += 1
        promptImmersive = false
        refitStage()
    }

    func toggleImmersivePrompt() {
        if promptImmersive {
            exitImmersivePrompt()
        } else {
            enterImmersivePrompt()
        }
    }

    func selectPromptTab(_ i: Int) {
        guard i != activePromptTab, promptTabs.indices.contains(i) else {
            return
        }
        stashPromptTab()
        activePromptTab = i
        loadPromptTab(promptTabs[i])
    }

    /// A LOCKED prompt (something was made from it) copied into a new tab
    /// after it, open and editable: its words and mentions -- the same
    /// words are still that prompt; changed, a new one.
    func duplicatePromptTab(_ i: Int) {
        guard promptTabs.indices.contains(i) else { return }
        stashPromptTab()
        let t = promptTabs[i]
        var copy = PromptTab()
        copy.marked = t.marked
        copy.mentions = t.mentions
        promptTabs.insert(copy, at: i + 1)
        activePromptTab = i + 1
        loadPromptTab(copy)
    }

    /// A new, empty prompt after the others, open.
    func newPromptTab() {
        stashPromptTab()
        promptTabs.append(PromptTab())
        activePromptTab = promptTabs.count - 1
        loadPromptTab(promptTabs[activePromptTab])
    }

    /// A tab's words as they are now (the open one's: the text view's).
    func promptTabWords(_ i: Int) -> String {
        guard promptTabs.indices.contains(i) else { return "" }
        return i == activePromptTab ? prompt : promptTabs[i].words
    }

    /// DO-NOT-APPLY: a text asset only looked at -- unedited, or read only
    /// (something was made from it). It never becomes the prompt box's:
    /// no way back to the box from it, and the editor going from it
    /// leaves the box as it was.
    func tabDoNotApply(_ i: Int) -> Bool {
        guard promptTabs.indices.contains(i),
              promptTabs[i].source != nil else { return false }
        return promptTabs[i].readOnly
            || promptTabWords(i) == promptTabs[i].sourceWords
    }

    var activeTabDoNotApply: Bool {
        promptImmersive && tabDoNotApply(activePromptTab)
    }

    var activeTabReadOnly: Bool {
        promptImmersive && promptTabs.indices.contains(activePromptTab)
            && promptTabs[activePromptTab].readOnly
    }

    /// A TEXT ASSET selected in Assets (DESIGN §10c): the Prompt Editor,
    /// the prompt box's words its first tab and the asset in the second
    /// -- to look at (do-not-apply until edited; read only when something
    /// was made from it). Already open: the tab showing it, else an
    /// untouched one replaced, else a new one after the open one.
    func openTextAsset(_ a: AssetDTO) {
        guard a.kind == "text", let text = a.text else { return }
        var tab = PromptTab()
        tab.marked = text
        tab.assetId = a.id
        tab.source = a.id
        tab.sourceWords = text
        tab.readOnly = (a.uses ?? 0) > 0
        if !promptImmersive {
            withAnimation(Self.motion) { enterImmersivePrompt() }
            // The box's words first.
            if activePromptTab != 0 {
                let box = promptTabs.remove(at: activePromptTab)
                promptTabs.insert(box, at: 0)
                activePromptTab = 0
            }
        }
        stashPromptTab()
        if let i = promptTabs.firstIndex(where: { $0.source == a.id }) {
            selectPromptTab(i)
            return
        }
        let at: Int
        if let i = promptTabs.indices.first(where: {
            promptTabs[$0].source != nil && tabDoNotApply($0)
                && $0 != 0 }) {
            promptTabs[i] = tab
            at = i
        } else {
            at = min(activePromptTab + 1, promptTabs.count)
            promptTabs.insert(tab, at: at)
        }
        activePromptTab = at
        loadPromptTab(promptTabs[at])
    }

    /// Another kind of asset selected in Assets: the editor goes (the box
    /// as it was, when the tab open is one only looked at).
    func retreatFromEditor() {
        guard promptImmersive else { return }
        withAnimation(Self.motion) { exitImmersivePrompt() }
    }

    /// A tab's (x). Its words not in Assets yet -- nothing made from them,
    /// not kept -- the person is asked whether to keep them there first.
    /// Closing never changes the prompt box.
    func requestClosePromptTab(_ i: Int) {
        guard promptTabs.indices.contains(i) else { return }
        let words = promptTabWords(i).trimmingCharacters(
            in: .whitespacesAndNewlines)
        let kept = words.isEmpty || tabDoNotApply(i)
            || assets.contains {
                $0.isPrompt && ($0.text ?? "").trimmingCharacters(
                    in: .whitespacesAndNewlines) == words
            }
        if !kept {
            switch Self.askKeepPrompt() {
            case .keep:
                keepPromptTab(i)
            case .drop:
                break
            case .cancel:
                return
            }
        }
        withAnimation(Self.motion) { closePromptTab(i) }
    }

    enum KeepAnswer { case keep, drop, cancel }

    /// "Keep this prompt in Assets?" -- a scripted run answers by
    /// VALTZ_SNAPSHOT_CLOSE_ANSWER (keep | drop; drop by default).
    static func askKeepPrompt() -> KeepAnswer {
        let env = ProcessInfo.processInfo.environment
        if env["VALTZ_SNAPSHOT"] != nil {
            return env["VALTZ_SNAPSHOT_CLOSE_ANSWER"] == "keep" ? .keep
                                                               : .drop
        }
        let alert = NSAlert()
        alert.messageText = String(localized: "Keep this prompt in Assets?")
        alert.informativeText = String(localized: "Nothing has been made from it yet, so it is not in Assets: closing its tab would lose it.")
        alert.addButton(withTitle: String(localized: "Keep in Assets"))
        alert.addButton(withTitle: String(localized: "Don't Keep"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        switch alert.runModal() {
        case .alertFirstButtonReturn: return .keep
        case .alertSecondButtonReturn: return .drop
        default: return .cancel
        }
    }

    /// A tab's prompt captured as an asset (core capture_prompt): its
    /// mentions named where their media sit in the row.
    private func keepPromptTab(_ i: Int) {
        guard let core, let projectId else { return }
        let live = i == activePromptTab
        let t = promptTabs[i]
        let mentions = live ? promptMentions : t.mentions
        let text = mentions.isEmpty ? promptTabWords(i)
                                    : (live ? promptMarked : t.marked)
        let inline = mentions.map { id in
            promptAttachments.first { $0.id == id }
                .flatMap { assetId(of: $0) } ?? ""
        }
        var req: [String: Any] = ["project": projectId, "prompt": text,
                                  "row": rowAssetIds, "inline": inline]
        if let a = live ? promptAssetId : t.assetId { req["prompt_asset"] = a }
        if let n = promptTabName(i) { req["prompt_name"] = n }
        let r = core.capturePrompt(req)
        if !r.ok { flash(r.message, level: .error) }
        reloadAssets()
    }

    /// A prompt closed; the one open closed, its neighbour opens. The last
    /// one is emptied instead.
    func closePromptTab(_ i: Int) {
        guard promptTabs.indices.contains(i) else { return }
        if promptTabs.count == 1 {
            promptTabs = [PromptTab()]
            activePromptTab = 0
            loadPromptTab(promptTabs[0])
            return
        }
        stashPromptTab()
        promptTabs.remove(at: i)
        if i == activePromptTab {
            activePromptTab = min(i, promptTabs.count - 1)
            loadPromptTab(promptTabs[activePromptTab])
        } else if i < activePromptTab {
            activePromptTab -= 1
        }
    }

    /// The prompt as it is now, into its tab.
    func stashPromptTab() {
        guard promptTabs.indices.contains(activePromptTab) else { return }
        // The marked text agrees with the words (the text view synced),
        // or the words were just replaced and are the truth.
        let synced = promptMarked.replacingOccurrences(of: "\u{FFFC}",
                                                       with: "") == prompt
        promptTabs[activePromptTab].marked = synced ? promptMarked : prompt
        promptTabs[activePromptTab].mentions = synced ? promptMentions : []
        promptTabs[activePromptTab].assetId = promptAssetId
        promptTabs[activePromptTab].enhanced = enhanced
        promptTabs[activePromptTab].name = promptName
    }

    /// A tab's prompt into the text view: its mentions where they were
    /// (PromptEditor's reload goes by the marked text).
    private func loadPromptTab(_ t: PromptTab) {
        promptMarked = t.marked
        promptMentions = t.mentions
        prompt = t.words
        promptAssetId = t.assetId
        promptName = t.name
        enhanced = t.enhanced
        promptRevision += 1
        promptChanged()
    }

    /// A tab's title: the name the person gave it; a text asset opened
    /// from Assets, its name there; else its first words
    /// -- tags as their names ("Image 1"), Markdown's markers left out --
    /// or "New Prompt".
    func promptTabTitle(_ i: Int) -> String {
        if let n = promptTabName(i) { return n }
        // A text asset opened from Assets: called as Assets calls it.
        if promptTabs.indices.contains(i), let src = promptTabs[i].source,
           let a = asset(id: src) {
            return a.name
        }
        let t = Self.tabTitle(i == activePromptTab ? prompt
                              : promptTabs.indices.contains(i)
                                  ? promptTabs[i].words : "")
        return t.isEmpty ? String(localized: "New Prompt") : t
    }

    func promptTabTitleIsEmpty(_ i: Int) -> Bool {
        promptTabName(i) == nil
            && Self.tabTitle(i == activePromptTab ? prompt
                             : promptTabs.indices.contains(i)
                                 ? promptTabs[i].words : "").isEmpty
    }

    // MARK: A prompt's name (DESIGN §10c)

    /// The name the person gave a tab's prompt. Once its prompt is in
    /// Assets the asset's is the one -- renamed there, undone, it follows
    /// (a prompt its words name: none); before, the tab keeps it, and the
    /// prompt captured from it (Start, Keep in Assets) is named so.
    func promptTabName(_ i: Int) -> String? {
        guard promptTabs.indices.contains(i) else { return nil }
        let live = i == activePromptTab
        return givenName(asset: live ? promptAssetId : promptTabs[i].assetId,
                         kept: live ? promptName : promptTabs[i].name)
    }

    /// The box's prompt's (the open tab's), sent with Start.
    var promptGivenName: String? {
        givenName(asset: promptAssetId, kept: promptName)
    }

    private func givenName(asset: String?, kept: String?) -> String? {
        if let asset, let a = self.asset(id: asset),
           a.isPrompt {
            return a.isNamed ? a.name : nil
        }
        return kept.flatMap { $0.isEmpty ? nil : $0 }
    }

    /// A tab named, as typed in its header: its prompt in Assets renamed
    /// with it (one undoable step); not there yet, the tab keeps the name
    /// for it. Emptied, the prompt is named by its words again.
    func renamePromptTab(_ i: Int, _ typed: String) {
        guard promptTabs.indices.contains(i) else { return }
        let name = typed.split(whereSeparator: \.isNewline)
            .joined(separator: " ")
            .trimmingCharacters(in: .whitespaces)
        let given = name.isEmpty ? nil : name
        let live = i == activePromptTab
        if live { promptName = given }
        promptTabs[i].name = given
        guard let id = live ? promptAssetId : promptTabs[i].assetId,
              let a = asset(id: id),
              let core, let projectId else { return }
        // Any other text (a transcript looked at) is named only when a
        // name is given.
        guard a.isPrompt || given != nil else { return }
        let r = core.assetOp(project: projectId, "rename",
                             ["asset": id, "name": name])
        if !r.ok { flash(r.message, level: .error) }
        reloadAssets()
    }

    nonisolated static func tabTitle(_ words: String) -> String {
        var t = words.replacing(
            /<valtz_ref_(img|vid|aud)_(\d+)>/
        ) { m in
            let kind = switch m.output.1 {
            case "vid": String(localized: "Video")
            case "aud": String(localized: "Audio")
            default: String(localized: "Image")
            }
            return "\(kind) \((Int(m.output.2) ?? 0) + 1)"
        }
        for marker in ["***", "**", "__", "~~", "`", "<u>", "</u>", "*"] {
            t = t.replacingOccurrences(of: marker, with: "")
        }
        let line = t.split(whereSeparator: \.isNewline)
            .map { $0.trimmingCharacters(in: .whitespaces) }
            .first { !$0.isEmpty } ?? ""
        let words = line.drop { $0 == "#" }
            .trimmingCharacters(in: .whitespaces)
        return String(words.prefix(90))
    }
}
