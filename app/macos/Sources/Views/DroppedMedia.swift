import AppKit
import UniformTypeIdentifiers

/// MEDIA DRAGGED IN FROM OUTSIDE (DESIGN §7b): a picture out of a web
/// browser, Photos or Mail, an image another app drags as its bytes, a
/// link to a picture, a clip or a sound, a folder from the Finder. The
/// window, Assets, the stage and the prompt read a drag through this, so
/// each takes the same things: the person's FILES as they are, and what
/// is no file yet MADE one -- a promised file received, bytes written, a
/// link downloaded -- in this process's own folder (`root`), imported as
/// a copy and deleted once it is an asset (AppModel.importMade).
///
/// A drag is read off its PASTEBOARD, not SwiftUI's item providers: the
/// provider of a browser's picture offered a type it could not load
/// (public.jpeg, made up from its link), and asking a drop for its
/// providers RECEIVES its promised files -- which AppKit allows only
/// while the drop is performed: asked while the drag still moves, it
/// raises and Valtz aborts. So a drag is told by its types as it moves
/// (`peek`), and taken (`take`) only in a perform. SwiftUI's drops use a
/// DropDelegate for it (an onDrop closure is handed providers, promises
/// received, before it runs).
@MainActor
enum DroppedMedia {
    /// What a drop place registers for: anything -- the pasteboard says
    /// what it is (`peek`), each place's validation what it takes.
    static let dropTypes: [UTType] = {
        var t: [UTType] = [.item, .data, .fileURL, .url, .image,
                           .audiovisualContent, .plainText]
        t += promiseTypes.compactMap { UTType($0.rawValue) }
        return t
    }()

    /// What an AppKit view registers for, to be handed such drags:
    /// AppKit matches the drag's own types, not what they conform to.
    static let pasteboardTypes: [NSPasteboard.PasteboardType] =
        promiseTypes + [.fileURL, .URL, .string, .png, .tiff]
        + [UTType.jpeg, .heic, .heif, .gif, .webP, .bmp, .mpeg4Movie,
           .quickTimeMovie, .mpeg4Audio, .mp3, .wav, .aiff]
            .map { NSPasteboard.PasteboardType($0.identifier) }

    /// Which links count: any web link -- downloaded, its type then
    /// checked -- or only one naming a picture, a clip or a sound by its
    /// extension (the prompt's words: a link to a page dropped there is
    /// typed in as words).
    enum Links { case any, media }

    /// The pasteboard types an item carries a FILE PROMISE by: the
    /// modern provider's and the old NSFilesPromisePboardType.
    static let promiseTypes: [NSPasteboard.PasteboardType] =
        NSFilePromiseReceiver.readableDraggedTypes.map {
            NSPasteboard.PasteboardType($0)
        }

    /// This process's folder for what drops make, a folder a file in it
    /// (each keeps its name): removed at the quit, and -- a crash's --
    /// at the next launch (AppModel.removeOrphanSessions).
    nonisolated static let root = FileManager.default.temporaryDirectory
        .appendingPathComponent("Valtz Drops", isDirectory: true)
        .appendingPathComponent(String(getpid()), isDirectory: true)

    /// What a drag holds, told by its types alone (as it moves).
    struct Peek {
        /// The person's own files (folders too), as they are.
        var files: [URL] = []
        /// Promised files, media as bytes, links to media.
        var made = 0
        /// Words that are none of those.
        var text = false
        var media: Bool { !files.isEmpty || made > 0 }
    }

    /// One item of a drag, as it will be taken.
    private enum Item {
        case file(URL)
        /// The drag's n-th file promise.
        case promise(Int)
        /// The drag's n-th item's bytes of that type.
        case data(Int, NSPasteboard.PasteboardType, UTType, name: String,
                  ext: String)
        case link(URL)
        case text(String)
    }

    static func peek(_ pb: NSPasteboard, links: Links = .any) -> Peek {
        var p = Peek()
        for item in items(pb, links: links) {
            switch item {
            case .file(let u): p.files.append(u)
            case .promise, .data, .link: p.made += 1
            case .text: p.text = true
            }
        }
        return p
    }

    /// A row of Assets in the drag ("valtz-asset:<id>"), and the stage's
    /// own drag ("valtz-stage"): each place takes those itself.
    static func rows(_ pb: NSPasteboard) -> [String] {
        strings(pb).compactMap { AppModel.draggedAsset($0) }
    }

    static func isStage(_ pb: NSPasteboard) -> Bool {
        strings(pb).contains(AppModel.stagePrefix)
    }

    private static func strings(_ pb: NSPasteboard) -> [String] {
        (pb.pasteboardItems ?? []).flatMap { i in
            [i.string(forType: .string), i.string(forType: .URL)]
                .compactMap { $0 }
        }
    }

    /// The drag's items. A browser's picture is one item of several
    /// forms -- a promise, the picture as TIFF, its link, a web archive:
    /// the first of file, promise, bytes, link is what it is.
    private static func items(_ pb: NSPasteboard, links: Links) -> [Item] {
        let promises = Set(promiseTypes)
        var out: [Item] = []
        var promise = 0
        for (index, item) in (pb.pasteboardItems ?? []).enumerated() {
            let types = item.types
            let promised = types.contains { promises.contains($0) }
            defer { if promised { promise += 1 } }
            if let s = item.string(forType: .fileURL),
               let u = URL(string: s), u.isFileURL {
                out.append(.file((u as NSURL).filePathURL ?? u))
                continue
            }
            if promised {
                out.append(.promise(promise))
                continue
            }
            let link = item.string(forType: .URL).flatMap(URL.init(string:))
            if let (t, u) = mediaType(types) {
                out.append(.data(index, t, u,
                                 name: name(of: item, link: link,
                                            kind: kind(u)),
                                 ext: ext(of: u, named: link)))
                continue
            }
            if let link, ["http", "https"].contains(link.scheme ?? ""),
               links == .any || kind(link) != nil {
                out.append(.link(link))
                continue
            }
            if let s = item.string(forType: .string),
               AppModel.draggedAsset(s) == nil, s != AppModel.stagePrefix,
               !s.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                out.append(.text(s))
            }
        }
        return out
    }

    /// The item's best media type: its own format before the TIFF a
    /// browser or an app adds beside it; none a picture, clip or sound
    /// cannot be read from (SVG).
    private static func mediaType(_ types: [NSPasteboard.PasteboardType])
        -> (NSPasteboard.PasteboardType, UTType)? {
        let media = types.compactMap { t -> (NSPasteboard.PasteboardType,
                                             UTType)? in
            guard let u = UTType(t.rawValue), !u.isDynamic,
                  kind(u) != nil, !u.conforms(to: .svg) else { return nil }
            return (t, u)
        }
        return media.first { !$0.1.conforms(to: .tiff) } ?? media.first
    }

    /// "image", "video" or "audio": as the prompt row files a medium.
    nonisolated static func kind(_ t: UTType) -> String? {
        if t.conforms(to: .image) { return "image" }
        if t.conforms(to: .movie) || t.conforms(to: .video) { return "video" }
        if t.conforms(to: .audio) { return "audio" }
        return nil
    }

    nonisolated private static func kind(_ u: URL) -> String? {
        UTType(filenameExtension: u.pathExtension).flatMap { kind($0) }
    }

    /// The extension a file of that type is given: its link's own where
    /// it says that type (.jpg, not the type's .jpeg).
    nonisolated static func ext(of t: UTType, named link: URL?) -> String {
        if let e = link?.pathExtension, !e.isEmpty,
           UTType(filenameExtension: e).map({ $0.conforms(to: t) }) == true {
            return e
        }
        return t.preferredFilenameExtension ?? "data"
    }

    /// A name for bytes dragged in: the item's own, its link's last part,
    /// else what it is.
    private static func name(of item: NSPasteboardItem, link: URL?,
                             kind: String?) -> String {
        let own = item.string(forType: NSPasteboard.PasteboardType(
            "public.url-name"))
            ?? link.map { $0.deletingPathExtension().lastPathComponent }
        if let own = own.map(clean), !own.isEmpty, own != "/" { return own }
        switch kind {
        case "video": return String(localized: "Dropped Clip")
        case "audio": return String(localized: "Dropped Sound")
        default: return String(localized: "Dropped Picture")
        }
    }

    /// A name made a file's: no slashes or colons, not too long.
    nonisolated static func clean(_ s: String) -> String {
        let bad = CharacterSet(charactersIn: "/:\\\0")
            .union(.newlines).union(.controlCharacters)
        let parts = s.components(separatedBy: bad)
        let one = parts.joined(separator: " ")
            .trimmingCharacters(in: .whitespaces)
        return String(one.prefix(120))
    }

    /// A new folder in `root` for one file.
    nonisolated static func newFolder() -> URL? {
        let dir = root.appendingPathComponent(UUID().uuidString,
                                              isDirectory: true)
        do {
            try FileManager.default.createDirectory(
                at: dir, withIntermediateDirectories: true)
            return dir
        } catch {
            return nil
        }
    }

    /// What a PASTE holds of media: a picture's (a clip's, a sound's)
    /// BYTES -- a screenshot copied to the clipboard, a browser's Copy
    /// Image -- with no words beside them but a link. Words copied are
    /// pasted as words: a table copied from Numbers or Word carries a
    /// picture of itself too. Files copied in the Finder are the text
    /// view's own (`fileURLs`).
    private static func pastedItems(_ pb: NSPasteboard) -> [Item] {
        // Named as pasted where they bring no name of their own.
        let fallback = Set(["image", "video", "audio"].map {
            name(of: NSPasteboardItem(), link: nil, kind: $0)
        })
        let bytes = items(pb, links: .media).compactMap { item -> Item? in
            guard case let .data(i, t, u, named, ext) = item else {
                return nil
            }
            guard fallback.contains(named) else { return item }
            let pasted: String = switch kind(u) {
            case "video": String(localized: "Pasted Clip")
            case "audio": String(localized: "Pasted Sound")
            default: String(localized: "Pasted Picture")
            }
            return .data(i, t, u, name: pasted, ext: ext)
        }
        guard !bytes.isEmpty else { return [] }
        for item in pb.pasteboardItems ?? [] {
            guard let s = item.string(forType: .string)?
                    .trimmingCharacters(in: .whitespacesAndNewlines),
                  !s.isEmpty else { continue }
            let link = URL(string: s)
            if link?.scheme == nil || s.contains(where: \.isWhitespace) {
                return []
            }
        }
        return bytes
    }

    /// Whether ⌘V would stage media (Paste offered for them).
    static func pastes(_ pb: NSPasteboard) -> Bool {
        !pastedItems(pb).isEmpty
    }

    /// A paste's picture TAKEN (its bytes written to a file of `root`),
    /// nil when it holds words instead.
    static func pasted(_ pb: NSPasteboard) -> Intake? {
        let list = pastedItems(pb)
        return list.isEmpty ? nil : take(pb, list: list)
    }

    /// The drag TAKEN, in a perform -- AppKit receives a promise only
    /// then: its files, its words, and what it holds that is no file yet,
    /// being made one (`Intake.made`).
    static func take(_ pb: NSPasteboard, links: Links = .any) -> Intake {
        take(pb, list: items(pb, links: links))
    }

    private static func take(_ pb: NSPasteboard, list: [Item]) -> Intake {
        let intake = Intake()
        var receivers: [NSFilePromiseReceiver]?
        for item in list {
            switch item {
            case .file(let u):
                intake.files.append(u)
            case .text(let s):
                intake.text.append(s)
            case .promise(let n):
                if receivers == nil {
                    receivers = pb.readObjects(
                        forClasses: [NSFilePromiseReceiver.self])
                        as? [NSFilePromiseReceiver] ?? []
                }
                guard let r = receivers?[safe: n],
                      let dir = newFolder() else {
                    intake.failed.append(String(localized: "A promised file could not be received."))
                    continue
                }
                intake.receive(r, into: dir)
            case .data(let i, let t, _, let name, let ext):
                // Read now: a source's bytes may be gone with its drag.
                guard let d = pb.pasteboardItems?[safe: i]?
                        .data(forType: t),
                      !d.isEmpty, let dir = newFolder() else { continue }
                let file = dir.appendingPathComponent(name)
                    .appendingPathExtension(ext)
                do {
                    try d.write(to: file)
                    intake.written.append(file)
                } catch {
                    intake.failed.append(error.localizedDescription)
                }
            case .link(let u):
                intake.links.append(u)
            }
        }
        return intake
    }

    /// A drag taken: what came as files and words at once, what is being
    /// made a file.
    @MainActor
    final class Intake {
        var files: [URL] = []
        var text: [String] = []
        fileprivate var written: [URL] = []
        fileprivate var links: [URL] = []
        /// Each promise's files as they come (nil: one that failed), and
        /// how many are owed.
        private var promised: [AsyncStream<URL?>] = []
        /// What could not be made, in words.
        fileprivate(set) var failed: [String] = []

        /// Anything to make a file of.
        var makes: Bool {
            !written.isEmpty || !links.isEmpty || !promised.isEmpty
        }

        fileprivate func receive(_ r: NSFilePromiseReceiver, into dir: URL) {
            let owed = max(1, r.fileTypes.count)
            let (stream, c) = AsyncStream.makeStream(of: URL?.self)
            promised.append(stream)
            let queue = OperationQueue()
            queue.qualityOfService = .userInitiated
            let count = Counter(owed)
            // @Sendable: called on `queue`, not the main thread. Written
            // here plainly it took Intake's main-actor isolation, and
            // Swift 6's check at its entry trapped -- a browser's picture
            // dropped on the prompt's row crashed Valtz (Valtz-122). AppKit
            // marks the reader no Sendable block, so nothing warned.
            r.receivePromisedFiles(atDestination: dir, options: [:],
                                   operationQueue: queue) {
                @Sendable url, error in
                c.yield(error == nil ? url : nil)
                if count.done() { c.finish() }
            }
        }

        /// The files made: promises received, bytes written, links
        /// downloaded -- each a picture, a clip or a sound (others
        /// deleted, said in `failed`). A promise not kept in a minute is
        /// given up.
        func made() async -> [URL] {
            var out = written
            for stream in promised {
                let got = await Self.collect(stream, seconds: 60)
                for u in got {
                    if let u, Self.isMedia(u) {
                        out.append(u)
                    } else {
                        if let u { DroppedMedia.discard(u) }
                        failed.append(u.map {
                            String(localized: "“\($0.lastPathComponent)” is not a picture, a clip or a sound.")
                        } ?? String(localized: "A promised file did not come."))
                    }
                }
                if got.isEmpty {
                    failed.append(String(localized: "A promised file did not come."))
                }
            }
            for link in links {
                switch await DroppedMedia.download(link) {
                case .success(let u): out.append(u)
                case .failure(let e): failed.append(e.text)
                }
            }
            return out
        }

        private static func collect(_ s: AsyncStream<URL?>,
                                    seconds: Double) async -> [URL?] {
            await withTaskGroup(of: [URL?]?.self) { g in
                g.addTask {
                    var got: [URL?] = []
                    for await u in s { got.append(u) }
                    return got
                }
                g.addTask {
                    try? await Task.sleep(for: .seconds(seconds))
                    return nil
                }
                let first = await g.next() ?? nil
                g.cancelAll()
                return first ?? []
            }
        }

        nonisolated static func isMedia(_ u: URL) -> Bool {
            var dir: ObjCBool = false
            guard FileManager.default.fileExists(atPath: u.path,
                                                 isDirectory: &dir),
                  !dir.boolValue,
                  let t = UTType(filenameExtension: u.pathExtension)
            else { return false }
            return t.conforms(to: .image) || t.conforms(to: .audio)
                || t.conforms(to: .movie) || t.conforms(to: .video)
        }
    }

    /// A made file gone, with the folder it was made in.
    nonisolated static func discard(_ u: URL) {
        let dir = u.deletingLastPathComponent()
        let fm = FileManager.default
        if dir.deletingLastPathComponent().standardizedFileURL
            == root.standardizedFileURL {
            try? fm.removeItem(at: dir)
        } else {
            try? fm.removeItem(at: u)
        }
    }

    struct Failure: Error { let text: String }

    /// A link dropped, fetched -- no cookies, nothing kept -- into a
    /// folder of `root`: taken when the server says (or its name says) it
    /// is a picture, a clip or a sound.
    nonisolated static func download(_ url: URL) async
        -> Result<URL, Failure> {
        let host = url.host() ?? url.absoluteString
        let cfg = URLSessionConfiguration.ephemeral
        cfg.timeoutIntervalForRequest = 30
        cfg.timeoutIntervalForResource = 600
        let session = URLSession(configuration: cfg)
        defer { session.finishTasksAndInvalidate() }
        let tmp: URL
        let response: URLResponse
        do {
            (tmp, response) = try await session.download(from: url)
        } catch {
            return .failure(Failure(text: String(localized: "Could not download from \(host): \(error.localizedDescription)")))
        }
        let fm = FileManager.default
        if let h = response as? HTTPURLResponse,
           !(200..<300).contains(h.statusCode) {
            try? fm.removeItem(at: tmp)
            return .failure(Failure(text: String(localized: "Could not download from \(host): \(HTTPURLResponse.localizedString(forStatusCode: h.statusCode))")))
        }
        let byMime = response.mimeType.flatMap { UTType(mimeType: $0) }
        let byName = UTType(filenameExtension: url.pathExtension)
        guard let t = [byMime, byName].compactMap({ $0 }).first(where: {
            kind($0) != nil && !$0.conforms(to: .svg)
        }) else {
            try? fm.removeItem(at: tmp)
            return .failure(Failure(text: String(localized: "What \(host) sent is not a picture, a clip or a sound.")))
        }
        let suggested = response.suggestedFilename.map {
            URL(fileURLWithPath: $0).deletingPathExtension()
                .lastPathComponent
        } ?? url.deletingPathExtension().lastPathComponent
        var base = clean(suggested)
        if base.isEmpty { base = clean(host) }
        guard let dir = newFolder() else {
            try? fm.removeItem(at: tmp)
            return .failure(Failure(text: String(localized: "Could not download from \(host).")))
        }
        // The name's own extension where it says what was sent (.jpg,
        // not the type's .jpeg).
        let named = [response.suggestedFilename.map {
            URL(fileURLWithPath: $0).pathExtension
        }, url.pathExtension].compactMap { $0 }.first { e in
            UTType(filenameExtension: e).map { $0.conforms(to: t) } == true
        }
        let ext = named ?? t.preferredFilenameExtension ?? url.pathExtension
        let file = dir.appendingPathComponent(base)
            .appendingPathExtension(ext)
        do {
            try fm.moveItem(at: tmp, to: file)
        } catch {
            try? fm.removeItem(at: tmp)
            try? fm.removeItem(at: dir)
            return .failure(Failure(text: error.localizedDescription))
        }
        return .success(file)
    }

    /// A folder dropped: the pictures, clips and sounds in it and in its
    /// folders (packages and hidden files left out), at most `limit`.
    nonisolated static func media(in folder: URL, limit: Int = 1000)
        -> [URL] {
        guard let e = FileManager.default.enumerator(
            at: folder, includingPropertiesForKeys: [.isRegularFileKey],
            options: [.skipsHiddenFiles, .skipsPackageDescendants])
        else { return [] }
        var out: [URL] = []
        for case let u as URL in e {
            if Intake.isMedia(u) { out.append(u) }
            if out.count >= limit { break }
        }
        return out.sorted { $0.path.localizedStandardCompare($1.path)
                                == .orderedAscending }
    }
}

/// How many of a promise's files are still owed: the last one in ends
/// its stream. (Its reader is called on a queue of its own.)
private final class Counter: @unchecked Sendable {
    private let lock = NSLock()
    private var left: Int
    init(_ n: Int) { left = n }
    func done() -> Bool {
        lock.lock()
        defer { lock.unlock() }
        left -= 1
        return left <= 0
    }
}

private extension Array {
    subscript(safe i: Int) -> Element? {
        indices.contains(i) ? self[i] : nil
    }
}

/// The pasteboard of the drag over an editor window NOW, as AppKit hands
/// it to the window's SwiftUI drop view (`watch`). SwiftUI's drop
/// delegates are given item providers, not the drag (DroppedMedia reads
/// a pasteboard), and the system's named drag pasteboard is the drag's
/// for most drags but not all: a DOCK STACK's -- the Downloads folder in
/// the Dock -- drags on a pasteboard of its own, so a drop read the named
/// one took the files of the drag before it (two files last moved in the
/// Finder, nowhere near the one dragged). So the window's drop view is
/// watched: each drag's own pasteboard noted as it enters, moves, is
/// prepared and performed, let go as it leaves or concludes.
@MainActor
enum DragBoard {
    private static var current: NSPasteboard?
    private static var hooked = Set<ObjectIdentifier>()

    /// The drag's own pasteboard while one is over a watched view; else
    /// the named drag pasteboard, as before.
    static var pasteboard: NSPasteboard {
        current ?? NSPasteboard(name: .drag)
    }

    /// The window's SwiftUI drop views watched -- their classes, once.
    /// Made as the window's content is laid out, they may come after the
    /// window: looked for again a few times.
    static func watch(_ window: NSWindow, tries: Int = 6) {
        guard let root = window.contentView?.superview
                ?? window.contentView else { return }
        var found = false
        func walk(_ v: NSView) {
            // SwiftUI's own (its _PlatformDraggingDestinationView); an
            // AppKit view of Valtz's reads the drag it is handed.
            let name = NSStringFromClass(type(of: v))
            if !v.registeredDraggedTypes.isEmpty, name.contains("SwiftUI"),
               let cls = object_getClass(v) {
                hook(cls)
                found = true
            }
            v.subviews.forEach(walk)
        }
        walk(root)
        guard !found, tries > 0 else { return }
        Task { @MainActor [weak window] in
            try? await Task.sleep(for: .milliseconds(500))
            if let window { watch(window, tries: tries - 1) }
        }
    }

    private typealias OpFn = @convention(c) (NSObject, Selector,
                                             NSDraggingInfo) -> NSDragOperation
    private typealias BoolFn = @convention(c) (NSObject, Selector,
                                               NSDraggingInfo) -> Bool
    private typealias EndFn = @convention(c) (NSObject, Selector,
                                              NSDraggingInfo?) -> Void

    private static func hook(_ cls: AnyClass) {
        guard hooked.insert(ObjectIdentifier(cls)).inserted else { return }
        // AppKit calls a drag destination on the main thread.
        let note: @Sendable (NSDraggingInfo) -> Void = { info in
            nonisolated(unsafe) let info = info
            MainActor.assumeIsolated { current = info.draggingPasteboard }
        }
        let forget: @Sendable () -> Void = {
            MainActor.assumeIsolated { current = nil }
        }
        for sel in [#selector(NSDraggingDestination.draggingEntered(_:)),
                    #selector(NSDraggingDestination.draggingUpdated(_:))] {
            replace(cls, sel) { orig in
                let f = unsafeBitCast(orig, to: OpFn.self)
                let b: @convention(block) (NSObject, NSDraggingInfo)
                    -> NSDragOperation = { me, info in
                        note(info)
                        return f(me, sel, info)
                    }
                return b
            }
        }
        for sel in [
            #selector(NSDraggingDestination.prepareForDragOperation(_:)),
            #selector(NSDraggingDestination.performDragOperation(_:)),
        ] {
            replace(cls, sel) { orig in
                let f = unsafeBitCast(orig, to: BoolFn.self)
                let b: @convention(block) (NSObject, NSDraggingInfo)
                    -> Bool = { me, info in
                        note(info)
                        return f(me, sel, info)
                    }
                return b
            }
        }
        // Gone or done: the named pasteboard again (a drop elsewhere,
        // not watched, reads that).
        for sel in [#selector(NSDraggingDestination.draggingExited(_:)),
                    #selector(NSDraggingDestination.concludeDragOperation(_:))] {
            let had = class_getInstanceMethod(cls, sel)
            let orig = had.map(method_getImplementation)
            let b: @convention(block) (NSObject, NSDraggingInfo?) -> Void = {
                me, info in
                if let orig {
                    unsafeBitCast(orig, to: EndFn.self)(me, sel, info)
                }
                forget()
            }
            let imp = imp_implementationWithBlock(b)
            if let had {
                if !class_addMethod(cls, sel, imp,
                                    method_getTypeEncoding(had)) {
                    method_setImplementation(had, imp)
                }
            } else {
                class_addMethod(cls, sel, imp, "v@:@")
            }
        }
    }

    /// `sel` of `cls` (its own or inherited) replaced on `cls` by what
    /// `make` builds around the original.
    private static func replace(_ cls: AnyClass, _ sel: Selector,
                                _ make: (IMP) -> Any) {
        guard let m = class_getInstanceMethod(cls, sel) else { return }
        let imp = imp_implementationWithBlock(make(method_getImplementation(m)))
        if !class_addMethod(cls, sel, imp, method_getTypeEncoding(m)) {
            method_setImplementation(m, imp)
        }
    }
}

/// A RefMod (DESIGN §4s): MiniMax H3 reference latents in a .safetensors
/// -- told by its header, `refmod_meta` (or `audio_refmod_meta`) in its
/// metadata beside its `latent` or `ref_<i>` tensors, as the core tells it
/// (media/refmod.h): a .safetensors is weights as often.
enum RefModFile {
    /// What a RefMod is named as in the prompt -- its first picture's or
    /// clip's kind, else "audio"; nil when the file is no RefMod.
    nonisolated static func namedAs(_ url: URL) -> String? {
        guard url.isFileURL,
              url.pathExtension.lowercased() == "safetensors",
              let h = header(url),
              let meta = h["__metadata__"] as? [String: Any],
              let text = (meta["refmod_meta"] ?? meta["audio_refmod_meta"])
                  as? String,
              let m = try? JSONSerialization.jsonObject(
                  with: Data(text.utf8)) as? [String: Any]
        else { return nil }
        if m["kind"] as? String == "bundle" {
            guard h["ref_0"] != nil,
                  let members = m["members"] as? [[String: Any]],
                  !members.isEmpty else { return nil }
            let kinds = members.compactMap { $0["kind"] as? String }
            return kinds.first { $0 != "audio" }
                ?? (kinds.contains("audio") ? "audio" : nil)
        }
        guard h["latent"] != nil,
              let kind = m["kind"] as? String,
              ["image", "video", "audio"].contains(kind) else { return nil }
        return kind
    }

    /// The safetensors header: its tensors and its metadata.
    nonisolated private static func header(_ url: URL) -> [String: Any]? {
        guard let f = try? FileHandle(forReadingFrom: url) else { return nil }
        defer { try? f.close() }
        guard let head = try? f.read(upToCount: 8), head.count == 8
        else { return nil }
        let n = head.withUnsafeBytes { $0.loadUnaligned(as: UInt64.self) }
        // A RefMod's header is small; a checkpoint's may be megabytes.
        guard n > 0, n < 16 << 20,
              let text = try? f.read(upToCount: Int(n)),
              text.count == Int(n) else { return nil }
        return try? JSONSerialization.jsonObject(with: text)
            as? [String: Any]
    }
}

