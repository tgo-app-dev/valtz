import AppKit
import SwiftUI

/// The Prompt Editor's TABS (DESIGN §10c): the prompts open at once, down
/// the left side of the prompt card as the tray's cards hang under it --
/// each a card tucked behind the prompt card's edge, rounded on its outer
/// side, its title running up it. The open one is white, of a piece with
/// the card; the others grey. While they fit in 80% of the height each
/// shows its whole title (to a length); past that the longest titles give
/// way first, and when even short tabs do not fit, they scroll, the round
/// + below them in the last fifth. Otherwise + follows the last tab. A
/// tab's title is typed over in place to NAME its prompt: a double-click
/// on it, or its menu's Rename.
struct PromptTabRail: View {
    @Bindable var model: AppModel
    /// The prompt card's height.
    let height: CGFloat

    /// How far a tab stands out of the card, and how far it tucks under.
    static let visible: CGFloat = 32
    static let tuck: CGFloat = 22
    /// Room for the tabs' shadows inside the scroll view.
    static let pad: CGFloat = 10
    /// How far the rail reaches left of the card.
    static let outset: CGFloat = visible + pad
    private static let top: CGFloat = 16
    private static let gap: CGFloat = 8
    private static let radius: CGFloat = 14
    private static let minLength: CGFloat = 64
    private static let maxLength: CGFloat = 240
    /// A tab whose name is being typed: room to type in.
    private static let editLength: CGFloat = 200
    private static let inset: CGFloat = 14
    private static let font = NSFont.systemFont(ofSize: 12)

    var body: some View {
        let n = model.promptTabs.count
        let titles = (0..<n).map { model.promptTabTitle($0) }
        let room = max(0, height - Self.top) * 0.8
        let editing = model.promptTabs.firstIndex {
            $0.id == model.renamingPromptTab }
        let (lengths, scrolls) = Self.lengths(
            titles.enumerated().map { i, t in
                let l = Self.natural(t, number: i + 1,
                                     locked: model.promptTabs[i].readOnly)
                return i == editing ? max(l, Self.editLength) : l
            },
            room: room - Self.gap * CGFloat(max(0, n - 1)))
        VStack(alignment: .leading, spacing: Self.gap) {
            ScrollViewReader { reader in
                ScrollView(.vertical, showsIndicators: false) {
                    VStack(alignment: .leading, spacing: Self.gap) {
                        ForEach(Array(model.promptTabs.enumerated()),
                                id: \.element.id) { i, tab in
                            PromptTabCard(
                                number: i + 1, title: titles[i],
                                length: lengths[i],
                                open: i == model.activePromptTab,
                                closable: n > 1
                                    || !model.promptTabTitleIsEmpty(i),
                                lookOnly: model.tabDoNotApply(i),
                                locked: tab.readOnly,
                                editing: i == editing,
                                select: { act { model.selectPromptTab(i) } },
                                close: { model.requestClosePromptTab(i) },
                                new: { act { model.newPromptTab() } },
                                rename: {
                                    act { model.selectPromptTab(i) }
                                    model.renamingPromptTab = tab.id
                                },
                                duplicate: tab.readOnly ? {
                                    act { model.duplicatePromptTab(i) }
                                } : nil,
                                named: { typed in
                                    model.renamingPromptTab = nil
                                    if let typed, typed != titles[i] {
                                        act { model.renamePromptTab(i, typed) }
                                    }
                                })
                                .id(tab.id)
                        }
                        if !scrolls { plus }
                    }
                    .padding(.vertical, Self.pad)
                }
                .scrollDisabled(!scrolls)
                .frame(height: scrolls ? room + 2 * Self.pad : nil)
                .onChange(of: model.activePromptTab) { _, i in
                    guard model.promptTabs.indices.contains(i) else { return }
                    withAnimation(AppModel.motion) {
                        reader.scrollTo(model.promptTabs[i].id)
                    }
                }
            }
            if scrolls { plus }
        }
        .padding(.top, Self.top - Self.pad)
        .frame(width: Self.pad + Self.visible + Self.tuck, alignment: .leading)
        .animation(AppModel.motion, value: model.promptTabs.map(\.id))
    }

    private func act(_ f: @escaping () -> Void) {
        withAnimation(AppModel.motion) { f() }
    }

    /// The round + after the tabs: a new prompt.
    private var plus: some View {
        Button {
            act { model.newPromptTab() }
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
        .help("New prompt")
        .accessibilityLabel(Text("New Prompt"))
        .padding(.leading, Self.pad + (Self.visible - 26) / 2)
    }

    /// A tab's length for its whole title: its number, the title, its
    /// ends -- within the bounds a tab has.
    private static func natural(_ title: String, number: Int,
                                locked: Bool = false) -> CGFloat {
        let w = { (s: String) in
            (s as NSString).size(withAttributes: [.font: font]).width
        }
        // A little over the measure: SwiftUI's line takes a few points
        // more than the font's advances.
        // A locked one's lock beside its number.
        let len = w(String(number)) + 6 + w(title) + 2 * inset + 12
            + (locked ? 14 : 0)
        return min(maxLength, max(minLength, ceil(len)))
    }

    /// The tabs' lengths in `room`: their own while they fit; else the
    /// longest cut to one length -- water-filling -- so the short ones
    /// keep theirs; and when that length would be shorter than a tab can
    /// be, each at its shortest, scrolling.
    static func lengths(_ natural: [CGFloat], room: CGFloat)
        -> ([CGFloat], Bool) {
        guard natural.reduce(0, +) > room, !natural.isEmpty else {
            return (natural, false)
        }
        var left = room
        var cap = room
        for (i, l) in natural.sorted().enumerated() {
            let share = left / CGFloat(natural.count - i)
            if l <= share {
                left -= l
            } else {
                cap = share
                break
            }
        }
        if cap < minLength {
            return (natural.map { min($0, minLength) }, true)
        }
        return (natural.map { min($0, cap) }, false)
    }
}

/// One tab: a card on its side, its number and title running up it; the
/// number turns into × on hover. Renamed, the title is a field.
private struct PromptTabCard: View {
    let number: Int
    let title: String
    let length: CGFloat
    let open: Bool
    let closable: Bool
    /// A text asset only looked at (do-not-apply): its title in italics;
    /// read only, a lock before it.
    var lookOnly = false
    var locked = false
    var editing = false
    let select: () -> Void
    let close: () -> Void
    let new: () -> Void
    /// Its title made a field; and what was typed there (nil: Escape).
    let rename: () -> Void
    /// A locked prompt's copy to edit (none for another).
    let duplicate: (() -> Void)?
    let named: (String?) -> Void
    @State private var hovering = false

    private var run: CGFloat { max(0, length - 28) }

    var body: some View {
        let shape = UnevenRoundedRectangle(topLeadingRadius: 14,
                                           bottomLeadingRadius: 14,
                                           bottomTrailingRadius: 0,
                                           topTrailingRadius: 0,
                                           style: .continuous)
        HStack(spacing: 0) {
            Color.clear.frame(width: PromptTabRail.pad)
            label
                .frame(width: PromptTabRail.visible, height: length)
                .padding(.trailing, PromptTabRail.tuck)
                .background(shape.fill(open
                    ? Color(nsColor: .textBackgroundColor)
                    : ComposerStack.trayFill))
                .shadow(color: .black.opacity(open ? 0.13 : 0.09),
                        radius: 7, y: 3)
                .contentShape(shape)
                // A double-click on it names it (a click alone selects at
                // once: no wait for a second).
                .onClick({ if !editing { select() } },
                         double: { if !editing { rename() } })
                .onHover { hovering = $0 }
        }
        .animation(.smooth(duration: 0.15), value: hovering)
        .contextMenu {
            Button("New Prompt", action: new)
            Button("Rename Prompt", action: rename)
            if let duplicate {
                Button("Duplicate and Edit", action: duplicate)
            }
            if closable {
                Button("Close Prompt", action: close)
            }
        }
        .help(editing ? "" : String(localized: "\(title) -- double-click to name it"))
        .accessibilityElement(children: .ignore)
        .accessibilityLabel(Text(verbatim: "\(number) \(title)"))
        .accessibilityAddTraits(open ? [.isSelected, .isButton] : .isButton)
        .accessibilityAction(named: Text("Close Prompt"), close)
    }

    /// Laid out along the tab, then stood up: it reads from the bottom.
    private var label: some View {
        HStack(spacing: 6) {
            if hovering && closable {
                Button(action: close) {
                    Image(systemName: "xmark")
                        .font(.system(size: 8, weight: .bold))
                        .frame(width: 12, height: 12)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .help("Close this prompt")
            } else {
                Text(verbatim: String(number))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(open ? Color.accentColor : .secondary)
            }
            if locked {
                Image(systemName: "lock.fill")
                    .font(.system(size: 8))
                    .foregroundStyle(.secondary)
            }
            if editing {
                TabNameField(initial: title, done: named)
            } else {
                Text(verbatim: title)
                    .font(.callout)
                    .italic(lookOnly)
                    .foregroundStyle(open ? .primary : .secondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
        }
        .frame(width: run, alignment: .leading)
        .rotationEffect(.degrees(-90))
        .frame(width: 20, height: run)
    }
}

/// A tab's title as a field: its name typed over it -- Return (or a
/// click away) names the prompt, Escape leaves it as it was; emptied, the
/// prompt is named by its words again.
private struct TabNameField: View {
    let initial: String
    let done: (String?) -> Void
    @State private var text = ""
    @State private var ended = false
    @FocusState private var focused: Bool

    var body: some View {
        TextField("Prompt name", text: $text)
            .textFieldStyle(.plain)
            .font(.callout)
            .focused($focused)
            // Read as a field: a light box, the accent's edge.
            .padding(.horizontal, 4)
            .background(RoundedRectangle(cornerRadius: 4)
                .fill(Color(nsColor: .textBackgroundColor)))
            .overlay(RoundedRectangle(cornerRadius: 4)
                .strokeBorder(Color.accentColor.opacity(0.7)))
            .onSubmit { end(text) }
            .onExitCommand { end(nil) }
            .onChange(of: focused) { _, now in
                if !now { end(text) }
            }
            .onAppear {
                text = initial
                // Focused once it is in the window: it takes the keyboard,
                // its words selected.
                DispatchQueue.main.async { focused = true }
            }
            .accessibilityLabel(Text("Prompt name"))
    }

    private func end(_ typed: String?) {
        guard !ended else { return }
        ended = true
        done(typed)
    }
}

// MARK: - The tabs

extension AppModel {
    /// Into the Prompt Editor: the prompt becomes a tab -- the first, or
    /// the one it was when last here (the others as they were left). The
    /// stage goes small: no markup, no crop editing, fitted; the
    /// inspector offers Layers and Assets (Assets, if it showed another).
    func enterImmersivePrompt() {
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
        if inspectorTab != .layers { inspectorTab = .assets }
        stage.fitRequest += 1
        promptImmersive = true
        // Back from it, the box: the prompt, not the timeline it may have
        // left from.
        timelineWanted = false
        timelineCutting = false
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
        // Its settings popover goes with it, Tune's options too: the
        // card's own would open otherwise.
        showsGenerationSettings = false
        showsTuning = false
        stage.fitRequest += 1
        promptImmersive = false
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
        if !r.ok { flash(r.message) }
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
           let a = assets.first(where: { $0.id == src }) {
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
        if let asset, let a = assets.first(where: { $0.id == asset }),
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
              let a = assets.first(where: { $0.id == id }),
              let core, let projectId else { return }
        // Any other text (a transcript looked at) is named only when a
        // name is given.
        guard a.isPrompt || given != nil else { return }
        let r = core.assetOp(project: projectId, "rename",
                             ["asset": id, "name": name])
        if !r.ok { flash(r.message) }
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
