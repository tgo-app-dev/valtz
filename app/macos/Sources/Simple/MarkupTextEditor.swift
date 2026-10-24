import AppKit

/// The markup text's EDITOR, IN PLACE (DESIGN §10a Markup): a text box's
/// words -- or a shape's -- typed where they lie on the stage, in their
/// own font, size, colour, alignment and spacing at the stage's zoom,
/// turned with their object: a text view in the stage's markup overlay,
/// laid out in the object's own pixels (its bounds are canvas pixels, its
/// frame the points they are shown at), so its lines break where the
/// core's do. A text box's words run down from its top, every line shown
/// while it is typed (the box shows the lines that fit whole once it is
/// done); a shape's sit centred in its text area. The Edit menu's cut,
/// copy, paste, undo and redo are the text's while it has the keyboard
/// (⌘Z its typing first: AppModel.undoAny asks the key window's text
/// view). What is typed is drawn as it is typed -- the object live, its
/// words the editor's -- and KEPT on its layer once typing pauses and
/// when the editing ends: Escape, a click elsewhere, another tool or
/// selection, the markup toolbar shut. Markup edits coalesce in the
/// project's history, so a sentence is one Undo there. A text box left
/// with no words goes.
@MainActor
final class MarkupTextEditor: NSObject, NSTextStorageDelegate {
    /// The object's own box at the stage's zoom, turned with it -- and in
    /// it the same box, its bounds its own pixels. Apart: a view's
    /// frameCenterRotation turns it about the centre its BOUNDS' size
    /// makes (seen: 44 points off at 130%), so a scaled one cannot turn.
    private let rotor = EditorHolder()
    private let holder = EditorHolder()
    private let textView: InPlaceTextView
    private let container: NSTextContainer
    private let layoutManager = NSLayoutManager()
    private weak var host: MarkupOverlayView?
    private weak var model: AppModel?
    /// The object it edits.
    private(set) var objectId: String?
    private var keepSoon: Task<Void, Never>?
    /// Its words set from the object, not typed.
    private var loading = false
    /// The look its text was last set in: set again only when it changes
    /// (not at each word typed).
    private var styled: Look?

    private struct Look: Equatable {
        var font: MarkupFont
        var color: RGBA
        var align: String?
        var spacing: Double?
        var paragraph: Double?
        var text: Bool
    }

    var isOpen: Bool { objectId != nil }
    /// For the snapshot hooks.
    var editor: NSTextView? { isOpen ? textView : nil }

    override init() {
        container = NSTextContainer(size: NSSize(
            width: 100, height: CGFloat.greatestFiniteMagnitude))
        container.lineFragmentPadding = 0
        container.widthTracksTextView = false
        layoutManager.addTextContainer(container)
        let storage = NSTextStorage()
        storage.addLayoutManager(layoutManager)
        textView = InPlaceTextView(frame: .zero, textContainer: container)
        super.init()
        let tv = textView
        tv.owner = self
        tv.isRichText = false
        tv.importsGraphics = false
        tv.allowsUndo = true
        tv.drawsBackground = false
        tv.textContainerInset = .zero
        tv.isVerticallyResizable = false
        tv.isHorizontallyResizable = false
        tv.focusRingType = .none
        tv.usesFindBar = false
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        // The storage, not the view's textDidChange: undo and redo change
        // the words without it.
        storage.delegate = self
        holder.addSubview(tv)
        rotor.addSubview(holder)
    }

    /// Opened on the selected text -- or shape -- and given the keyboard,
    /// its words selected: typing replaces them. A shape with none yet
    /// takes the toolbar's font, centred, in a colour that shows on its
    /// fill.
    func open(_ model: AppModel) {
        guard let o = model.markup.selectedWords,
              let host = model.markupTextHost else { return }
        if objectId != nil && objectId != o.id { close() }
        self.model = model
        if o.fillable && o.text.isEmpty {
            let font = model.markup.font
            model.editSelection(commit: false) {
                guard $0.id == o.id else { return }
                $0.font = font
                $0.align = nil
                $0.textColor = Self.textColor(on: $0.fill)
            }
        } else if o.kind == .text && !o.box {
            // A text from before: a box round what it shows, a pixel
            // wider -- its lines kept as they were.
            model.editSelection(commit: false) {
                guard $0.id == o.id else { return }
                let b = MarkupRender.ownBounds($0)
                ($0.x0, $0.y0, $0.x1, $0.y1) = (b.minX, b.minY,
                                                b.maxX + 1, b.maxY)
                $0.box = true
            }
        }
        objectId = o.id
        model.markup.editingText = o.id
        model.liveSelection()
        attach(to: host)
        if let now = model.markup.selection.first(where: { $0.id == o.id }) {
            load(now)
        }
        host.window?.makeFirstResponder(textView)
        textView.selectAll(nil)
    }

    /// The selection changed: its words or style changed elsewhere (the
    /// toolbar, the project's undo) shown; another object -- none, the
    /// markup toolbar out of reach -- the editing ended.
    func follow(_ model: AppModel) {
        guard isOpen else { return }
        guard model.markupOpen && model.markupReady,
              let o = model.markup.selectedWords, o.id == objectId else {
            close()
            return
        }
        if model.markupTextHost !== host, let h = model.markupTextHost {
            attach(to: h)
        }
        if !textView.hasMarkedText() {
            if textView.string != o.text {
                loading = true
                textView.string = o.text
                loading = false
                styled = nil
                textView.undoManager?.removeAllActions()
            }
            style(o)
        }
        layout()
    }

    /// The editing ended: its words kept, the editor gone -- a text box
    /// left with none, gone too.
    func close() {
        guard let id = objectId else { return }
        keepSoon?.cancel()
        keepSoon = nil
        objectId = nil
        // Out of the window once the responder change it may be called
        // from is done -- unless opened again meanwhile.
        let r = rotor
        DispatchQueue.main.async { [weak self] in
            if self?.isOpen != true { r.removeFromSuperview() }
        }
        if host?.textEditor === self { host?.textEditor = nil }
        textView.undoManager?.removeAllActions()
        guard let model else { return }
        let m = model.markup
        m.editingText = nil
        if let o = m.selection.first(where: { $0.id == id }),
           o.kind == .text,
           o.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            model.deleteSelectedObjects()
            model.settleSelection()
            return
        }
        if m.selection.contains(where: { $0.id == id }) {
            model.commitSelection(keep: true)
        }
    }

    // MARK: Where it is

    private func attach(to host: MarkupOverlayView) {
        if self.host !== host {
            self.host?.textEditor = nil
            rotor.removeFromSuperview()
        }
        self.host = host
        host.textEditor = self
        if rotor.superview !== host { host.addSubview(rotor) }
    }

    /// Laid over its object as the stage shows it now: its box at the
    /// zoom, turned with it; the words in it -- a text box's from its top,
    /// a shape's centred in its text area.
    func layout() {
        guard let id = objectId, let host, let model,
              let o = model.markup.selection.first(where: { $0.id == id })
        else { return }
        let t = host.toView
        let ppp = max(1e-6, hypot(t.a, t.b))
        let own = MarkupRender.box(o)
        let area = o.kind == .text ? own : MarkupRender.textArea(o) ?? own
        let c = CGPoint(x: own.midX, y: own.midY)
            .applying(MarkupRender.transform(o)).applying(t)
        if rotor.frameCenterRotation != 0 { rotor.frameCenterRotation = 0 }
        rotor.frame = CGRect(x: c.x - own.width * ppp / 2,
                             y: c.y - own.height * ppp / 2,
                             width: max(1, own.width * ppp),
                             height: max(1, own.height * ppp))
        holder.frame = rotor.bounds
        holder.setBoundsSize(NSSize(width: max(1e-3, own.width),
                                    height: max(1e-3, own.height)))
        holder.setBoundsOrigin(own.origin)
        if o.rotate != 0 { rotor.frameCenterRotation = o.rotate }
        container.size = NSSize(width: max(1, area.width),
                                height: CGFloat.greatestFiniteMagnitude)
        layoutManager.ensureLayout(for: container)
        let line = MarkupRender.lineHeight(o.font)
        let used = max(layoutManager.usedRect(for: container).height, line)
        let top = o.kind == .text ? area.minY
            : area.minY + max(0, (area.height - used) / 2)
        textView.frame = CGRect(x: area.minX, y: top,
                                width: max(1, area.width), height: used)
        textView.needsDisplay = true
    }

    /// Where its words are typed -- and its object's box -- in the
    /// window's points from its top left (the snapshot hooks print them).
    var frameInWindow: CGRect? { inWindow(textView) }
    var boxInWindow: CGRect? { inWindow(holder) }

    private func inWindow(_ v: NSView) -> CGRect? {
        guard isOpen, let w = v.window else { return nil }
        let r = v.convert(v.bounds, to: nil)
        return CGRect(x: r.minX, y: w.frame.height - r.maxY,
                      width: r.width, height: r.height)
    }

    // MARK: Its words

    /// The object's words in the editor, in their style; the typing's
    /// history begun again.
    private func load(_ o: MarkupObject) {
        loading = true
        textView.string = o.text
        loading = false
        styled = nil
        style(o)
        textView.undoManager?.removeAllActions()
        layout()
    }

    /// The editor's text in the object's font, colour and paragraph
    /// style, as the core sets them -- the line spacing between lines.
    private func style(_ o: MarkupObject) {
        let rgba = o.kind == .text
            ? o.stroke : o.textColor ?? RGBA(r: 0, g: 0, b: 0, a: 1)
        let look = Look(font: o.font, color: rgba, align: o.align,
                        spacing: o.lineSpacing,
                        paragraph: o.paragraphSpacing, text: o.kind == .text)
        guard look != styled else { return }
        styled = look
        let font = MarkupRender.font(o.font) as NSFont
        let color = NSColor(cgColor: rgba.cgColor) ?? .black
        let p = NSMutableParagraphStyle()
        p.alignment = switch o.align ?? (o.kind == .text ? "left"
                                                         : "center") {
        case "center": .center
        case "right": .right
        case "justified": .justified
        default: .left
        }
        p.lineSpacing = CGFloat(min(3, max(1, o.lineSpacing ?? 1)) - 1)
            * MarkupRender.lineHeight(o.font)
        p.paragraphSpacing = CGFloat(min(2000, max(0,
                                                   o.paragraphSpacing ?? 0)))
        var attrs: [NSAttributedString.Key: Any] = [
            .font: font, .foregroundColor: color, .paragraphStyle: p,
        ]
        if o.font.underline {
            attrs[.underlineStyle] = NSUnderlineStyle.single.rawValue
        }
        // The view's own defaults too: a selection change in empty text
        // sets the typing attributes back to them.
        textView.font = font
        textView.textColor = color
        textView.defaultParagraphStyle = p
        textView.typingAttributes = attrs
        textView.insertionPointColor = color.alphaComponent > 0.2
            ? color : .controlAccentColor
        if let s = textView.textStorage, s.length > 0 {
            let selection = textView.selectedRanges
            s.beginEditing()
            s.setAttributes(attrs, range: NSRange(location: 0,
                                                  length: s.length))
            s.endEditing()
            textView.selectedRanges = selection
        }
    }

    /// Words that show on a shape's fill: white on a dark one, black on a
    /// light one or none.
    static func textColor(on fill: RGBA) -> RGBA {
        let luma = 0.2126 * fill.r + 0.7152 * fill.g + 0.0722 * fill.b
        return fill.a > 0.35 && luma < 0.5
            ? RGBA(r: 1, g: 1, b: 1, a: 1) : RGBA(r: 0, g: 0, b: 0, a: 1)
    }

    /// What was typed, kept on its layer -- the selection still live: the
    /// editor draws its words until the editing ends.
    private func keep() {
        keepSoon?.cancel()
        keepSoon = nil
        guard let model, let id = objectId,
              model.markup.selection.contains(where: { $0.id == id })
        else { return }
        model.commitSelection(keep: true)
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
        guard !loading, let model, let id = objectId else { return }
        let s = textView.string
        if model.markup.selection.first(where: { $0.id == id })?.text != s {
            model.editSelection(commit: false) {
                if $0.id == id { $0.text = s }
            }
        }
        // Laid out once the storage is done with its edit.
        DispatchQueue.main.async { [weak self] in self?.layout() }
        keepSoon?.cancel()
        keepSoon = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(800))
            if !Task.isCancelled { self?.keep() }
        }
    }

    /// The text view let go of the keyboard -- a click on the stage, in
    /// another field: the editing ended.
    fileprivate func lostKeyboard() {
        guard isOpen else { return }
        close()
    }
}

/// The editor's holder: flipped, as the canvas's pixels run down.
private final class EditorHolder: NSView {
    override var isFlipped: Bool { true }

    override init(frame: NSRect) {
        super.init(frame: frame)
        clipsToBounds = false
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }
}

/// The editor's text view: a typing history of its own -- the window's
/// would keep steps on a view that goes -- Escape ending the editing (not
/// completing a word), and the keyboard let go ending it too.
final class InPlaceTextView: NSTextView {
    fileprivate weak var owner: MarkupTextEditor?
    private let typing = UndoManager()

    override var undoManager: UndoManager? { typing }

    override func keyDown(with event: NSEvent) {
        if event.keyCode == 53, !hasMarkedText() {
            owner?.close()
            return
        }
        super.keyDown(with: event)
    }

    override func resignFirstResponder() -> Bool {
        let done = super.resignFirstResponder()
        if done {
            let owner = owner
            // After the responder change: the click that took the
            // keyboard is handled with the editing ended.
            owner?.lostKeyboard()
        }
        return done
    }
}
