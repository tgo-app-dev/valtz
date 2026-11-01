import AppKit
import SwiftUI

/// The Prompt Editor's TABS (DESIGN §10c), along the top of the grown
/// composer: the prompts open at once, left to right, the open one
/// pressed in; + after them, a new prompt, always in reach. A tab is chosen by a click; a
/// double-click (or its menu's Rename) types its NAME over its title; its
/// × -- on the open one, and on any the pointer is over -- closes it, its
/// menu duplicates a locked one. Past the bar's width they scroll, the
/// open one kept in sight. At the bar's end, Markdown's bold, italic and
/// underline on the selection (⌘B, ⌘I, ⌘U in the text) and whether
/// Markdown is drawn as it reads.
///
/// The model's side -- choosing, adding, closing, naming -- is
/// PromptStudio's.
struct PromptTabBar: View {
    @Bindable var model: AppModel
    /// The tabs' own width, scrolled or not.
    @State private var tabsWidth: CGFloat = 0

    static let height: CGFloat = 36

    var body: some View {
        let n = model.promptTabs.count
        let editing = model.promptTabs.firstIndex {
            $0.id == model.renamingPromptTab }
        HStack(spacing: VZ.Space.s) {
            ScrollViewReader { reader in
                ScrollView(.horizontal, showsIndicators: false) {
                    HStack(spacing: VZ.Space.xxs) {
                        ForEach(Array(model.promptTabs.enumerated()),
                                id: \.element.id) { i, tab in
                            let title = model.promptTabTitle(i)
                            PromptTabItem(
                                title: title,
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
                                    if let typed, typed != title {
                                        act { model.renamePromptTab(i, typed) }
                                    }
                                    // The keyboard back to the prompt --
                                    // unless the click that ended the
                                    // name gave it to something else.
                                    DispatchQueue.main.async {
                                        if let tv = model.promptTextView,
                                           let w = tv.window,
                                           w.firstResponder == nil
                                            || w.firstResponder === w {
                                            w.makeFirstResponder(tv)
                                        }
                                    }
                                })
                                .id(tab.id)
                        }
                    }
                    .padding(.horizontal, VZ.Space.s)
                    .onGeometryChange(for: CGFloat.self) {
                        $0.size.width
                    } action: { tabsWidth = $0 }
                }
                // As wide as the tabs, while they fit: + right after them.
                .frame(maxWidth: tabsWidth > 0 ? tabsWidth : nil)
                .onChange(of: model.activePromptTab) { _, i in
                    guard model.promptTabs.indices.contains(i) else { return }
                    withAnimation(AppModel.motion) {
                        reader.scrollTo(model.promptTabs[i].id)
                    }
                }
            }
            // Out of the scroll: in reach however many tabs there are.
            plus
            Spacer(minLength: 0)
            HStack(spacing: VZ.Space.xxs) {
                format(.bold, symbol: "bold",
                       help: String(localized: "Bold (⌘B)"))
                    .shortcutHint("⌘B")
                format(.italic, symbol: "italic",
                       help: String(localized: "Italic (⌘I)"))
                    .shortcutHint("⌘I")
                format(.underline, symbol: "underline",
                       help: String(localized: "Underline (⌘U)"))
                    .shortcutHint("⌘U")
            }
            MarkdownToggle(model: model)
                .padding(.trailing, VZ.Space.s)
        }
        .controlSize(.small)
        .frame(height: Self.height)
        .overlay(alignment: .bottom) {
            Rectangle()
                .fill(Color(nsColor: .separatorColor))
                .frame(height: 0.5)
        }
        .animation(AppModel.motion, value: model.promptTabs.map(\.id))
        .accessibilityElement(children: .contain)
        .accessibilityLabel(Text("Prompts"))
    }

    private func act(_ f: @escaping () -> Void) {
        withAnimation(AppModel.motion) { f() }
    }

    /// + after the tabs: a new prompt.
    private var plus: some View {
        Button {
            act { model.newPromptTab() }
        } label: {
            Image(systemName: "plus")
                .frame(width: 24, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .help("New prompt")
        .accessibilityLabel(Text("New Prompt"))
    }

    /// One style's button: on the text view showing the prompt, which
    /// keeps the keyboard.
    private func format(_ style: MarkdownStyle, symbol: String,
                        help: String) -> some View {
        Button {
            guard let tv = model.promptTextView else { return }
            tv.window?.makeFirstResponder(tv)
            tv.toggleMarkdown(style)
        } label: {
            Image(systemName: symbol)
                .frame(width: 24, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .help(help)
        .accessibilityLabel(Text(verbatim: help))
    }
}

/// One tab: its title (a lock before a locked one's, a text only looked
/// at in italics), × after it on the open one and under the pointer.
/// Renamed, the title is a field.
private struct PromptTabItem: View {
    let title: String
    let open: Bool
    let closable: Bool
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

    static let minWidth: CGFloat = 72
    static let maxWidth: CGFloat = 200

    var body: some View {
        let shape = RoundedRectangle(cornerRadius: VZ.Radius.control,
                                     style: .continuous)
        HStack(spacing: VZ.Space.xs) {
            if locked {
                Image(systemName: "lock.fill")
                    .imageScale(.small)
                    .foregroundStyle(.secondary)
            }
            if editing {
                TabNameField(initial: title, done: named)
                    .frame(width: 160)
            } else {
                Text(verbatim: title)
                    .italic(lookOnly)
                    .foregroundStyle(open ? .primary : .secondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(maxWidth: Self.maxWidth, alignment: .leading)
            }
            closeButton
                .opacity(closable && (open || hovering) && !editing ? 1 : 0)
                .allowsHitTesting(closable && (open || hovering) && !editing)
        }
        .font(.callout)
        .padding(.leading, VZ.Space.s)
        .padding(.trailing, VZ.Space.xs)
        .frame(minWidth: Self.minWidth, minHeight: 24)
        .background(shape.fill(Color.primary.opacity(
            open ? 0.09 : hovering ? 0.045 : 0)))
        .contentShape(shape)
        // A double-click on it names it (a click alone selects at once:
        // no wait for a second).
        .onClick({ if !editing { select() } },
                 double: { if !editing { rename() } })
        .onHover { hovering = $0 }
        .animation(VZ.Motion.quick, value: hovering)
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
        .accessibilityLabel(Text(verbatim: title))
        .accessibilityAddTraits(open ? [.isSelected, .isButton] : .isButton)
        .accessibilityAction(.default, select)
        .accessibilityAction(named: Text("Rename Prompt"), rename)
        .accessibilityAction(named: Text("Close Prompt"), close)
    }

    private var closeButton: some View {
        Button(action: close) {
            Image(systemName: "xmark")
                .font(.caption2.weight(.bold))
                .frame(width: 16, height: 16)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .foregroundStyle(.secondary)
        .help("Close this prompt")
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
            .textFieldStyle(.roundedBorder)
            .focused($focused)
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

/// Markdown drawn as it reads, or the text as typed: the Prompt Editor's
/// switch, and a text's opened large in the Atlas -- one setting, kept
/// between launches (AppModel.promptMarkdown).
struct MarkdownToggle: View {
    @Bindable var model: AppModel

    var body: some View {
        let on = model.promptMarkdown
        Button {
            model.promptMarkdown.toggle()
        } label: {
            Label("Markdown", systemImage: on ? "eye"
                  : "chevron.left.forwardslash.chevron.right")
                .labelStyle(.titleAndIcon)
                .padding(.horizontal, 6)
                .frame(height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .background(RoundedRectangle(cornerRadius: 5)
            .fill(on ? Color.accentColor.opacity(0.22) : .clear))
        .foregroundStyle(on ? Color.accentColor : Color.primary)
        .help(on ? "Markdown drawn as it reads: click to see it as typed"
                 : "Markdown as typed: click to draw it as it reads")
    }
}
