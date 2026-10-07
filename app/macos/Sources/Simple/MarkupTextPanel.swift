import AppKit

/// The markup text's EDIT WINDOW (DESIGN §10a Markup): a small panel over
/// the stage, moved and sized as any window, holding the selected text's
/// words -- as many lines as it takes, with the Edit menu's cut, copy,
/// paste, undo and redo as in any editor (⌘Z is the typing's while the
/// panel has the keyboard: AppModel.undoAny asks the key window's text
/// view first). What is typed is drawn on the stage as it is typed, and
/// KEPT on its layer once typing pauses, when the panel lets go of the
/// keyboard and when it closes -- markup edits coalesce in the project's
/// history, so a sentence is one Undo there.
///
/// It edits one text at a time: another text selected, its words are
/// loaded (the typing's undo starting again); none, the panel closes.
@MainActor
final class MarkupTextPanel: NSObject, NSWindowDelegate,
                             NSTextStorageDelegate {
    private var panel: NSPanel?
    private var textView: NSTextView?
    private weak var model: AppModel?
    /// The text it edits.
    private(set) var objectId: String?
    /// The typing's own history: a text loaded starts it again (the words
    /// it would undo into were another text's).
    private let undo = UndoManager()
    private var keepSoon: Task<Void, Never>?

    /// Shown by open() until closed -- hidden meanwhile while Valtz is
    /// not the active app, as panels are.
    var isOpen: Bool { objectId != nil && panel != nil }
    /// For the snapshot hooks.
    var window: NSWindow? { panel }
    var editor: NSTextView? { textView }

    /// Opened on the selected text and given the keyboard, its words
    /// selected: typing replaces them.
    func open(_ model: AppModel) {
        guard let t = model.markup.selectedText else { return }
        self.model = model
        let p = panel ?? make()
        load(t)
        if !p.isVisible { place(p) }
        p.makeKeyAndOrderFront(nil)
        p.makeFirstResponder(textView)
        textView?.selectAll(nil)
    }

    /// The selection changed: another text loaded, its words changed
    /// elsewhere (the project's undo) shown, none -- or the markup
    /// toolbar out of reach -- the panel closed.
    func follow(_ model: AppModel) {
        guard isOpen else { return }
        guard model.markupOpen && model.markupReady,
              let t = model.markup.selectedText else {
            close()
            return
        }
        if t.id != objectId {
            load(t)
        } else if let tv = textView, tv.string != t.text {
            tv.string = t.text
            undo.removeAllActions()
        }
    }

    func close() {
        keep()
        objectId = nil
        panel?.orderOut(nil)
    }

    // MARK: The window

    private func make() -> NSPanel {
        let p = NSPanel(
            contentRect: NSRect(x: 0, y: 0, width: 320, height: 150),
            styleMask: [.titled, .closable, .resizable, .utilityWindow],
            backing: .buffered, defer: false)
        p.title = String(localized: "Text")
        p.isFloatingPanel = true
        p.hidesOnDeactivate = true
        p.isReleasedWhenClosed = false
        p.minSize = NSSize(width: 200, height: 90)
        p.delegate = self

        let scroll = NSTextView.scrollableTextView()
        scroll.hasVerticalScroller = true
        scroll.borderType = .noBorder
        let tv = scroll.documentView as! NSTextView
        tv.isRichText = false
        tv.importsGraphics = false
        tv.allowsUndo = true
        tv.usesFindBar = true
        tv.textContainerInset = NSSize(width: 4, height: 6)
        tv.font = .systemFont(ofSize: 14)
        // The storage, not the view's textDidChange: undo and redo
        // change the words without it.
        tv.textStorage?.delegate = self
        p.contentView = scroll
        textView = tv
        panel = p
        return p
    }

    /// Where it was last left; the first time, at the stage's top right.
    private func place(_ p: NSPanel) {
        let name = "MarkupTextPanel"
        if !p.setFrameUsingName(name),
           let main = NSApp.windows.first(where: {
               $0.isVisible && $0.canBecomeMain && !($0 is NSPanel)
           }) {
            let f = main.frame
            p.setFrameTopLeftPoint(NSPoint(
                x: f.maxX - p.frame.width - 24, y: f.maxY - 110))
        }
        p.setFrameAutosaveName(name)
    }

    /// `t`'s words in the editor, in its typeface.
    private func load(_ t: MarkupObject) {
        guard let tv = textView else { return }
        keepSoon?.cancel()
        objectId = t.id
        tv.font = NSFont(name: t.font.family, size: 14)
            ?? .systemFont(ofSize: 14)
        tv.string = t.text
        undo.removeAllActions()
    }

    /// What was typed, kept on its layer -- while the text is still the
    /// one selected (a selection let go has kept it already).
    private func keep() {
        keepSoon?.cancel()
        keepSoon = nil
        guard let model, let id = objectId,
              model.markup.selectedText?.id == id else { return }
        model.commitSelection(keep: true)
    }

    // MARK: Delegates

    func windowWillReturnUndoManager(_ window: NSWindow) -> UndoManager? {
        undo
    }

    func windowDidResignKey(_ notification: Notification) { keep() }

    func windowWillClose(_ notification: Notification) {
        keep()
        objectId = nil
    }

    nonisolated func textStorage(
        _ textStorage: NSTextStorage,
        didProcessEditing editedMask: NSTextStorageEditActions,
        range editedRange: NSRange, changeInLength delta: Int
    ) {
        guard editedMask.contains(.editedCharacters) else { return }
        MainActor.assumeIsolated { wordsChanged() }
    }

    /// The words changed -- typed, cut, pasted, undone: drawn at once,
    /// kept once typing pauses.
    private func wordsChanged() {
        guard let model, let id = objectId, let tv = textView,
              model.markup.selectedText?.text != tv.string else { return }
        let s = tv.string
        model.editSelection(commit: false) {
            if $0.id == id { $0.text = s }
        }
        keepSoon?.cancel()
        keepSoon = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(800))
            if !Task.isCancelled { self?.keep() }
        }
    }
}
