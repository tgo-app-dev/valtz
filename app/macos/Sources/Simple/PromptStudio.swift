import AppKit
import SwiftUI

/// The Prompt Editor's TABS (DESIGN §10c): the prompts open at once, down
/// the left side of the prompt card as the tray's cards hang under it --
/// each a card tucked behind the prompt card's edge, rounded on its outer
/// side, its title running up it. The open one is white, of a piece with
/// the card; the others grey. While they fit in 80% of the height each
/// shows its whole title (to a length); past that the longest titles give
/// way first, and when even short tabs do not fit, they scroll, the round
/// + below them in the last fifth. Otherwise + follows the last tab.
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
    private static let inset: CGFloat = 14
    private static let font = NSFont.systemFont(ofSize: 12)

    var body: some View {
        let n = model.promptTabs.count
        let titles = (0..<n).map { model.promptTabTitle($0) }
        let room = max(0, height - Self.top) * 0.8
        let (lengths, scrolls) = Self.lengths(
            titles.enumerated().map { Self.natural($1, number: $0 + 1) },
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
                                select: { act { model.selectPromptTab(i) } },
                                close: { model.requestClosePromptTab(i) },
                                new: { act { model.newPromptTab() } })
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
    private static func natural(_ title: String, number: Int) -> CGFloat {
        let w = { (s: String) in
            (s as NSString).size(withAttributes: [.font: font]).width
        }
        // A little over the measure: SwiftUI's line takes a few points
        // more than the font's advances.
        let len = w(String(number)) + 6 + w(title) + 2 * inset + 12
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
/// number turns into × on hover.
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
    let select: () -> Void
    let close: () -> Void
    let new: () -> Void
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
                .onTapGesture(perform: select)
                .onHover { hovering = $0 }
        }
        .animation(.smooth(duration: 0.15), value: hovering)
        .contextMenu {
            Button("New Prompt", action: new)
            if closable {
                Button("Close Prompt", action: close)
            }
        }
        .help(title)
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
            Text(verbatim: title)
                .font(.callout)
                .italic(lookOnly)
                .foregroundStyle(open ? .primary : .secondary)
                .lineLimit(1)
                .truncationMode(.tail)
        }
        .frame(width: run, alignment: .leading)
        .rotationEffect(.degrees(-90))
        .frame(width: 20, height: run)
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
        markupOpen = false
        showsTuning = false
        cropEditing = false
        if inspectorTab != .layers { inspectorTab = .assets }
        stage.fitRequest += 1
        promptImmersive = true
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
        if let a = t.assetId { req["prompt_asset"] = a }
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
    }

    /// A tab's prompt into the text view: its mentions where they were
    /// (PromptEditor's reload goes by the marked text).
    private func loadPromptTab(_ t: PromptTab) {
        promptMarked = t.marked
        promptMentions = t.mentions
        prompt = t.words
        promptAssetId = t.assetId
        enhanced = t.enhanced
        promptRevision += 1
        promptChanged()
    }

    /// A tab's name: its first words -- tags as their names ("Image 1"),
    /// Markdown's markers left out -- or "New Prompt".
    func promptTabTitle(_ i: Int) -> String {
        let t = Self.tabTitle(i == activePromptTab ? prompt
                              : promptTabs.indices.contains(i)
                                  ? promptTabs[i].words : "")
        return t.isEmpty ? String(localized: "New Prompt") : t
    }

    func promptTabTitleIsEmpty(_ i: Int) -> Bool {
        Self.tabTitle(i == activePromptTab ? prompt
                      : promptTabs.indices.contains(i)
                          ? promptTabs[i].words : "").isEmpty
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
