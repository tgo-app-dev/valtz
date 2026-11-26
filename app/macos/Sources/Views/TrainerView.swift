import AppKit
import AVKit
import SwiftUI
import UniformTypeIdentifiers

/// The LoRA TRAINER (Settings › Experimental; DESIGN §8c), the ☰
/// column's above the Log -- no project's: its title bar is its own
/// (TrainerTitleField, TrainerTitleControls). Laid out as the editor is:
///
///   ┌ stage ──────────────────────────────┐ ┌ island ─────┐
///   │ the sample selected, or a preview   │ │ Settings or │
///   │                                     │ │ Samples     │
///   └─────────────────────────────────────┘ │             │
///   ┌ progress island (the prompt's place)┐ │             │
///   └ phase · steps · loss curve · previews┘ └──────tabs⌄─┘
///
/// The stage where the editor's is, the progress where its prompt box
/// or timeline is, and the configuration (its Settings tab) and the
/// pictures trained on (Samples: pictures and folders dropped anywhere
/// on the screen) in the inspector's place, its tabs at its bottom
/// right as the tray's cards have theirs. The progress island's top
/// edge is dragged for its height, the settings island's left edge for
/// its width.
struct TrainerView: View {
    @Bindable var model: AppModel
    @Bindable var trainer: TrainerModel
    @State private var dropping = false

    /// Between the islands, and between them and the window's edges.
    static let gap: CGFloat = 22

    var body: some View {
        HStack(alignment: .top, spacing: Self.gap) {
            VStack(spacing: Self.gap) {
                TrainerStage(trainer: trainer)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                TrainerProgress(trainer: trainer)
                    .frame(height: trainer.progressHeight)
                    .background(RoundedRectangle(cornerRadius: 18,
                                                 style: .continuous)
                        .fill(Color.valtzPanel)
                        .shadow(color: .black.opacity(0.09), radius: 7,
                                y: 3))
                    // Its top edge, dragged: its height.
                    .overlay(alignment: .top) {
                        EdgeHandle(vertical: true) { d in
                            trainer.progressHeight = clamp(
                                trainer.progressHeight - d,
                                TrainerModel.heightRange)
                        }
                        .frame(height: 10)
                        .offset(y: -5)
                    }
            }
            TabbedIsland(selected: $trainer.tab) {
                switch trainer.tab {
                case .settings: TrainerConfig(trainer: trainer)
                case .samples: TrainerSamples(trainer: trainer)
                }
            }
            .frame(width: trainer.islandWidth)
            // Its left edge, dragged: its width.
            .overlay(alignment: .leading) {
                EdgeHandle(vertical: false) { d in
                    trainer.islandWidth = clamp(trainer.islandWidth - d,
                                                TrainerModel.widthRange)
                }
                .frame(width: 10)
                .offset(x: -5)
            }
        }
        .padding(Self.gap)
        .overlay {
            if dropping {
                RoundedRectangle(cornerRadius: 18)
                    .strokeBorder(Color.accentColor, lineWidth: 3)
                    .padding(6)
                    .allowsHitTesting(false)
            }
        }
        .onDrop(of: [.fileURL], isTargeted: $dropping) { providers in
            takeDrop(providers)
            return true
        }
        .onAppear { trainer.reload() }
        .sheet(isPresented: $trainer.askingMeaning) {
            MeaningSheet(trainer: trainer)
        }
        // The meaning given or changed: what still lacks a caption.
        .onChange(of: trainer.triggerMeaning) { _, _ in
            trainer.captionMissing(asked: false)
        }
    }

    private func clamp(_ v: Double, _ r: ClosedRange<Double>) -> Double {
        min(max(v, r.lowerBound), r.upperBound)
    }

    private func takeDrop(_ providers: [NSItemProvider]) {
        for p in providers {
            _ = p.loadObject(ofClass: URL.self) { url, _ in
                guard let url else { return }
                Task { @MainActor in
                    if !trainer.training, trainer.add([url]) > 0 {
                        trainer.tab = .samples
                    }
                }
            }
        }
    }
}

/// An island's edge as a handle: the resize cursor over it, a drag
/// handing on how far it went since the last (points; right and down
/// positive). An AppKit view: it hears the pointer whatever SwiftUI's
/// gestures do, and the hooks' posted drags.
/// The question auto captions ask first: what the trigger stands for.
private struct MeaningSheet: View {
    @Bindable var trainer: TrainerModel
    @State private var text = ""

    var body: some View {
        let t = trainer.trigger.isEmpty ? "ohwx" : trainer.trigger
        VStack(alignment: .leading, spacing: 12) {
            Text("What does \u{201C}\(t)\u{201D} mean?")
                .font(.headline)
            Text("\(String(trainer.uncaptioned.count)) samples have no caption. Say what \(t) stands for, as you would tell someone -- the agent's model then captions each one, calling it \(t) and describing everything else.")
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            TextField("", text: $text,
                      prompt: Text("e.g. a small felt teapot robot with an orange button face"),
                      axis: .vertical)
                .lineLimit(3...5)
                .textFieldStyle(.roundedBorder)
            HStack {
                Spacer()
                Button("Not Now") { trainer.putOffMeaning() }
                    .keyboardShortcut(.cancelAction)
                Button("Caption Them") { trainer.answerMeaning(text) }
                    .keyboardShortcut(.defaultAction)
                    .disabled(text.trimmingCharacters(in: .whitespaces)
                                  .isEmpty)
            }
        }
        .padding(20)
        .frame(width: 420)
        .onAppear { text = trainer.triggerMeaning }
    }
}

private struct EdgeHandle: NSViewRepresentable {
    let vertical: Bool
    let moved: (CGFloat) -> Void

    func makeNSView(context: Context) -> HandleView {
        let v = HandleView()
        v.vertical = vertical
        v.moved = moved
        return v
    }

    func updateNSView(_ v: HandleView, context: Context) {
        v.vertical = vertical
        v.moved = moved
    }

    final class HandleView: NSView {
        var vertical = false
        var moved: (CGFloat) -> Void = { _ in }
        private var last: NSPoint?

        override func resetCursorRects() {
            addCursorRect(bounds, cursor: vertical ? .resizeUpDown
                                                   : .resizeLeftRight)
        }

        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        override func mouseDown(with e: NSEvent) {
            last = e.locationInWindow
            if ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] != nil {
                FileHandle.standardError.write(Data(
                    "snapshot: trainer-edge down \(vertical ? "top" : "left")\n".utf8))
            }
        }

        override func mouseDragged(with e: NSEvent) {
            guard let last else { return }
            let p = e.locationInWindow
            // Window points go up: down positive.
            moved(vertical ? last.y - p.y : p.x - last.x)
            self.last = p
        }

        override func mouseUp(with e: NSEvent) { last = nil }
    }
}

enum TrainerTab: CaseIterable, Identifiable {
    case settings, samples
    var id: Self { self }
    var symbol: String {
        self == .settings ? "slider.horizontal.3" : "photo.stack"
    }
    var title: LocalizedStringKey {
        self == .settings ? "Settings" : "Samples"
    }
}

// MARK: The tabbed island

/// A floating island whose tabs hang under its bottom right, as the
/// tray's cards' headers do: the one chosen joined to the island by
/// concave corners, the others behind it.
private struct TabbedIsland<Content: View>: View {
    @Binding var selected: TrainerTab
    @ViewBuilder let content: () -> Content

    private let tabHeight: CGFloat = 37
    private let tabWidth: CGFloat = 112
    private let radius: CGFloat = 18
    private let fillet: CGFloat = 12

    var body: some View {
        GeometryReader { g in
            let w = g.size.width, h = g.size.height
            let tabs = TrainerTab.allCases
            ZStack(alignment: .topLeading) {
                // Behind: the tabs not chosen.
                ForEach(Array(tabs.enumerated()), id: \.element) { i, t in
                    if t != selected {
                        IslandShape(body: false, tab: tabRect(i, w, h),
                                    radius: radius, fillet: fillet,
                                    rowTop: h - tabHeight)
                            .fill(Color.valtzPanel.opacity(0.75))
                            .shadow(color: .black.opacity(0.06), radius: 5,
                                    y: 2)
                    }
                }
                if let i = tabs.firstIndex(of: selected) {
                    IslandShape(body: true, tab: tabRect(i, w, h),
                                radius: radius, fillet: fillet,
                                rowTop: h - tabHeight)
                        .fill(Color.valtzPanel)
                        .shadow(color: .black.opacity(0.09), radius: 7, y: 3)
                }
                content()
                    .frame(width: w, height: h - tabHeight)
                    .clipShape(UnevenRoundedRectangle(
                        topLeadingRadius: radius, bottomLeadingRadius: radius,
                        bottomTrailingRadius: radius,
                        topTrailingRadius: radius, style: .continuous))
                ForEach(Array(tabs.enumerated()), id: \.element) { i, t in
                    let r = tabRect(i, w, h)
                    Button {
                        withAnimation(AppModel.motion) { selected = t }
                    } label: {
                        HStack(spacing: 8) {
                            Image(systemName: t.symbol)
                                .frame(width: 20)
                            Text(t.title)
                        }
                        .font(.callout)
                        .foregroundStyle(t == selected ? .primary
                                                       : .secondary)
                        .frame(width: r.width, height: r.height)
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .offset(x: r.minX, y: r.minY)
                }
            }
        }
    }

    /// Tab i, the last at the right edge.
    private func tabRect(_ i: Int, _ w: CGFloat, _ h: CGFloat) -> CGRect {
        let n = TrainerTab.allCases.count
        let x = w - CGFloat(n - i) * tabWidth
        return CGRect(x: x, y: h - tabHeight, width: tabWidth,
                      height: tabHeight)
    }
}

/// The island and its tab as one outline: the body down to the tab row,
/// rounded at every corner, the tab under it joined by concave fillets
/// -- or the tab alone.
private struct IslandShape: Shape {
    var body: Bool
    var tab: CGRect
    var radius: CGFloat
    var fillet: CGFloat
    var rowTop: CGFloat

    func path(in rect: CGRect) -> Path {
        guard body else {
            return Path(roundedRect: CGRect(x: tab.minX + 3, y: tab.minY - radius,
                                            width: tab.width - 6,
                                            height: tab.height + radius - 3),
                        cornerRadius: radius, style: .continuous)
        }
        var c: [(CGPoint, CGFloat)] = [
            (CGPoint(x: rect.minX, y: rect.minY), radius),
            (CGPoint(x: rect.maxX, y: rect.minY), radius),
        ]
        let right = tab.maxX >= rect.maxX - 0.5
        if right {
            c.append((CGPoint(x: rect.maxX, y: tab.maxY), radius))
        } else {
            c += [(CGPoint(x: rect.maxX, y: rowTop), radius),
                  (CGPoint(x: tab.maxX, y: rowTop), fillet),
                  (CGPoint(x: tab.maxX, y: tab.maxY), radius)]
        }
        c += [(CGPoint(x: tab.minX, y: tab.maxY), radius),
              (CGPoint(x: tab.minX, y: rowTop), fillet),
              (CGPoint(x: rect.minX, y: rowTop), radius)]
        var p = Path()
        let first = c[0].0, last = c[c.count - 1].0
        p.move(to: CGPoint(x: (first.x + last.x) / 2,
                           y: (first.y + last.y) / 2))
        for (i, (pt, r)) in c.enumerated() {
            p.addArc(tangent1End: pt, tangent2End: c[(i + 1) % c.count].0,
                     radius: r)
        }
        p.closeSubpath()
        return p
    }
}

// MARK: The samples

/// The Samples tab: how many, then each picture -- its thumbnail, name,
/// caption -- one clicked shown on the stage.
private struct TrainerSamples: View {
    @Bindable var trainer: TrainerModel
    @State private var picked: Set<TrainerModel.Item.ID> = []

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text(trainer.items.isEmpty
                     ? String(localized: "No samples")
                     : trainer.query.isEmpty
                       ? String(localized: "\(String(trainer.items.count)) samples")
                       : String(localized: "\(String(trainer.shownItems.count)) of \(String(trainer.items.count)) samples"))
                    .font(.headline)
                Text(captioned + clipsCount)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if trainer.captioning {
                    ProgressView().controlSize(.mini)
                    Text("Captioning \(String(trainer.captionQueue.count + 1))")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .help("The agent's model captions the samples without one")
                }
                Spacer()
                Button {
                    choose()
                } label: {
                    Image(systemName: "plus")
                }
                .help("Add pictures, or folders of them; a .txt beside a picture is its caption")
                Button {
                    trainer.remove(picked)
                } label: {
                    Image(systemName: "minus")
                }
                .disabled(picked.isEmpty)
                .help("Remove the selected pictures")
            }
            .buttonStyle(.borderless)
            .disabled(trainer.training)
            .padding(.horizontal, 14)
            .frame(height: 40)
            Divider()
            if trainer.items.isEmpty {
                VStack(spacing: 10) {
                    Image(systemName: "photo.stack")
                        .font(.system(size: 34))
                        .foregroundStyle(.tertiary)
                    Text(trainer.takesClips
                         ? "Drop pictures, clips or folders here"
                         : "Drop pictures or folders here")
                        .font(.callout.weight(.semibold))
                    Text(trainer.referenceKind != nil
                         ? "A folder's samples are taken, in its subfolders too, but a folder named references. A .txt beside a sample is its caption, a .refs its references: a file a line."
                         : "A folder's pictures are taken, in its subfolders too. A .txt beside a picture, named as it is, is its caption.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.center)
                }
                .padding(20)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List(selection: $picked) {
                    ForEach(trainer.shownItems) { item in
                        TrainerRow(item: item,
                                   captioning: trainer.captioningNow == item.id)
                            .tag(item.id)
                            .contextMenu {
                                Button("Show in Finder") {
                                    NSWorkspace.shared
                                        .activateFileViewerSelecting([item.url])
                                }
                                Button("Remove") {
                                    trainer.remove(picked.contains(item.id)
                                                   ? picked : [item.id])
                                }
                            }
                    }
                }
                .scrollContentBackground(.hidden)
                .onChange(of: picked) { _, now in
                    if now.count == 1, let id = now.first {
                        trainer.showSample(id)
                    }
                }
                .onDeleteCommand { trainer.remove(picked) }
            }
        }
    }

    private var clipsCount: String {
        let n = trainer.items.filter(\.isClip).count
        return n == 0 ? "" : " · " + String(localized: "\(String(n)) clips")
    }

    private var captioned: String {
        let n = trainer.items.filter { !$0.caption.isEmpty }.count
        return trainer.items.isEmpty ? ""
            : String(localized: "\(String(n)) captioned")
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = trainer.takesClips
            ? [.image, .movie, .folder] : [.image, .folder]
        panel.prompt = String(localized: "Add")
        guard panel.runModal() == .OK else { return }
        trainer.add(panel.urls)
    }
}

/// A picture's row: its thumbnail, its name, its caption's start.
private struct TrainerRow: View {
    let item: TrainerModel.Item
    var captioning = false
    @State private var thumb: NSImage?

    var body: some View {
        HStack(spacing: 8) {
            Thumb(image: thumb)
                .frame(width: 40, height: 40)
                .overlay(alignment: .bottomLeading) {
                    if item.isClip {
                        Image(systemName: "film")
                            .font(.system(size: 9, weight: .semibold))
                            .foregroundStyle(.white)
                            .padding(2)
                            .background(.black.opacity(0.55),
                                        in: RoundedRectangle(cornerRadius: 3))
                            .padding(2)
                    }
                }
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: item.name)
                    .lineLimit(1)
                    .truncationMode(.middle)
                HStack(spacing: 4) {
                    if captioning {
                        ProgressView().controlSize(.mini)
                    } else if !item.captionedBy.isEmpty {
                        Image(systemName: "sparkles")
                            .font(.system(size: 9))
                            .foregroundStyle(.secondary)
                            .help(String(localized: "Captioned by \(item.captionedBy)"))
                    }
                    Text(captioning ? String(localized: "Captioning…")
                         : item.caption.isEmpty
                         ? String(localized: "No caption")
                         : item.caption)
                        .font(.caption)
                        .foregroundStyle(item.caption.isEmpty ? .tertiary
                                                              : .secondary)
                        .lineLimit(1)
                }
            }
            if !item.references.isEmpty {
                Spacer(minLength: 4)
                Label(String(item.references.count), systemImage: "link")
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .help("Its references")
            }
        }
        .task(id: item.url) {
            thumb = await TrainerThumbs.image(item.url, 96)
        }
    }
}

private struct Thumb: View {
    let image: NSImage?
    var body: some View {
        // The square the caller frames, the picture filling it and cut
        // there (a wide clip's frame overflowed its row).
        RoundedRectangle(cornerRadius: 4)
            .fill(Color.primary.opacity(0.06))
            .overlay {
                if let image {
                    Image(nsImage: image).resizable()
                        .aspectRatio(contentMode: .fill)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 4))
    }
}

enum TrainerThumbs {
    /// A picture made small off the main thread (ImageIO's thumbnail).
    static func image(_ url: URL, _ px: Int) async -> NSImage? {
        let cg = await Task.detached(priority: .utility) {
            await Self.cg(url, px)
        }.value
        return cg.map { NSImage(cgImage: $0, size: .zero) }
    }

    nonisolated static func cg(_ url: URL, _ px: Int) async -> CGImage? {
        if TrainerModel.clipTypes.contains(url.pathExtension.lowercased()) {
            // A clip: its first frame, upright.
            let gen = AVAssetImageGenerator(asset: AVURLAsset(url: url))
            gen.appliesPreferredTrackTransform = true
            gen.maximumSize = CGSize(width: px, height: px)
            return try? await gen.image(at: .zero).image
        }
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return nil
        }
        let opts: [CFString: Any] = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: px,
            kCGImageSourceCreateThumbnailWithTransform: true,
        ]
        return CGImageSourceCreateThumbnailAtIndex(src, 0, opts as CFDictionary)
    }
}

// MARK: The stage

/// The top right, as the editor's stage: the sample selected -- its
/// caption under it, edited, and what the model reads -- or a preview,
/// as the title bar's Sample | Preview says (a sample clicked, a preview
/// clicked: it follows). Fitted, or at the title bar's zoom, scrolled.
private struct TrainerStage: View {
    @Bindable var trainer: TrainerModel
    @State private var image: NSImage?

    var body: some View {
        VStack(spacing: 10) {
            GeometryReader { g in
                picture(in: g.size)
                    .frame(width: g.size.width, height: g.size.height)
                    .onChange(of: g.size) { _, sz in fit(sz) }
                    .onChange(of: image?.size) { _, _ in fit(g.size) }
                    .onAppear { fit(g.size) }
            }
            caption
        }
        .task(id: trainer.stagePath) {
            guard let p = trainer.stagePath else {
                image = nil
                return
            }
            image = await TrainerThumbs.image(URL(fileURLWithPath: p), 4096)
        }
    }

    private var stageIsClip: Bool {
        trainer.stagePath.map {
            TrainerModel.clipTypes.contains(
                URL(fileURLWithPath: $0).pathExtension.lowercased())
        } ?? false
    }

    @ViewBuilder
    private func picture(in room: CGSize) -> some View {
        if stageIsClip, let p = trainer.stagePath, let image {
            // A clip -- a sample's, a preview's -- played on a loop, fitted.
            let k = min(room.width / max(image.size.width, 1),
                        room.height / max(image.size.height, 1))
            TrainerClip(url: URL(fileURLWithPath: p))
                .frame(width: image.size.width * k,
                       height: image.size.height * k)
                .clipShape(RoundedRectangle(cornerRadius: 10,
                                            style: .continuous))
                .shadow(color: .black.opacity(0.18), radius: 10, y: 4)
        } else if let image {
            let k = trainer.stageScale
            let w = image.size.width * k, h = image.size.height * k
            let card = Image(nsImage: image)
                .resizable()
                .interpolation(k >= 2 ? .none : .high)
                .frame(width: w, height: h)
                .clipShape(RoundedRectangle(cornerRadius: 10,
                                            style: .continuous))
                .shadow(color: .black.opacity(0.18), radius: 10, y: 4)
            if trainer.stageZoom == nil {
                card
            } else {
                ScrollView([.horizontal, .vertical]) {
                    card
                        .padding(12)
                        .frame(minWidth: room.width, minHeight: room.height)
                }
                .scrollIndicators(.automatic)
            }
        } else {
            VStack(spacing: 8) {
                Image(systemName: trainer.stageShows == .sample
                      ? "photo" : "sparkles")
                    .font(.system(size: 30))
                    .foregroundStyle(.tertiary)
                Text(trainer.stageShows == .sample
                     ? "Select a sample in Samples"
                     : "Previews come at each checkpoint")
                    .foregroundStyle(.secondary)
            }
        }
    }

    /// The scale that fits the picture to the room (at most 4x).
    private func fit(_ room: CGSize) {
        guard let s = image?.size, s.width > 0, s.height > 0 else { return }
        let k = min(room.width / s.width, room.height / s.height, 4)
        if abs(k - trainer.fitScale) > 1e-4 { trainer.fitScale = k }
    }

    @ViewBuilder
    private var caption: some View {
        if trainer.stageShows == .sample, let item = trainer.selectedItem {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    if item.isClip, let image {
                        Text(verbatim: "\(Int(image.size.width)) × \(Int(image.size.height))")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    } else if let rep = NSImageRep(contentsOf: item.url) {
                        Text(verbatim: "\(rep.pixelsWide) × \(rep.pixelsHigh)")
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    if !item.captionedBy.isEmpty {
                        Label(String(localized: "By \(item.captionedBy)"),
                              systemImage: "sparkles")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Text(readAs(item))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.tail)
                }
                TextField("Caption", text: Binding(
                    get: { item.caption },
                    set: { trainer.setCaption(item.id, $0) }),
                          prompt: Text("No caption: the trigger alone"),
                          axis: .vertical)
                    .lineLimit(1...3)
                    .textFieldStyle(.roundedBorder)
                    .disabled(trainer.training)
                if trainer.referenceKind != nil {
                    TrainerReferences(trainer: trainer, item: item)
                }
            }
        }
    }

    /// What the model reads for it: the template with its trigger.
    private func readAs(_ item: TrainerModel.Item) -> String {
        let t = trainer.trigger
        let c = item.caption
        let read: String
        if c.isEmpty {
            read = trainer.missingCaption == "trigger_only" ? t
                : trainer.missingCaption == "skip"
                    ? String(localized: "(left out: no caption)")
                    : String(localized: "(refused: no caption)")
        } else if !t.isEmpty,
                  c.range(of: "\\b\(NSRegularExpression.escapedPattern(for: t))\\b",
                          options: [.regularExpression, .caseInsensitive])
                    != nil {
            read = c.replacingOccurrences(of: "[trigger]", with: t)
        } else {
            read = trainer.captionTemplate
                .replacingOccurrences(of: "{trigger}", with: t)
                .replacingOccurrences(of: "{caption}", with: c)
        }
        return String(localized: "Trained as: \(read)")
    }
}

// MARK: The configuration

/// The Settings tab: what vpipe's train-lora offers, as its keys. Every
/// number reads beside its stepper, centred on it, the steppers on the
/// line of the pop-ups' chevrons.
private struct TrainerConfig: View {
    @Bindable var trainer: TrainerModel

    var body: some View {
        ScrollViewReader { proxy in
        Form {
            Section("Model") {
                Picker("Train on", selection: Binding(
                    get: { trainer.modelId },
                    set: { trainer.choose($0) })) {
                    ForEach(trainer.models) { m in
                        Text(m.installed ? m.name
                             : String(localized: "\(m.name) (not downloaded)"))
                            .tag(m.id)
                    }
                }
                if !trainer.runs.isEmpty {
                    Picker("Trained", selection: Binding(
                        get: { trainer.currentRun?.name ?? "" },
                        set: { n in
                            if let r = trainer.runs.first(where: {
                                $0.name == n
                            }) { trainer.load(r) }
                        })) {
                        Text("New").tag("")
                        ForEach(trainer.runs) { r in
                            Text(verbatim: r.name
                                 + (r.resumable ? " ⏸" : "")
                                 + (r.adapter.isEmpty ? "" : " ✓"))
                                .tag(r.name)
                        }
                    }
                    .help("A training kept: its settings and pictures back here -- ✓ trained, ⏸ paused, to train on")
                }
            }
            Section {
                TextField("Trigger word", text: $trainer.trigger,
                          prompt: Text(verbatim: "ohwx"))
                    .help("The word that calls up what the LoRA learns: choose one that is not an ordinary word. A caption that already holds it, as a whole word, is kept as it is; [trigger] in a caption is replaced by it.")
                TextField("Caption template", text: $trainer.captionTemplate)
                    .help("How the trigger joins a caption: {trigger} and {caption}")
                Picker("No caption", selection: $trainer.missingCaption) {
                    Text("The trigger alone").tag("trigger_only")
                    Text("Leave it out").tag("skip")
                    Text("Refuse").tag("error")
                }
                Toggle("Shuffle tags", isOn: $trainer.shuffleTags)
                    .help("Comma-separated tags shuffled after the first ones kept; each shuffle encoded up front")
                if trainer.shuffleTags {
                    number("Tags kept first", $trainer.keepTags, 0...16)
                }
                number("Repeats", $trainer.repeats, 1...100)
                    .help("Each picture's passes an epoch")
                Toggle("Mirrored copies", isOn: $trainer.flip)
                    .help("Each picture also encoded flipped left to right")
            } header: {
                Text("Trigger and captions")
            } footer: {
                Text("Caption what should stay promptable, and leave the subject to the trigger: it learns only what the captions do not already say.")
            }
            Section {
                VStack(alignment: .leading, spacing: 4) {
                    Text("\(trainer.trigger.isEmpty ? "ohwx" : trainer.trigger) means")
                        .font(Self.label)
                    TextField("", text: $trainer.triggerMeaning,
                              prompt: Text("e.g. a small felt teapot robot with an orange button face"),
                              axis: .vertical)
                        .lineLimit(2...4)
                        .textFieldStyle(.roundedBorder)
                        .labelsHidden()
                        .multilineTextAlignment(.leading)
                        .font(Self.value)
                }
                .help("What the trigger word stands for, as you would tell someone: the model captioning the samples calls it by the trigger and describes everything else")
                Toggle("Caption samples without one", isOn: $trainer.autoCaption)
                    .help("Each sample added without a caption is captioned by the agent's model as it comes; one you type is never replaced")
                HStack {
                    Text(trainer.captioner.isEmpty
                         ? String(localized: "No model here sees pictures")
                         : String(localized: "By \(trainer.captioner)"))
                        .font(Self.value)
                        .foregroundStyle(.secondary)
                    Spacer()
                    if trainer.captioning {
                        Button("Stop") { trainer.stopCaptioning() }
                    } else {
                        Button("Caption Missing") { trainer.captionMissing() }
                            .disabled(trainer.uncaptioned.isEmpty
                                      || trainer.captioner.isEmpty)
                    }
                }
                if let e = trainer.captionError {
                    Text(e)
                        .font(.caption)
                        .foregroundStyle(.red)
                        .fixedSize(horizontal: false, vertical: true)
                }
            } header: {
                Text("Auto captions")
            } footer: {
                Text("The agent's model is shown each sample -- a clip as three frames -- and writes a sentence that starts with the trigger: what it does, where, the light and the framing, never the subject's own look.")
            }
            .id("captions")
            Section("Size") {
                HStack {
                    Text("Resolutions").font(Self.label)
                    Spacer()
                    ForEach(trainer.offeredResolutions, id: \.self) { r in
                        ResolutionChip(
                            size: r, on: trainer.resolutions.contains(r)) {
                            if trainer.resolutions.contains(r) {
                                trainer.resolutions.remove(r)
                            } else {
                                trainer.resolutions.insert(r)
                            }
                        }
                    }
                }
                .help("Each picture encoded at every size chosen, in buckets of its shape: several sizes help a LoRA learned from few pictures; they cost encode time only")
                number("Bucket step", $trainer.bucketStep, 16...128, by: 16)
                real("Widest shape", $trainer.maxAspect, digits: 1)
                    .help("Pictures wider (or taller) than this are cropped")
            }
            if trainer.takesClips {
                Section("Clips") {
                    Picker("Window", selection: $trainer.frames) {
                        ForEach([22, 39, 56, 73, 107, 124], id: \.self) { f in
                            Text("\(String(f)) frames").tag(f)
                        }
                    }
                    .help("Each clip, put on 24 fps, is cut into windows of this many frames (17n + 5, what H3's video VAE makes); a clip takes the longest it fills")
                    number("Windows a clip", $trainer.windows, 1...16)
                        .help("How many windows a clip yields, spread evenly over it; each step trains one")
                    Toggle("Soundtrack", isOn: $trainer.soundtrack)
                        .help("Each window's stretch of the clip's sound trained beside its picture; a clip without one trains on its picture alone")
                    if trainer.soundtrack {
                        real("Sound's loss", $trainer.audioLossWeight,
                             digits: 2)
                            .help("The soundtrack's loss beside the picture's (1 even)")
                    }
                }
                .id("clips")
            }
            if trainer.takesKeyframes {
                Section("Keyframe tasks") {
                    real("From words", $trainer.taskT2VA, digits: 1)
                    real("From a first frame", $trainer.taskI2VA, digits: 1)
                    real("To a last frame", $trainer.taskL2VA, digits: 1)
                    real("First and last", $trainer.taskFL2VA, digits: 1)
                }
                .help("FL2VA's tasks by weight: each clip window draws one -- its own first and last frames the keyframes, shown to the text encoder too")
            }
            Section("Length") {
                Picker("Train for", selection: $trainer.length) {
                    Text("Automatic").tag(TrainerModel.Length.auto)
                    Text("Steps").tag(TrainerModel.Length.steps)
                    Text("Epochs").tag(TrainerModel.Length.epochs)
                }
                .help("Automatic: about 100 steps a picture for a small set (600 to 3,000), two epochs for a large one")
                if trainer.length == .steps {
                    number("Steps", $trainer.steps, 1...100_000, by: 50)
                } else if trainer.length == .epochs {
                    number("Epochs", $trainer.epochs, 1...1000)
                }
                number("Batch", $trainer.batch, 1...64)
                    .help("Samples an optimizer step: run one after another, they cost time, not memory")
                number("Save every", $trainer.saveEvery, 0...100_000, by: 50)
                    .help("Steps between checkpoints, each with its previews; 0 automatic (250 on a small set)")
            }
            Section("Adapter") {
                Picker("Rank", selection: $trainer.rank) {
                    ForEach([4, 8, 16, 32, 64, 128], id: \.self) {
                        Text(String($0)).tag($0)
                    }
                }
                number("Alpha", $trainer.alpha, 0...256)
                    .help("0: the rank's")
                Picker("Trains", selection: $trainer.targets) {
                    Text("Attention and feed-forward").tag("attn_ff")
                    Text("Attention only").tag("attn")
                }
            }
            Section("Optimizer") {
                Picker("Optimizer", selection: $trainer.optimizer) {
                    Text(verbatim: "AdamW").tag("adamw")
                    Text("AdamW, 8-bit").tag("adamw8bit")
                    Text(verbatim: "Adafactor").tag("adafactor")
                    Text(verbatim: "Prodigy").tag("prodigy")
                    Text(verbatim: "Lion").tag("lion")
                    Text(verbatim: "SGD").tag("sgd")
                }
                .id("optimizer")
                field("Learning rate", $trainer.learningRate,
                      LowerScientific())
                    .help("0: the optimizer's own (AdamW 1e-4, Prodigy 1, Lion 3e-5, SGD 1e-2); the model's own default where it has one")
                real("LoRA+ ratio", $trainer.loraPlus, digits: 1)
                    .help("LoRA+: the B factors learn this many times faster than the A factors (16 is the paper's); 1 off")
                Picker("Schedule", selection: $trainer.lrSchedule) {
                    Text("Automatic").tag("auto")
                    Text("Constant").tag("constant")
                    Text("Linear").tag("linear")
                    Text("Cosine").tag("cosine")
                    Text("Cosine with restarts").tag("cosine_restarts")
                }
                .help("Automatic: constant for a small set, cosine from 64 pictures")
                number("Warm-up steps", $trainer.warmup, -1...100_000)
                    .help("-1: 2% of the run")
                real("Gradient clip", $trainer.gradClip, digits: 2)
                    .help("The gradients' global norm held to it; 0 none")
                real("EMA", $trainer.ema, digits: 4)
                    .help("The weights' moving average's decay (0.999); 0 none. Checkpoints keep the averaged adapter too")
                real("Caption dropout", $trainer.captionDropout, digits: 2)
                    .help("How often a sample trains against the empty prompt, which keeps guidance working")
                Picker("Noise levels", selection: $trainer.timestepSampling) {
                    Text("Automatic").tag("auto")
                    Text("Uniform").tag("uniform")
                    Text("Logit-normal").tag("logit_normal")
                    Text("Shifted").tag("shift")
                }
                number("Seed", $trainer.seed, 0...Int(Int32.max))
            }
            Section("Previews") {
                TextEditor(text: $trainer.previewPrompts)
                    .font(.callout)
                    .frame(height: 54)
                    .help("A prompt a line, sampled at every checkpoint with the same seed; {trigger} is the trigger word")
                number("Steps", $trainer.previewSteps, 1...100)
                if !trainer.guidanceFree {
                    real("Guidance", $trainer.previewCfg, digits: 1)
                }
            }
            Section("Arithmetic") {
                if trainer.matrixCores {
                    Toggle("int8 GEMM", isOn: $trainer.int8)
                        .help("The matrix cores' int8 GEMMs, forward and backward: about 6% faster at 1024² on an M5, a short run learning 98% of what bf16 does")
                } else {
                    Toggle("Neural Engine", isOn: $trainer.ane)
                        .help("M4 series: the large GEMMs split with the Neural Engine -- it pays at 1024²")
                }
                Picker("Activations", selection: $trainer.recompute) {
                    Text("Automatic").tag("auto")
                    Text("Recompute each block").tag("always")
                    Text("Keep every block").tag("never")
                }
                .help("Kept, a step is about a quarter faster and takes far more memory (21.6 GB at 1024² for Qwen-Image 2.1); Automatic keeps them where they fit")
            }
        }
        .formStyle(.grouped)
        .scrollContentBackground(.hidden)
        .controlSize(.small)
        .disabled(trainer.training)
        .onChange(of: trainer.scrollTo) { _, t in
            if let t { proxy.scrollTo(t, anchor: .top) }
        }
        }
    }

    /// The form's own label size: rows built of an HStack were drawn a
    /// size smaller than a Toggle's or a Picker's under `.small`.
    static let label = Font.system(size: NSFont.systemFontSize)
    /// A value's size: the pop-ups' (their small control size's).
    static let value = Font.system(size: NSFont.smallSystemFontSize)

    /// A stepper as small as a pop-up's arrows, set in from the row's
    /// edge as they are: the two on one line.
    static func stepper<S: View>(_ s: S) -> some View {
        s.labelsHidden()
            .controlSize(.mini)
            .padding(.trailing, 3)
    }

    /// A whole number: its label, then the value typed beside its
    /// stepper, the row's middle line through all three.
    private func number(_ title: LocalizedStringKey, _ v: Binding<Int>,
                        _ range: ClosedRange<Int>, by step: Int = 1)
        -> some View {
        HStack(alignment: .center, spacing: 4) {
            Text(title).font(Self.label).lineLimit(1)
            Spacer()
            TextField("", value: v, format: .number.grouping(.never))
                .textFieldStyle(.plain)
                .lineLimit(1)
                .multilineTextAlignment(.trailing)
                .monospacedDigit()
                .font(Self.value)
                .frame(width: 70)
            Self.stepper(Stepper("", value: v, in: range, step: step))
        }
    }

    /// A real: as a number, a step of its last digit.
    private func real(_ title: LocalizedStringKey, _ v: Binding<Double>,
                      digits: Int) -> some View {
        let step = pow(10, -Double(digits))
        return HStack(alignment: .center, spacing: 4) {
            Text(title).font(Self.label).lineLimit(1)
            Spacer()
            TextField("", value: v,
                      format: .number.precision(.fractionLength(digits)))
                .textFieldStyle(.plain)
                .lineLimit(1)
                .multilineTextAlignment(.trailing)
                .monospacedDigit()
                .font(Self.value)
                .frame(width: 70)
            Self.stepper(Stepper("", value: v, in: 0...1_000, step: step))
        }
    }

    /// A value typed, its stepper's room kept so it lines up with those.
    private func field<F: ParseableFormatStyle>(
        _ title: LocalizedStringKey, _ v: Binding<Double>, _ format: F
    ) -> some View where F.FormatInput == Double, F.FormatOutput == String {
        HStack(alignment: .center, spacing: 4) {
            Text(title).font(Self.label).lineLimit(1)
            Spacer()
            TextField("", value: v, format: format)
                .textFieldStyle(.plain)
                .lineLimit(1)
                .multilineTextAlignment(.trailing)
                .monospacedDigit()
                .font(Self.value)
                .frame(width: 90)
            Self.stepper(Stepper("", onIncrement: { v.wrappedValue *= 2 },
                                 onDecrement: { v.wrappedValue /= 2 }))
                .help("Doubled or halved")
        }
    }
}

/// A resolution chosen or not: chosen, black (the label's colour) with
/// its number knocked out.
private struct ResolutionChip: View {
    let size: Int
    let on: Bool
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Text(verbatim: String(size))
                .font(.caption.monospacedDigit())
                .padding(.horizontal, 7)
                .padding(.vertical, 3)
                .foregroundStyle(on ? Color(nsColor: .windowBackgroundColor)
                                    : .primary)
                .background(RoundedRectangle(cornerRadius: 5)
                    .fill(on ? Color.primary : Color.primary.opacity(0.08)))
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(on ? .isSelected : [])
    }
}

// MARK: The progress

/// The bottom island: what the training is doing -- the dataset
/// encoded, then each step and its loss -- the time left, the loss curve
/// (its steps zoomed by the range under it, the loss axis fitted), and
/// the previews on the right, one clicked shown on the stage.
private struct TrainerProgress: View {
    @Bindable var trainer: TrainerModel

    var body: some View {
        HStack(alignment: .top, spacing: 16) {
            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .firstTextBaseline) {
                    Text(title).font(.callout.weight(.semibold))
                    Spacer()
                    Text(line)
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                if trainer.training {
                    if trainer.total > 0 {
                        ProgressView(value: Double(trainer.done),
                                     total: Double(trainer.total))
                    } else {
                        ProgressView().progressViewStyle(.linear)
                    }
                } else {
                    Text(status)
                        .font(.caption)
                        .foregroundStyle(trainer.failed ? .red : .secondary)
                        .lineLimit(2)
                }
                LossChart(trainer: trainer)
            }
            .frame(maxWidth: .infinity)
            previewGrid
                .frame(width: 236)
        }
        .padding(14)
    }

    private var status: String {
        trainer.outcome ?? trainer.blocker
            ?? String(localized: "Ready: Start encodes the pictures, then trains.")
    }

    private var title: String {
        guard trainer.training else { return String(localized: "Progress") }
        switch trainer.phase {
        case "train-encode": return String(localized: "Encoding the dataset")
        case "train": return String(localized: "Training")
        default: return String(localized: "Preparing…")
        }
    }

    private var line: String {
        var parts: [String] = []
        if trainer.phase == "train", trainer.total > 0 {
            parts.append(String(localized: "step \(String(trainer.done)) of \(String(trainer.total))"))
            if let l = trainer.losses.last {
                parts.append(String(localized: "loss \(String(format: "%.4f", l))"))
            }
            if trainer.stepSeconds > 0 {
                parts.append(String(format: "%.2f s/step", trainer.stepSeconds))
            }
            if let left = trainer.secondsLeft {
                parts.append(String(localized: "~\(Self.clock(left)) left"))
            }
            if let c = trainer.checkpoints.last {
                parts.append(String(localized: "saved at \(String(c.step))"))
            }
        } else if trainer.training, trainer.total > 0 {
            parts.append("\(trainer.done) / \(trainer.total)")
            if !trainer.detail.isEmpty { parts.append(trainer.detail) }
        }
        if trainer.training, let t = trainer.started {
            parts.append(String(localized: "\(Self.clock(Date().timeIntervalSince(t))) elapsed"))
        }
        return parts.joined(separator: " · ")
    }

    static func clock(_ s: Double) -> String {
        let n = Int(s)
        return n >= 3600
            ? String(format: "%d:%02d:%02d", n / 3600, n / 60 % 60, n % 60)
            : String(format: "%d:%02d", n / 60, n % 60)
    }

    private var previewGrid: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Previews")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            if trainer.previews.isEmpty {
                Text("Previews come at each checkpoint")
                    .font(.caption)
                    .foregroundStyle(.tertiary)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVGrid(columns: Array(repeating: GridItem(.fixed(54),
                                                                 spacing: 6),
                                             count: 4), spacing: 6) {
                        ForEach(trainer.previews.reversed(), id: \.self) { p in
                            Button {
                                trainer.showPreview(p)
                            } label: {
                                PreviewThumb(path: p)
                                    .frame(width: 54, height: 54)
                                    .overlay(RoundedRectangle(cornerRadius: 4)
                                        .strokeBorder(
                                            trainer.stageShows == .preview
                                                && trainer.stagePath == p
                                                ? Color.accentColor : .clear,
                                            lineWidth: 2))
                            }
                            .buttonStyle(.plain)
                            .help(URL(fileURLWithPath: p).lastPathComponent)
                        }
                    }
                }
            }
        }
    }
}

private struct PreviewThumb: View {
    let path: String
    @State private var img: NSImage?

    var body: some View {
        Thumb(image: img)
            .task(id: path) {
                img = await TrainerThumbs.image(URL(fileURLWithPath: path),
                                                128)
            }
    }
}

/// The loss at each step (faint) and its moving mean over 10 (the
/// line), over the steps the range under it shows -- 0 to the last step,
/// or any span between -- the loss axis fitted to what is shown.
private struct LossChart: View {
    @Bindable var trainer: TrainerModel

    private var last: Double {
        Double(max(trainer.total, trainer.losses.count, 1))
    }

    var body: some View {
        let range = trainer.lossRange ?? 0...last
        VStack(spacing: 4) {
            LossCurve(losses: trainer.losses, from: range.lowerBound,
                      to: range.upperBound)
                .help("The loss at each step, and its moving mean")
            HStack(spacing: 6) {
                Text(verbatim: String(Int(range.lowerBound)))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(width: 40, alignment: .leading)
                RangeBar(range: Binding(
                    get: { trainer.lossRange ?? 0...last },
                    set: { r in
                        trainer.lossRange = r.lowerBound <= 0
                            && r.upperBound >= last ? nil : r
                    }), bounds: 0...last)
                    .frame(height: 16)
                Text(verbatim: String(Int(range.upperBound)))
                    .font(.caption2.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(width: 40, alignment: .trailing)
                Button("All") { trainer.lossRange = nil }
                    .controlSize(.mini)
                    .disabled(trainer.lossRange == nil)
                    .help("Every step")
            }
        }
    }
}

private struct LossCurve: View {
    let losses: [Double]
    let from: Double
    let to: Double

    var body: some View {
        Canvas { ctx, size in
            ctx.fill(RoundedRectangle(cornerRadius: 6)
                        .path(in: CGRect(origin: .zero, size: size)),
                     with: .color(.primary.opacity(0.04)))
            // Steps from 1: loss i is step i + 1.
            let lo = max(0, Int(from.rounded(.down)) - 1)
            let hi = min(losses.count, Int(to.rounded(.up)))
            guard hi - lo >= 2 else {
                ctx.draw(Text("The loss is drawn here, step by step, as it trains")
                            .font(.caption).foregroundStyle(.tertiary),
                         at: CGPoint(x: size.width / 2, y: size.height / 2))
                return
            }
            let seen = losses[lo..<hi]
            let means: [Double] = (lo..<hi).map { i in
                let w = losses[max(0, i - 9)...i]
                return w.reduce(0, +) / Double(w.count)
            }
            let vmin = min(seen.min() ?? 0, means.min() ?? 0)
            let vmax = max(seen.max() ?? 1, vmin + 1e-6)
            let span = max(to - from, 1)
            func pt(_ step: Double, _ v: Double) -> CGPoint {
                CGPoint(x: 6 + (size.width - 12) * CGFloat((step - from) / span),
                        y: 6 + (size.height - 12)
                            * CGFloat(1 - (v - vmin) / (vmax - vmin)))
            }
            var raw = Path(), mean = Path()
            for (k, i) in (lo..<hi).enumerated() {
                let s = Double(i + 1)
                if k == 0 {
                    raw.move(to: pt(s, losses[i]))
                    mean.move(to: pt(s, means[k]))
                } else {
                    raw.addLine(to: pt(s, losses[i]))
                    mean.addLine(to: pt(s, means[k]))
                }
            }
            ctx.stroke(raw, with: .color(.secondary.opacity(0.35)),
                       lineWidth: 1)
            ctx.stroke(mean, with: .color(.accentColor), lineWidth: 1.6)
            // The loss axis's ends.
            for (v, y) in [(vmax, CGFloat(12)), (vmin, size.height - 10)] {
                ctx.draw(Text(verbatim: String(format: "%.3f", v))
                            .font(.system(size: 9).monospacedDigit())
                            .foregroundStyle(.tertiary),
                         at: CGPoint(x: 8, y: y), anchor: .leading)
            }
        }
    }
}

/// Two handles on a track: a span of `bounds`, at least a step wide.
private struct RangeBar: View {
    @Binding var range: ClosedRange<Double>
    let bounds: ClosedRange<Double>

    var body: some View {
        GeometryReader { g in
            let w = max(g.size.width - 12, 1)
            let total = max(bounds.upperBound - bounds.lowerBound, 1)
            let x = { (v: Double) in 6 + w * CGFloat((v - bounds.lowerBound) / total) }
            let value = { (px: CGFloat) in
                min(max(bounds.lowerBound + Double((px - 6) / w) * total,
                        bounds.lowerBound), bounds.upperBound).rounded()
            }
            ZStack(alignment: .leading) {
                Capsule().fill(Color.primary.opacity(0.1))
                    .frame(height: 4)
                Capsule().fill(Color.accentColor.opacity(0.6))
                    .frame(width: max(2, x(range.upperBound) - x(range.lowerBound)),
                           height: 4)
                    .offset(x: x(range.lowerBound))
                handle
                    .offset(x: x(range.lowerBound) - 6)
                    .gesture(DragGesture(coordinateSpace: .named("bar")).onChanged { d in
                        let v = min(value(d.location.x), range.upperBound - 1)
                        range = max(v, bounds.lowerBound)...range.upperBound
                    })
                handle
                    .offset(x: x(range.upperBound) - 6)
                    .gesture(DragGesture(coordinateSpace: .named("bar")).onChanged { d in
                        let v = max(value(d.location.x), range.lowerBound + 1)
                        range = range.lowerBound...min(v, bounds.upperBound)
                    })
            }
            .frame(height: g.size.height)
            .coordinateSpace(name: "bar")
        }
    }

    private var handle: some View {
        Circle()
            .fill(Color(nsColor: .controlColor))
            .overlay(Circle().strokeBorder(Color.primary.opacity(0.3)))
            .shadow(color: .black.opacity(0.15), radius: 1, y: 0.5)
            .frame(width: 12, height: 12)
    }
}

/// Scientific notation as people write it -- 1e-4, 2.5e-4, 0 -- with a
/// lower-case e and no needless digits; typed as any number is.
struct LowerScientific: ParseableFormatStyle {
    var parseStrategy: Parse { Parse() }

    func format(_ v: Double) -> String {
        guard v != 0, v.isFinite else { return "0" }
        let e = Int(floor(log10(abs(v))))
        var m = v / pow(10, Double(e))
        m = (m * 1000).rounded() / 1000
        var ms = String(format: "%.3f", m)
        while ms.hasSuffix("0") { ms.removeLast() }
        if ms.hasSuffix(".") { ms.removeLast() }
        return e == 0 ? ms : "\(ms)e\(e)"
    }

    struct Parse: ParseStrategy {
        func parse(_ s: String) throws -> Double {
            guard let v = Double(s.trimmingCharacters(in: .whitespaces)
                                     .lowercased()) else {
                throw CocoaError(.formatting)
            }
            return v
        }
    }
}

/// A clip on the trainer's stage: played muted on a loop, as a sample
/// or a preview is looked at.
private struct TrainerClip: NSViewRepresentable {
    let url: URL

    final class Coordinator {
        var looper: AVPlayerLooper?
    }
    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> AVPlayerView {
        let v = AVPlayerView()
        v.controlsStyle = .inline
        v.videoGravity = .resizeAspect
        load(v, context.coordinator)
        return v
    }

    func updateNSView(_ v: AVPlayerView, context: Context) {
        if (v.player?.currentItem?.asset as? AVURLAsset)?.url != url {
            load(v, context.coordinator)
        }
    }

    private func load(_ v: AVPlayerView, _ c: Coordinator) {
        let q = AVQueuePlayer()
        q.isMuted = true
        c.looper = AVPlayerLooper(player: q,
                                  templateItem: AVPlayerItem(url: url))
        v.player = q
        q.play()
    }

    static func dismantleNSView(_ v: AVPlayerView, coordinator: Coordinator) {
        v.player?.pause()
        coordinator.looper = nil
    }
}

/// A sample's REFERENCES -- Ref2VA's, an edit's source (the first) --
/// under its caption: their thumbnails, + to add, one dropped on the row,
/// x to take one out. Kept in the sample's .refs in the run's dataset.
private struct TrainerReferences: View {
    @Bindable var trainer: TrainerModel
    let item: TrainerModel.Item
    @State private var over = false

    var body: some View {
        HStack(spacing: 6) {
            Text(trainer.referenceKind == "edit"
                 ? "Edited from" : "References")
                .font(.caption)
                .foregroundStyle(.secondary)
            ScrollView(.horizontal) {
                HStack(spacing: 6) {
                    ForEach(item.references, id: \.self) { u in
                        RefThumb(url: u) {
                            trainer.removeReference(item.id, u)
                        }
                    }
                    if item.references.isEmpty {
                        Text(trainer.referenceKind == "edit"
                             ? "None: it trains from the words alone"
                             : "None")
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                    }
                }
            }
            .scrollIndicators(.never)
            Button {
                choose()
            } label: {
                Image(systemName: "plus")
            }
            .buttonStyle(.borderless)
            .help(trainer.referenceKind == "edit"
                  ? "The picture it was edited from (the first), and any others it is made from"
                  : "Pictures or clips it is made from, named <Picture 1>, <Video 1> ... in its caption as the model reads them")
        }
        .frame(height: 40)
        .padding(.horizontal, 4)
        .background(RoundedRectangle(cornerRadius: 6)
            .strokeBorder(Color.accentColor.opacity(over ? 0.8 : 0)))
        .disabled(trainer.training)
        .onDrop(of: [.fileURL], isTargeted: $over) { providers in
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    guard let url else { return }
                    Task { @MainActor in
                        trainer.addReferences(item.id, [url])
                    }
                }
            }
            return true
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.allowsMultipleSelection = true
        panel.allowedContentTypes = trainer.referenceKind == "optional"
            ? [.image, .movie] : [.image]
        panel.prompt = String(localized: "Add")
        guard panel.runModal() == .OK else { return }
        trainer.addReferences(item.id, panel.urls)
    }
}

private struct RefThumb: View {
    let url: URL
    let remove: () -> Void
    @State private var img: NSImage?
    @State private var hovering = false

    var body: some View {
        Thumb(image: img)
            .frame(width: 34, height: 34)
            .overlay(alignment: .topTrailing) {
                if hovering {
                    Button(action: remove) {
                        Image(systemName: "xmark.circle.fill")
                            .font(.system(size: 11))
                            .foregroundStyle(.white, .black.opacity(0.6))
                    }
                    .buttonStyle(.plain)
                    .help("Take it out")
                }
            }
            .onHover { hovering = $0 }
            .help(url.lastPathComponent)
            .task(id: url) { img = await TrainerThumbs.image(url, 96) }
    }
}
