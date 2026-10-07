import AppKit
import AVFoundation
import ImageIO
import SwiftUI
import UniformTypeIdentifiers

/// A MENTION in the prompt text: a staged medium (the row over the text,
/// PromptAttachment) named where it stands -- a small thumbnail with the
/// number the model calls it by.
final class MentionAttachment: NSTextAttachment {
    let item: UUID

    init(item: UUID, image: NSImage) {
        self.item = item
        super.init(data: nil, ofType: nil)
        self.image = image
        let s = image.size
        // Centred on the text line rather than sat on the baseline.
        bounds = CGRect(x: 0, y: -(s.height - 12) / 2, width: s.width,
                        height: s.height)
    }

    required init?(coder: NSCoder) { fatalError("not used") }
}

/// A positional REFERENCE in the prompt text (DESIGN §10c): the text's
/// "<valtz_ref_img_0>" -- the row's first picture, whatever is there --
/// drawn as a TAG: its kind and place ("Image 1") in the accent, joined to
/// the thumbnail of what the row has there; red, alone, while it has
/// nothing there yet.
final class RefTagAttachment: NSTextAttachment {
    let kind: String   // "img" | "vid" | "aud"
    let index: Int     // its place within its kind, from 0

    init(kind: String, index: Int, image: NSImage) {
        self.kind = kind
        self.index = index
        super.init(data: nil, ofType: nil)
        self.image = image
        let s = image.size
        bounds = CGRect(x: 0, y: -(s.height - 12) / 2, width: s.width,
                        height: s.height)
    }

    required init?(coder: NSCoder) { fatalError("not used") }

    /// What it stands for in the prompt's text.
    var tag: String { "<valtz_ref_\(kind)_\(index)>" }

    /// Its kind as a person reads it.
    static func kindName(_ kind: String) -> String {
        switch kind {
        case "vid": Modality.video.label
        case "aud": Modality.audio.label
        default: Modality.image.label
        }
    }
}

/// The prompt text view: plain text with MENTIONS of the staged media --
/// a thumbnail dragged in from the row over it, or a file dropped (or
/// pasted) into it, which is staged and mentioned where it lands.
final class PromptTextView: NSTextView {
    /// Files dropped or pasted in: staged, their ids back, in order.
    var onFilesDropped: (([URL]) -> [UUID])?
    var onDropTargeted: ((Bool) -> Void)?
    /// A mention's picture, as the row has it now.
    var mentionImage: ((UUID) -> NSImage)?
    /// A click in the text: the stage shows what it showed again.
    var onClick: (() -> Void)?
    /// A positional tag's picture: its kind and place, joined to what the
    /// row has there.
    var refTagImage: ((String, Int) -> NSImage)?
    /// Focus taken: this is the prompt the toolbar formats.
    var onFocus: (() -> Void)?
    /// Focus taken or let go: Markdown's markers show only while the
    /// text is being edited.
    var onFocusChange: (() -> Void)?

    override func becomeFirstResponder() -> Bool {
        let took = super.becomeFirstResponder()
        if took {
            onFocus?()
            onFocusChange?()
        }
        return took
    }

    override func resignFirstResponder() -> Bool {
        let gave = super.resignFirstResponder()
        if gave { onFocusChange?() }
        return gave
    }

    /// ⌘B, ⌘I, ⌘U: Markdown's bold, italic and underline (PromptMarkdown)
    /// -- the text stays plain.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if window?.firstResponder === self,
           event.modifierFlags.intersection(.deviceIndependentFlagsMask)
               == .command {
            switch event.charactersIgnoringModifiers {
            case "b": toggleMarkdown(.bold); return true
            case "i": toggleMarkdown(.italic); return true
            case "u": toggleMarkdown(.underline); return true
            default: break
            }
        }
        return super.performKeyEquivalent(with: event)
    }

    /// What a thumbnail dragged from the row carries: this, and its id.
    static let referencePrefix = "valtz-reference:"
    static let mentionHeight: CGFloat = 24

    /// Assets dragged in from the list: staged, their ids back.
    var onAssetsDropped: (([String]) -> [UUID])?
    /// The stage dragged in: what it shows, captured and staged.
    var onStageDropped: (() -> Void)?

    /// A drag from the stage.
    static func draggedStage(_ pb: NSPasteboard) -> Bool {
        pb.string(forType: .string) == AppModel.stagePrefix
    }

    /// The staged medium a drag from the row carries.
    static func draggedReference(_ pb: NSPasteboard) -> UUID? {
        guard let s = pb.string(forType: .string),
              s.hasPrefix(referencePrefix) else { return nil }
        return UUID(uuidString: String(s.dropFirst(referencePrefix.count)))
    }

    /// An asset a drag from the asset list carries.
    @MainActor
    static func draggedAsset(_ pb: NSPasteboard) -> String? {
        pb.string(forType: .string).flatMap { AppModel.draggedAsset($0) }
    }

    private func carriesMedia(_ pb: NSPasteboard) -> Bool {
        Self.draggedReference(pb) != nil || Self.draggedAsset(pb) != nil
            || Self.draggedStage(pb) || !Self.fileURLs(pb).isEmpty
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        guard carriesMedia(sender.draggingPasteboard) else {
            return super.draggingEntered(sender)
        }
        if Self.draggedReference(sender.draggingPasteboard) == nil {
            onDropTargeted?(true)
        }
        return .copy
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        guard carriesMedia(sender.draggingPasteboard) else {
            return super.draggingUpdated(sender)
        }
        // Show the caret where the mention will land.
        let p = convert(sender.draggingLocation, from: nil)
        setSelectedRange(NSRange(location: characterIndexForInsertion(at: p),
                                 length: 0))
        return .copy
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        onDropTargeted?(false)
        super.draggingExited(sender)
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        let pb = sender.draggingPasteboard
        onDropTargeted?(false)
        let p = convert(sender.draggingLocation, from: nil)
        let at = characterIndexForInsertion(at: p)
        // A thumbnail from the row: mentioned here -- the one way a
        // mention is made. (Released here, not on the row: the stage stays
        // as it is.)
        if let id = Self.draggedReference(pb) {
            insertMentions([id], at: at)
            window?.makeFirstResponder(self)
            return true
        }
        // Anything else is STAGED in the row, mentioned nowhere -- a
        // mention is made by dragging its thumbnail into the text. The
        // stage: what it shows, captured.
        if Self.draggedStage(pb) {
            onStageDropped?()
            return true
        }
        if let asset = Self.draggedAsset(pb) {
            _ = onAssetsDropped?([asset])
            return true
        }
        let urls = Self.fileURLs(pb)
        guard !urls.isEmpty else {
            return super.performDragOperation(sender)
        }
        _ = onFilesDropped?(urls)
        return true
    }

    override func paste(_ sender: Any?) {
        let urls = Self.fileURLs(NSPasteboard.general)
        if urls.isEmpty {
            pasteAsPlainText(sender)  // no fonts or colors from elsewhere
        } else {
            _ = onFilesDropped?(urls)  // staged in the row
        }
    }

    override func mouseDown(with event: NSEvent) {
        onClick?()
        super.mouseDown(with: event)
    }

    /// Mentions of `ids` at `index`, each followed by a space, as one
    /// undoable change.
    func insertMentions(_ ids: [UUID], at index: Int) {
        guard let storage = textStorage, !ids.isEmpty else { return }
        let at = min(index, storage.length)
        let piece = NSMutableAttributedString()
        for id in ids {
            let img = mentionImage?(id) ?? NSImage()
            piece.append(NSAttributedString(
                attachment: MentionAttachment(item: id, image: img)))
            piece.append(NSAttributedString(string: " "))
        }
        piece.addAttributes(typingAttributes,
                            range: NSRange(location: 0, length: piece.length))
        guard shouldChangeText(in: NSRange(location: at, length: 0),
                               replacementString: piece.string) else {
            return
        }
        storage.insert(piece, at: at)
        didChangeText()
        setSelectedRange(NSRange(location: at + piece.length, length: 0))
    }

    /// The mentions drawn again as the row has them now (their numbers,
    /// the base, thumbnails that loaded) -- and those of media no longer
    /// staged taken out. True when the text changed.
    @discardableResult
    func refreshMentions(staged: Set<UUID>) -> Bool {
        guard let storage = textStorage else { return false }
        var found: [(NSRange, UUID)] = []
        storage.enumerateAttribute(
            .attachment, in: NSRange(location: 0, length: storage.length)
        ) { value, range, _ in
            if let a = value as? MentionAttachment { found.append((range, a.item)) }
        }
        guard !found.isEmpty else { return false }
        var removed = false
        storage.beginEditing()
        // From the end, so the ranges before stay where they are.
        for (range, id) in found.reversed() {
            if !staged.contains(id) {
                // The mention, and the space that followed it.
                var r = range
                if r.upperBound < storage.length,
                   (storage.string as NSString).character(at: r.upperBound)
                       == 0x20 {
                    r.length += 1
                }
                storage.deleteCharacters(in: r)
                removed = true
                continue
            }
            let attrs = storage.attributes(at: range.location,
                                           effectiveRange: nil)
                .filter { $0.key != .attachment }
            let piece = NSMutableAttributedString(attachment: MentionAttachment(
                item: id, image: mentionImage?(id) ?? NSImage()))
            piece.addAttributes(attrs, range: NSRange(location: 0, length: 1))
            storage.replaceCharacters(in: range, with: piece)
        }
        storage.endEditing()
        // Undo kept the text's old ranges; a length that changed outside
        // it would replay them in the wrong places.
        if removed { undoManager?.removeAllActions() }
        return removed
    }

    // MARK: Positional tags (DESIGN §10c)

    /// A tag as text with its look: an attachment in the typing
    /// attributes.
    func tagPiece(kind: String, index: Int) -> NSAttributedString {
        let piece = NSMutableAttributedString(attachment: RefTagAttachment(
            kind: kind, index: index,
            image: refTagImage?(kind, index) ?? NSImage()))
        piece.addAttributes(typingAttributes.filter { $0.key != .attachment },
                            range: NSRange(location: 0, length: 1))
        return piece
    }

    /// Tags typed or pasted as text ("<valtz_ref_img_0>") made tags --
    /// each one undoable edit. True when any was.
    @discardableResult
    func convertTypedTags() -> Bool {
        guard let storage = textStorage else { return false }
        let found = AppModel.refTags(in: storage.string)
        guard !found.isEmpty else { return false }
        var sel = selectedRange()
        for t in found.reversed() {
            let piece = tagPiece(kind: t.kind, index: t.index)
            guard shouldChangeText(in: t.range,
                                   replacementString: piece.string) else {
                continue
            }
            storage.replaceCharacters(in: t.range, with: piece)
            // The caret keeps its place in the text that follows.
            if sel.location >= t.range.upperBound {
                sel.location -= t.range.length - 1
            }
        }
        didChangeText()
        setSelectedRange(NSRange(location: min(sel.location, storage.length),
                                 length: 0))
        return true
    }

    /// The tag at `location` made `kind`, `index` -- one undoable edit,
    /// the selection kept on it.
    func setTag(at location: Int, kind: String, index: Int) {
        guard let storage = textStorage, location < storage.length,
              storage.attribute(.attachment, at: location,
                                effectiveRange: nil) is RefTagAttachment
        else { return }
        let r = NSRange(location: location, length: 1)
        let attrs = storage.attributes(at: location, effectiveRange: nil)
            .filter { $0.key != .attachment }
        let piece = NSMutableAttributedString(attachment: RefTagAttachment(
            kind: kind, index: max(0, index),
            image: refTagImage?(kind, max(0, index)) ?? NSImage()))
        piece.addAttributes(attrs, range: NSRange(location: 0, length: 1))
        guard shouldChangeText(in: r, replacementString: piece.string) else {
            return
        }
        storage.replaceCharacters(in: r, with: piece)
        didChangeText()
        setSelectedRange(r)
    }

    /// The tags drawn again as the row has them now: bound or not, the
    /// thumbnail of what is at their place. The text is unchanged.
    func refreshTags() {
        guard let storage = textStorage else { return }
        var found: [(NSRange, RefTagAttachment)] = []
        storage.enumerateAttribute(
            .attachment, in: NSRange(location: 0, length: storage.length)
        ) { value, range, _ in
            if let a = value as? RefTagAttachment { found.append((range, a)) }
        }
        guard !found.isEmpty else { return }
        // The same characters, drawn anew: the selection stays where it
        // is (a replacement would put it after them -- off a selected
        // tag, its editor closing).
        let selection = selectedRanges
        storage.beginEditing()
        for (range, a) in found {
            let attrs = storage.attributes(at: range.location,
                                           effectiveRange: nil)
                .filter { $0.key != .attachment }
            let piece = NSMutableAttributedString(attachment: RefTagAttachment(
                kind: a.kind, index: a.index,
                image: refTagImage?(a.kind, a.index) ?? NSImage()))
            piece.addAttributes(attrs, range: NSRange(location: 0, length: 1))
            storage.replaceCharacters(in: range, with: piece)
        }
        storage.endEditing()
        selectedRanges = selection
    }

    /// The tag selected, if the selection is exactly one.
    var selectedTag: (location: Int, tag: RefTagAttachment)? {
        let r = selectedRange()
        guard r.length == 1, let storage = textStorage,
              r.location < storage.length,
              let a = storage.attribute(.attachment, at: r.location,
                                        effectiveRange: nil)
                as? RefTagAttachment else { return nil }
        return (r.location, a)
    }

    /// Where a character is drawn, in the view.
    func rect(ofCharacter at: Int) -> NSRect {
        guard let lm = layoutManager, let tc = textContainer else {
            return .zero
        }
        let g = lm.glyphRange(forCharacterRange: NSRange(location: at,
                                                         length: 1),
                              actualCharacterRange: nil)
        var r = lm.boundingRect(forGlyphRange: g, in: tc)
        r.origin.x += textContainerOrigin.x
        r.origin.y += textContainerOrigin.y
        return r
    }

    /// Insert Reference: a positional tag at the caret -- the first place
    /// of that kind no tag in the text names yet.
    override func menu(for event: NSEvent) -> NSMenu? {
        let menu = super.menu(for: event) ?? NSMenu()
        let insert = NSMenu()
        for kind in ["img", "vid", "aud"] {
            let item = NSMenuItem(title: RefTagAttachment.kindName(kind),
                                  action: #selector(insertRefTag(_:)),
                                  keyEquivalent: "")
            item.representedObject = kind
            item.target = self
            insert.addItem(item)
        }
        let top = NSMenuItem(title: String(localized: "Insert Reference"),
                             action: nil, keyEquivalent: "")
        top.submenu = insert
        menu.insertItem(top, at: 0)
        menu.insertItem(.separator(), at: 1)
        return menu
    }

    @objc private func insertRefTag(_ sender: NSMenuItem) {
        guard let kind = sender.representedObject as? String,
              let storage = textStorage else { return }
        var used = Set<Int>()
        storage.enumerateAttribute(
            .attachment, in: NSRange(location: 0, length: storage.length)
        ) { value, _, _ in
            if let a = value as? RefTagAttachment, a.kind == kind {
                used.insert(a.index)
            }
        }
        var index = 0
        while used.contains(index) { index += 1 }
        let piece = tagPiece(kind: kind, index: index)
        let at = selectedRange()
        guard shouldChangeText(in: at, replacementString: piece.string) else {
            return
        }
        storage.replaceCharacters(in: at, with: piece)
        didChangeText()
        setSelectedRange(NSRange(location: at.location, length: 1))
    }

    /// A positional tag's look: "Image 1" on a rounded tag in the accent,
    /// joined to the thumbnail of what the row has there; in red, alone,
    /// while the row has nothing there. Drawn as the appearance is when
    /// it is drawn (the accent is near black, near white in the dark).
    static func refTagChip(kind: String, index: Int, bound: Bool,
                           thumb: CGImage?) -> NSImage {
        let h: CGFloat = 22
        let label = RefTagAttachment.kindName(kind) + " " + String(index + 1)
        let font = NSFont.systemFont(ofSize: 11, weight: .semibold)
        let textW = ceil(NSAttributedString(string: label,
                                            attributes: [.font: font])
            .size().width)
        let labelW = textW + 14
        let aspect = thumb.map {
            CGFloat($0.width) / CGFloat(max(1, $0.height))
        } ?? 1
        let thumbW = bound ? min(max(h * aspect, h * 0.8), h * 1.6) : 0
        let size = NSSize(width: labelW + thumbW, height: h)
        return NSImage(size: size, flipped: false) { r in
            let fill: NSColor = bound ? .controlAccentColor : .systemRed
            // Light text on a dark accent, dark on a light one.
            let rgb = fill.usingColorSpace(.sRGB)
            let lum = rgb.map {
                0.2126 * $0.redComponent + 0.7152 * $0.greenComponent
                    + 0.0722 * $0.blueComponent
            } ?? 0
            let ink: NSColor = lum > 0.6 ? .black : .white
            let whole = NSBezierPath(roundedRect: r, xRadius: 5, yRadius: 5)
            NSGraphicsContext.saveGraphicsState()
            whole.addClip()
            fill.setFill()
            NSRect(x: 0, y: 0, width: labelW, height: h).fill()
            if bound {
                let t = NSRect(x: labelW, y: 0, width: thumbW, height: h)
                if let thumb {
                    let s = max(t.width / CGFloat(thumb.width),
                                t.height / CGFloat(thumb.height))
                    let dw = CGFloat(thumb.width) * s
                    let dh = CGFloat(thumb.height) * s
                    NSGraphicsContext.current?.cgContext.draw(
                        thumb, in: CGRect(x: t.midX - dw / 2,
                                          y: t.midY - dh / 2,
                                          width: dw, height: dh))
                } else {
                    NSColor.quaternaryLabelColor.setFill()
                    t.fill()
                }
            }
            NSGraphicsContext.restoreGraphicsState()
            // One outline round the two, so they read as one.
            fill.setStroke()
            let ring = NSBezierPath(roundedRect: r.insetBy(dx: 0.5, dy: 0.5),
                                    xRadius: 4.5, yRadius: 4.5)
            ring.lineWidth = 1
            ring.stroke()
            let text = NSAttributedString(string: label, attributes: [
                .font: font, .foregroundColor: ink])
            let ts = text.size()
            text.draw(at: NSPoint(x: (labelW - ts.width) / 2,
                                  y: (h - ts.height) / 2 + 0.5))
            return true
        }
    }

    /// A mention's look: the thumbnail, small, rounded -- an accent ring
    /// on the base -- and its number in the corner; a placeholder until
    /// the thumbnail loads.
    static func mentionChip(_ cg: CGImage?, kind: String, number: Int?,
                            base: Bool) -> NSImage {
        let h = mentionHeight
        let aspect = cg.map { CGFloat($0.width) / CGFloat(max(1, $0.height)) }
            ?? 1
        let w = min(max(h * aspect, h * 0.8), h * 1.6)
        return NSImage(size: NSSize(width: w, height: h), flipped: false) { r in
            let path = NSBezierPath(roundedRect: r, xRadius: 5, yRadius: 5)
            NSGraphicsContext.saveGraphicsState()
            path.addClip()
            if let cg {
                let s = max(r.width / CGFloat(cg.width),
                            r.height / CGFloat(cg.height))
                let dw = CGFloat(cg.width) * s, dh = CGFloat(cg.height) * s
                NSGraphicsContext.current?.cgContext.draw(
                    cg, in: CGRect(x: (r.width - dw) / 2,
                                   y: (r.height - dh) / 2,
                                   width: dw, height: dh))
            } else {
                NSColor.quaternaryLabelColor.setFill()
                r.fill()
            }
            NSGraphicsContext.restoreGraphicsState()
            let ring = NSBezierPath(roundedRect: r.insetBy(dx: 0.75, dy: 0.75),
                                    xRadius: 4.5, yRadius: 4.5)
            ring.lineWidth = base ? 1.5 : 0.5
            (base ? NSColor.controlAccentColor
                  : NSColor.black.withAlphaComponent(0.2)).setStroke()
            ring.stroke()
            let label = number.map(String.init) ?? (kind == "video" ? "▶" : "")
            if !label.isEmpty {
                let font = NSFont.systemFont(ofSize: 8, weight: .bold)
                let text = NSAttributedString(string: label, attributes: [
                    .font: font, .foregroundColor: NSColor.white])
                let ts = text.size()
                let d = max(ts.width + 5, 11)
                let c = NSRect(x: r.maxX - d - 1.5, y: 1.5, width: d,
                               height: 11)
                (base ? NSColor.controlAccentColor
                      : NSColor.black.withAlphaComponent(0.6)).setFill()
                NSBezierPath(roundedRect: c, xRadius: 5.5, yRadius: 5.5).fill()
                text.draw(at: NSPoint(x: c.midX - ts.width / 2,
                                      y: c.midY - ts.height / 2))
            }
            return true
        }
    }

    // MARK: - Helpers

    static func fileURLs(_ pb: NSPasteboard) -> [URL] {
        (pb.readObjects(forClasses: [NSURL.self],
                        options: [.urlReadingFileURLsOnly: true]) as? [URL])
            ?? []
    }

    static func kind(of url: URL) -> String {
        guard let t = UTType(filenameExtension: url.pathExtension) else {
            return "file"
        }
        if t.conforms(to: .image) { return "image" }
        if t.conforms(to: .movie) || t.conforms(to: .video) { return "video" }
        if t.conforms(to: .audio) { return "audio" }
        return "file"
    }

    nonisolated static func imageThumbnail(_ url: URL, maxPixels: Int)
        -> CGImage? {
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return nil
        }
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: maxPixels,
            kCGImageSourceShouldCache: false,
        ]
        return CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary)
    }

    nonisolated static func videoFrame(_ url: URL, maxPixels: Int) async
        -> CGImage? {
        let gen = AVAssetImageGenerator(asset: AVURLAsset(url: url))
        gen.appliesPreferredTrackTransform = true
        gen.maximumSize = CGSize(width: maxPixels, height: maxPixels)
        return try? await gen.image(at: .zero).image
    }
}

/// A scroller without its track: over the white prompt card only the
/// knob shows, even in the always-visible (legacy) style.
final class KnobScroller: NSScroller {
    override class var isCompatibleWithOverlayScrollers: Bool { true }
    override func drawKnobSlot(in slotRect: NSRect, highlight flag: Bool) {}
}

/// The prompt's scroll view. Its vertical padding is a content inset, not
/// the text view's, so scrolling keeps the caret clear of the card's edges
/// (the host fades out what scrolls into them). The text view covers only
/// its text, so a click or a drop anywhere else in the box is passed on
/// to it: the whole card stays one field and one drop target.
final class PromptScrollView: NSScrollView {
    private var text: PromptTextView? { documentView as? PromptTextView }

    /// Typing goes to the prompt when it appears -- not to the first text
    /// field AppKit's key-view loop finds, which was one in the closed
    /// generation drawer.
    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        guard let window else { return }
        window.initialFirstResponder = documentView
        DispatchQueue.main.async { [weak self, weak window] in
            guard let self, let window, self.window === window else { return }
            window.makeFirstResponder(self.documentView)
        }
    }

    override func mouseDown(with event: NSEvent) {
        window?.makeFirstResponder(documentView)
    }

    override func draggingEntered(_ sender: NSDraggingInfo) -> NSDragOperation {
        text?.draggingEntered(sender) ?? []
    }

    override func draggingUpdated(_ sender: NSDraggingInfo) -> NSDragOperation {
        text?.draggingUpdated(sender) ?? []
    }

    override func draggingExited(_ sender: NSDraggingInfo?) {
        text?.draggingExited(sender)
    }

    override func performDragOperation(_ sender: NSDraggingInfo) -> Bool {
        text?.performDragOperation(sender) ?? false
    }
}

/// SwiftUI host: keeps `model.prompt` (the text) and
/// `model.promptMentions` (the staged media it mentions, in order) in
/// sync -- the mentions drawn as the row has them, those of media taken
/// out of the row taken out of the text -- and reports the height the
/// text needs so the box can grow smoothly, and how many lines it takes.
///
/// Two sizes once the text runs past two lines: the box grows with it up
/// to `maxHeight`, or -- `compact` -- stays three rows tall and scrolls,
/// with a scroll bar that stays visible so the hidden lines are evident.
struct PromptEditor: NSViewRepresentable {
    let model: AppModel
    @Binding var height: CGFloat
    @Binding var dropTargeted: Bool
    /// Lines the text takes as wrapped, an empty last line included.
    @Binding var lines: Int
    var compact = false
    var minHeight: CGFloat = 60
    var maxHeight: CGFloat = 240
    /// Text to mark wherever it occurs (the title bar's search).
    var highlight = ""
    /// Room around the text: the box's own, or immersive editing's wider
    /// margins.
    var inset = PromptEditor.inset

    /// The prompt's type: Georgia -- a serif keeps a long prompt, as a
    /// model's template makes it, readable -- falling back to the system's
    /// own for what it has no glyphs for: Chinese (and Japanese, Korean)
    /// in the system's sans, as it is.
    static let font: NSFont = {
        let size: CGFloat = 15
        let system = NSFont.systemFont(ofSize: size)
        guard let georgia = NSFont(name: "Georgia", size: size) else {
            return system
        }
        // The system font's own fallbacks for these languages (PingFang
        // for Chinese): Georgia's would pick a serif (Songti).
        let langs = Locale.preferredLanguages
            + ["zh-Hans", "zh-Hant", "ja", "ko"]
        let cascade = CTFontCopyDefaultCascadeListForLanguages(
            system as CTFont, langs as CFArray) as? [NSFontDescriptor] ?? []
        let d = georgia.fontDescriptor.addingAttributes(
            [.cascadeList: cascade])
        return NSFont(descriptor: d, size: size) ?? georgia
    }()
    /// A paragraph of Latin text: its lines 1.1x apart, a little room
    /// after it. A paragraph with Chinese (Japanese, Korean) in it keeps
    /// the system's own spacing.
    static let latinParagraph: NSParagraphStyle = {
        let p = NSMutableParagraphStyle()
        p.lineHeightMultiple = 1.1
        p.paragraphSpacing = 5
        return p
    }()
    static let cjkParagraph = NSParagraphStyle.default
    static let inset = NSSize(width: 14, height: 17)

    /// Han, kana, Hangul, and their punctuation: CJK text.
    nonisolated static func hasCJK(_ s: String) -> Bool {
        s.unicodeScalars.contains {
            switch $0.value {
            case 0x3000...0x303F, 0x3040...0x30FF, 0x3400...0x4DBF,
                 0x4E00...0x9FFF, 0xAC00...0xD7AF, 0xF900...0xFAFF,
                 0xFF00...0xFFEF, 0x20000...0x2FA1F: true
            default: false
            }
        }
    }

    @MainActor
    final class Coordinator: NSObject, NSTextViewDelegate {
        var parent: PromptEditor
        var revision = -1
        weak var textView: PromptTextView?
        weak var scrollView: NSScrollView?
        var wasCompact = false
        var marked = ""
        /// The row as the mentions were last drawn from it.
        var drawnFrom = ""
        /// The tag being edited: where it is, its editor, digits typed.
        var editingTagAt: Int?
        var tagEditor: NSPopover?
        var typedDigits = ""
        /// Markdown as last drawn: on or off, and the paragraph whose
        /// markers show (the caret's).
        var markdownOn: Bool?
        var caretParagraph = NSRange(location: NSNotFound, length: 0)

        init(_ p: PromptEditor) { parent = p }

        func textDidChange(_ notification: Notification) {
            sync()
        }

        func sync() {
            guard let tv = textView, let storage = tv.textStorage else {
                return
            }
            // An input method's unconfirmed text (Pinyin, Kana, ...): the
            // storage left alone until it is committed or cancelled, which
            // syncs. Restyled under it -- fonts re-set, CJK fonts
            // substituted again -- the composition was laid out at the
            // start of its run of CJK text, over the words there, and the
            // input method's panel went with it.
            if tv.hasMarkedText() {
                measure()
                return
            }
            // Tags typed or pasted as text become tags: an edit of its
            // own, which syncs again.
            if tv.convertTypedTags() { return }
            styleParagraphs()
            styleMarkdown()
            let (marked, items) = PromptEditor.marked(storage)
            let text = marked.replacingOccurrences(of: "\u{FFFC}", with: "")
            let model = parent.model
            if model.prompt != text || model.promptMentions != items
                || model.promptMarked != marked {
                model.prompt = text
                model.promptMarked = marked
                model.promptMentions = items
                model.promptChanged()
            }
            measure()
            mark(parent.highlight, reveal: false)
        }

        /// Each paragraph's spacing: a Latin one 1.1x, a CJK one the
        /// system's (PromptEditor.latinParagraph); typing follows the one
        /// at the caret.
        func styleParagraphs() {
            if let tv = textView { PromptEditor.styleParagraphs(tv) }
        }

        /// Markdown drawn as it reads (PromptMarkdown): every span's
        /// style; its markers dimmed in the caret's paragraph, hidden
        /// elsewhere. Off: the text as typed. Attributes only -- the
        /// characters, and so the prompt, are untouched.
        func styleMarkdown() {
            guard let tv = textView else { return }
            let on = parent.model.promptMarkdown
            caretParagraph = PromptEditor.caretParagraph(of: tv)
            // Nothing drawn before, and nothing to draw: leave the text.
            if markdownOn == false && !on { return }
            markdownOn = on
            PromptEditor.paintMarkdown(tv, on: on, caret: caretParagraph)
        }

        // MARK: Editing a tag (DESIGN §10c)

        /// A tag selected -- clicked, or the caret run over it -- opens its
        /// editor: its kind from a pop-up, its place typed or stepped.
        /// Digits typed while it is selected are its place.
        func textViewDidChangeSelection(_ notification: Notification) {
            // Composing: the selection is the input method's (sync).
            guard let tv = textView, !tv.hasMarkedText() else { return }
            // Another paragraph: its markers show, the last one's hide.
            if parent.model.promptMarkdown, let ns = tv.textStorage?.string
                as NSString? {
                let sel = tv.selectedRange()
                let at = min(sel.location, ns.length)
                let para = ns.paragraphRange(for: NSRange(
                    location: at, length: min(sel.length, ns.length - at)))
                if !NSEqualRanges(para, caretParagraph) {
                    styleMarkdown()
                    measure()
                }
            }
            if let sel = tv.selectedTag {
                if editingTagAt != sel.location { typedDigits = "" }
                editingTagAt = sel.location
                showTagEditor(sel.tag, at: sel.location)
            } else {
                editingTagAt = nil
                typedDigits = ""
                if tagEditor?.isShown == true { tagEditor?.close() }
            }
        }

        func textView(_ textView: NSTextView,
                      clickedOn cell: any NSTextAttachmentCellProtocol,
                      in cellFrame: NSRect, at charIndex: Int) {
            guard let storage = textView.textStorage,
                  charIndex < storage.length,
                  storage.attribute(.attachment, at: charIndex,
                                    effectiveRange: nil) is RefTagAttachment
            else { return }
            textView.setSelectedRange(NSRange(location: charIndex,
                                              length: 1))
        }

        func textView(_ textView: NSTextView,
                      shouldChangeTextIn range: NSRange,
                      replacementString s: String?) -> Bool {
            guard let tv = self.textView, let sel = tv.selectedTag,
                  range == NSRange(location: sel.location, length: 1),
                  let s else { return true }
            // Digits: the tag's place, as a person counts (1 the first).
            if !s.isEmpty, s.count <= 6,
               s.allSatisfy({ $0.isASCII && $0.isNumber }) {
                typedDigits = String((typedDigits + s).suffix(6))
                retag(sel.location, kind: sel.tag.kind)
                return false
            }
            // Delete takes a digit back while there are typed ones.
            if s.isEmpty, !typedDigits.isEmpty {
                typedDigits.removeLast()
                retag(sel.location, kind: sel.tag.kind)
                return false
            }
            return true
        }

        private func retag(_ at: Int, kind: String) {
            let n = max(1, Int(typedDigits) ?? 1)
            // After this change has been refused: an edit of its own.
            DispatchQueue.main.async { [weak self] in
                self?.textView?.setTag(at: at, kind: kind, index: n - 1)
            }
        }

        private func showTagEditor(_ tag: RefTagAttachment, at: Int) {
            guard let tv = textView, tv.window != nil else { return }
            let rect = tv.rect(ofCharacter: at)
            if let pop = tagEditor, pop.isShown,
               let c = pop.contentViewController as? RefTagEditor {
                c.show(kind: tag.kind, index: tag.index)
                pop.positioningRect = rect
                return
            }
            let c = RefTagEditor()
            c.onChange = { [weak self] kind, index in
                guard let self, let at = self.editingTagAt else { return }
                self.typedDigits = ""
                self.textView?.setTag(at: at, kind: kind, index: index)
            }
            c.loadViewIfNeeded()
            c.show(kind: tag.kind, index: tag.index)
            let pop = NSPopover()
            pop.contentViewController = c
            pop.behavior = .semitransient
            pop.animates = false
            pop.show(relativeTo: rect, of: tv, preferredEdge: .maxY)
            tagEditor = pop
            // Typing stays in the text: digits there are its place too.
            tv.window?.makeFirstResponder(tv)
        }

        /// Mark every occurrence of `query` as Find does, with temporary
        /// attributes (never saved with the text); a new query brings the
        /// first one into view.
        func mark(_ query: String, reveal: Bool) {
            guard let tv = textView, let lm = tv.layoutManager else { return }
            let text = tv.string as NSString
            lm.removeTemporaryAttribute(
                .backgroundColor,
                forCharacterRange: NSRange(location: 0, length: text.length))
            lm.removeTemporaryAttribute(
                .foregroundColor,
                forCharacterRange: NSRange(location: 0, length: text.length))
            marked = query
            let q = query.trimmingCharacters(in: .whitespaces)
            guard !q.isEmpty else { return }
            var first: NSRange?
            var from = 0
            while from < text.length {
                let r = text.range(
                    of: q, options: [.caseInsensitive, .diacriticInsensitive],
                    range: NSRange(location: from, length: text.length - from))
                guard r.location != NSNotFound else { break }
                lm.addTemporaryAttributes(
                    [.backgroundColor: NSColor.findHighlightColor,
                     .foregroundColor: NSColor.black],
                    forCharacterRange: r)
                if first == nil { first = r }
                from = r.location + max(r.length, 1)
            }
            if reveal, let first { tv.scrollRangeToVisible(first) }
        }

        func measure() {
            guard let tv = textView, let lm = tv.layoutManager,
                  let tc = tv.textContainer else { return }
            lm.ensureLayout(for: tc)
            let inset = parent.inset.height * 2
            let used = lm.usedRect(for: tc).height + inset
            var n = 0
            lm.enumerateLineFragments(forGlyphRange: lm.glyphRange(for: tc)) {
                _, _, _, _, _ in n += 1
            }
            // The line after a final newline (or of empty text).
            if lm.extraLineFragmentTextContainer != nil { n += 1 }

            let rows = ceil(3 * lm.defaultLineHeight(for: PromptEditor.font)
                            * PromptEditor.latinParagraph.lineHeightMultiple
                            + inset)
            let cap = parent.compact ? rows : max(rows, parent.maxHeight)
            let h = min(max(parent.minHeight, ceil(used)), cap)
            // Switching size keeps the caret in view -- once the box has
            // its new height, or the clip view keeps the first lines.
            let reveal = parent.compact != wasCompact
            wasCompact = parent.compact
            if abs(h - parent.height) > 0.5 {
                DispatchQueue.main.async {
                    withAnimation(.smooth(duration: 0.25)) {
                        self.parent.height = h
                    } completion: {
                        if reveal { tv.scrollRangeToVisible(tv.selectedRange()) }
                    }
                }
            } else if reveal {
                DispatchQueue.main.async {
                    tv.scrollRangeToVisible(tv.selectedRange())
                }
            }
            if n != parent.lines {
                DispatchQueue.main.async {
                    withAnimation(.smooth(duration: 0.2)) {
                        self.parent.lines = n
                    }
                }
            }
            // Compact and cut short: a scroll bar that stays. The legacy
            // style narrows the text; the wrap that follows cannot undo
            // the overflow, so this settles.
            if let scroll = scrollView {
                let bar = parent.compact && used > cap + 0.5
                let style: NSScroller.Style = bar ? .legacy : .overlay
                if scroll.scrollerStyle != style {
                    scroll.scrollerStyle = style
                    scroll.autohidesScrollers = !bar
                }
            }
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator(self) }

    func makeNSView(context: Context) -> NSScrollView {
        // TextKit 1, explicitly: exact used-rect measurement for the
        // growing box, and plain image attachments.
        let storage = NSTextStorage()
        let layout = NSLayoutManager()
        storage.addLayoutManager(layout)
        let container = NSTextContainer(
            size: NSSize(width: 0, height: CGFloat.greatestFiniteMagnitude))
        container.widthTracksTextView = true
        layout.addTextContainer(container)

        let tv = PromptTextView(frame: .zero, textContainer: container)
        tv.delegate = context.coordinator
        tv.isRichText = true           // attachments live in the storage
        tv.importsGraphics = false     // ...but only ours
        tv.allowsUndo = true
        tv.drawsBackground = false
        tv.isVerticallyResizable = true
        tv.isHorizontallyResizable = false
        // Free to grow past the visible rows: that is what scrolls.
        tv.minSize = .zero
        tv.maxSize = NSSize(width: CGFloat.greatestFiniteMagnitude,
                            height: CGFloat.greatestFiniteMagnitude)
        tv.autoresizingMask = [.width]
        tv.textContainerInset = NSSize(width: inset.width, height: 0)
        tv.font = Self.font
        tv.textColor = .labelColor
        tv.insertionPointColor = .controlAccentColor
        tv.typingAttributes = [.font: Self.font,
                               .foregroundColor: NSColor.labelColor,
                               .paragraphStyle: Self.latinParagraph]
        tv.isAutomaticQuoteSubstitutionEnabled = false
        tv.isAutomaticDashSubstitutionEnabled = false
        tv.registerForDraggedTypes([.fileURL, .string])
        tv.onDropTargeted = { t in
            DispatchQueue.main.async { dropTargeted = t }
        }
        tv.onFilesDropped = { urls in
            // The stage's own drag file: the stage captured instead.
            if urls.count == 1, model.isStageDrag(urls[0]) {
                return model.addStageToPrompt()
            }
            return model.addReferences(urls)
        }
        tv.onAssetsDropped = { ids in model.addAssetReferences(ids) }
        tv.onStageDropped = { model.addStageToPrompt() }
        tv.mentionImage = { id in Self.mentionImage(id, model) }
        tv.refTagImage = { kind, index in
            Self.refTagImage(kind, index, model)
        }
        tv.onClick = { model.clearReferenceFocus() }
        // The prompt the toolbar's B / I / U format (PromptBar).
        tv.onFocus = { [weak tv] in model.promptTextView = tv }
        tv.onFocusChange = { [weak coordinator = context.coordinator] in
            guard let coordinator,
                  coordinator.parent.model.promptMarkdown else { return }
            coordinator.styleMarkdown()
            coordinator.measure()
        }
        model.promptTextView = tv
        context.coordinator.textView = tv

        let scroll = PromptScrollView()
        scroll.documentView = tv
        scroll.drawsBackground = false
        scroll.automaticallyAdjustsContentInsets = false
        scroll.contentInsets = NSEdgeInsets(top: inset.height, left: 0,
                                            bottom: inset.height,
                                            right: 0)
        scroll.verticalScroller = KnobScroller()
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.scrollerStyle = .overlay
        scroll.registerForDraggedTypes([.fileURL, .string])
        context.coordinator.scrollView = scroll
        context.coordinator.wasCompact = compact
        return scroll
    }

    func updateNSView(_ scroll: NSScrollView, context: Context) {
        context.coordinator.parent = self
        guard let tv = context.coordinator.textView else { return }
        // A text asset something was made from: read, not written.
        let editable = !model.activeTabReadOnly
        if tv.isEditable != editable { tv.isEditable = editable }
        if context.coordinator.revision != model.promptRevision {
            context.coordinator.revision = model.promptRevision
            replaceText(in: tv, with: model.prompt)
            // Tags became pictures: the model's text follows the view.
            let coordinator = context.coordinator
            DispatchQueue.main.async { coordinator.sync() }
        }
        // The row changed (order, the base, a thumbnail, one taken out):
        // the mentions follow it.
        let row = model.promptAttachments.map {
            "\($0.id)\($0.kind)\($0.isBase)\(model.referenceThumbs[$0.id] != nil)"
        }.joined(separator: ",")
        if row != context.coordinator.drawnFrom {
            context.coordinator.drawnFrom = row
            let staged = Set(model.promptAttachments.map(\.id))
            // A tag's place filled in, emptied or another medium now: its
            // thumbnail drawn again (or none, in red).
            tv.refreshTags()
            if tv.refreshMentions(staged: staged) {
                let coordinator = context.coordinator
                DispatchQueue.main.async { coordinator.sync() }
            }
        }
        // Markdown drawn as it reads, or as typed: switched.
        if context.coordinator.markdownOn != nil,
           context.coordinator.markdownOn != model.promptMarkdown {
            context.coordinator.styleMarkdown()
        }
        context.coordinator.measure()
        if context.coordinator.marked != highlight {
            context.coordinator.mark(highlight, reveal: true)
        }
    }

    /// A positional tag's look: bound to what the row has at its place
    /// (its thumbnail), or not yet.
    @MainActor
    static func refTagImage(_ kind: String, _ index: Int,
                            _ model: AppModel) -> NSImage {
        let item = model.rowItem(kind: kind, index: index)
        return PromptTextView.refTagChip(
            kind: kind, index: index, bound: item != nil,
            thumb: item.flatMap { model.referenceThumbs[$0.id] })
    }

    /// A staged medium as its mention: its thumbnail, its number, the
    /// base's ring.
    @MainActor
    static func mentionImage(_ id: UUID, _ model: AppModel) -> NSImage {
        guard let item = model.promptAttachments.first(where: { $0.id == id })
        else { return PromptTextView.mentionChip(nil, kind: "file",
                                                  number: nil, base: false) }
        return PromptTextView.mentionChip(
            model.referenceThumbs[id], kind: item.kind,
            number: model.referenceNumber(item), base: item.isBase)
    }

    /// The text as the core reads it: a mention a U+FFFC (its medium in
    /// `items`), a tag its text ("<valtz_ref_img_0>").
    @MainActor
    static func marked(_ storage: NSTextStorage) -> (String, [UUID]) {
        var items: [UUID] = []
        var marked = ""
        let all = storage.string as NSString
        storage.enumerateAttribute(
            .attachment, in: NSRange(location: 0, length: storage.length)
        ) { value, range, _ in
            if let a = value as? MentionAttachment {
                items.append(a.item)
                marked += "\u{FFFC}"
            } else if let a = value as? RefTagAttachment {
                marked += a.tag
            } else {
                marked += all.substring(with: range)
            }
        }
        return (marked, items)
    }

    /// Replace the words -- the staged media stay in the row. With the
    /// model's MARKED text to go by (`promptMarked` and its mentions,
    /// agreeing with `prompt`: a tab of immersive editing, the box after
    /// it), each mention is put back where it was, of a medium still in
    /// the row. Else each tag the text names a picture by ("<image2>",
    /// numbered as the core numbers an edit's pictures: as the row has
    /// them) becomes its mention.
    private func replaceText(in tv: PromptTextView, with text: String) {
        guard let storage = tv.textStorage else { return }
        let marked = model.promptMarked
        let mentions = model.promptMentions
        let pieces = marked.components(separatedBy: "\u{FFFC}")
        if !mentions.isEmpty, pieces.count == mentions.count + 1,
           pieces.joined() == text {
            if Self.marked(storage) == (marked, mentions) { return }
            let staged = Set(model.promptAttachments.map(\.id))
            let out = NSMutableAttributedString()
            for (i, piece) in pieces.enumerated() {
                out.append(attributedWords(piece, in: tv))
                guard i < mentions.count, staged.contains(mentions[i]) else {
                    continue
                }
                let id = mentions[i]
                let mention = NSMutableAttributedString(attachment:
                    MentionAttachment(item: id,
                                      image: Self.mentionImage(id, model)))
                mention.addAttributes(tv.typingAttributes,
                                      range: NSRange(location: 0, length: 1))
                out.append(mention)
            }
            storage.setAttributedString(out)
            tv.setSelectedRange(NSRange(location: storage.length, length: 0))
            tv.scrollRangeToVisible(tv.selectedRange())
            return
        }
        let current = storage.string.replacingOccurrences(of: "\u{FFFC}",
                                                          with: "")
        if current == text && !text.isEmpty { return }
        storage.setAttributedString(attributedWords(text, in: tv))
        tv.setSelectedRange(NSRange(location: storage.length, length: 0))
        tv.scrollRangeToVisible(tv.selectedRange())
    }

    /// Words as the text view holds them: the model's own picture tags
    /// ("<image2>") as mentions of those pictures, positional tags as
    /// tags.
    private func attributedWords(_ text: String,
                                 in tv: PromptTextView)
        -> NSMutableAttributedString {
        let words = text as NSString
        var inline: [(NSRange, UUID)] = []
        let format = model.referenceTag
        if !format.isEmpty {
            for n in 1...max(1, model.promptAttachments.count) {
                guard let pic = model.picture(numbered: n) else { continue }
                let tag = format.replacingOccurrences(of: "{n}", with: "\(n)")
                var from = 0
                while from < words.length {
                    let r = words.range(
                        of: tag, range: NSRange(location: from,
                                                length: words.length - from))
                    guard r.location != NSNotFound else { break }
                    if !inline.contains(where: {
                        NSIntersectionRange($0.0, r).length > 0 }) {
                        inline.append((r, pic.id))
                    }
                    from = r.location + r.length
                }
            }
        }
        let out = NSMutableAttributedString()
        var at = 0
        for (r, id) in inline.sorted(by: { $0.0.location < $1.0.location }) {
            out.append(NSAttributedString(
                string: words.substring(with: NSRange(location: at,
                                                      length: r.location - at)),
                attributes: tv.typingAttributes))
            let mention = NSMutableAttributedString(attachment:
                MentionAttachment(item: id,
                                  image: Self.mentionImage(id, model)))
            mention.addAttributes(tv.typingAttributes,
                                  range: NSRange(location: 0, length: 1))
            out.append(mention)
            at = r.location + r.length
        }
        out.append(NSAttributedString(
            string: words.substring(from: at),
            attributes: tv.typingAttributes))
        // Positional tags as tags: what the row has at their places.
        for t in AppModel.refTags(in: out.string).reversed() {
            out.replaceCharacters(in: t.range,
                                  with: tv.tagPiece(kind: t.kind,
                                                    index: t.index))
        }
        return out
    }
}

/// A positional tag's editor, under it while it is selected: its kind
/// from a pop-up (Image, Video, Audio), its place typed or stepped -- as
/// a person counts, 1 the first.
final class RefTagEditor: NSViewController, NSTextFieldDelegate {
    var onChange: ((String, Int) -> Void)?
    private let kinds = ["img", "vid", "aud"]
    private let popup = NSPopUpButton(frame: .zero, pullsDown: false)
    private let field = NSTextField()
    private let stepper = NSStepper()

    override func loadView() {
        popup.addItems(withTitles: kinds.map(RefTagAttachment.kindName))
        popup.target = self
        popup.action = #selector(kindChanged)
        popup.controlSize = .small
        let f = NumberFormatter()
        f.allowsFloats = false
        f.minimum = 1
        f.maximum = 999
        field.formatter = f
        field.alignment = .center
        field.controlSize = .small
        field.font = .monospacedDigitSystemFont(
            ofSize: NSFont.smallSystemFontSize, weight: .regular)
        field.delegate = self
        field.widthAnchor.constraint(equalToConstant: 40).isActive = true
        stepper.minValue = 1
        stepper.maxValue = 999
        stepper.increment = 1
        stepper.valueWraps = false
        stepper.controlSize = .small
        stepper.target = self
        stepper.action = #selector(stepped)
        let stack = NSStackView(views: [popup, field, stepper])
        stack.spacing = 6
        stack.edgeInsets = NSEdgeInsets(top: 8, left: 10, bottom: 8,
                                        right: 10)
        view = stack
    }

    func show(kind: String, index: Int) {
        popup.selectItem(at: kinds.firstIndex(of: kind) ?? 0)
        field.integerValue = index + 1
        stepper.integerValue = index + 1
    }

    private var kind: String {
        kinds[max(0, min(popup.indexOfSelectedItem, kinds.count - 1))]
    }

    @objc private func kindChanged() {
        onChange?(kind, max(1, field.integerValue) - 1)
    }

    @objc private func stepped() {
        field.integerValue = stepper.integerValue
        onChange?(kind, stepper.integerValue - 1)
    }

    func controlTextDidChange(_ obj: Notification) {
        guard let n = Int(field.stringValue), n >= 1 else { return }
        stepper.integerValue = n
        onChange?(kind, n - 1)
    }
}
