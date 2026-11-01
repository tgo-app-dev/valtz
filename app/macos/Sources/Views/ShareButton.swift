import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// Share: a menu -- Save… first, then the system's Share… (AirDrop,
/// Messages, Mail, Photos, Notes…) -- for the image or video on screen,
/// adjustments included. Save… is Valtz's own item, not a service handed
/// to the share sheet: the sheet runs out of process, and an item added
/// to it never called back.
struct ShareButton: View {
    let model: AppModel
    @State private var anchor = MenuAnchor()

    var body: some View {
        Button(action: openMenu) {
            Label("Share", systemImage: "square.and.arrow.up")
        }
        .help("Save or share the image")
        // The menu opens under the button itself; the anchor is an
        // AppKit view in the button's place.
        .background(MenuAnchorView(anchor: anchor))
        .disabled(!model.canExportStage)
        .onChange(of: model.shareRequest) { openMenu() }
    }

    /// The menu open now (a scripted snapshot closes it).
    static weak var openedMenu: NSMenu?

    private func openMenu() {
        guard model.canExportStage else { return }
        let menu = NSMenu()
        let save = MenuAction.item(
            model.saveItemTitle,
            symbol: "square.and.arrow.down") { [model] in
            ShareButton.save(model)
        }
        save.keyEquivalent = "e"
        menu.addItem(save)
        menu.addItem(.separator())
        // The system's Share…: the file is made when it is chosen, not
        // for every opening of the menu -- an adjusted picture is
        // written out, a stack flattened.
        let share = MenuAction.item(String(localized: "Share…"),
                                    symbol: "square.and.arrow.up") {
            [model, anchor] in
            ShareButton.showPicker(model, anchor)
        }
        menu.addItem(share)
        ShareButton.openedMenu = menu
        if let v = anchor.view, v.window != nil {
            menu.popUp(positioning: nil,
                       at: NSPoint(x: 0, y: v.isFlipped ? v.bounds.maxY + 6
                                                        : -6),
                       in: v)
        } else if let cv = NSApp.keyWindow?.contentView {
            menu.popUp(positioning: nil,
                       at: NSPoint(x: cv.bounds.maxX - 150,
                                   y: cv.bounds.maxY - 52),
                       in: cv)
        }
    }

    /// The share sheet, from the button, for what is on screen.
    static func showPicker(_ model: AppModel, _ anchor: MenuAnchor) {
        guard let file = model.shareableFile() else { return }
        let picker = NSSharingServicePicker(items: [file])
        anchor.picker = picker
        if let v = anchor.view, v.window != nil {
            picker.show(relativeTo: v.bounds, of: v, preferredEdge: .minY)
        } else if let cv = NSApp.keyWindow?.contentView {
            // No anchor in the toolbar: under its right end.
            picker.show(relativeTo: CGRect(x: cv.bounds.maxX - 150,
                                           y: cv.bounds.maxY - 52,
                                           width: 1, height: 1),
                        of: cv, preferredEdge: .minY)
        }
    }

    /// A scripted snapshot's choices, made in the open panel, and the
    /// file it saves to -- never the panel's folder, which is the one
    /// a person last saved in.
    static var script: ((NSSavePanel, SaveOptions) -> Void)?
    static var scriptedDestination: URL?

    /// Save…: the panel -- a sheet on the window -- with the format and
    /// its quality under it. A sheet, not a modal loop of its own: run
    /// from a SwiftUI action, a modal panel held the main queue -- and
    /// with it every main-actor task -- until it closed.
    static func save(_ model: AppModel) {
        guard model.canExportStage else { return }
        let panel = NSSavePanel()
        let kind = model.currentAsset?.kind ?? "image"
        panel.title = model.savePanelTitle
        panel.nameFieldStringValue = model.exportFileName
        // A still's pages: a file each, numbered after the name.
        if let a = model.currentAsset, a.isPaged {
            let n = String(a.pageCount)
            panel.message = String(localized: "Its \(n) pages are saved as a file each, numbered after the name: Name-1, Name-2, …")
        }
        let ext = (model.exportFileName as NSString).pathExtension
        if let type = UTType(filenameExtension: ext) {
            panel.allowedContentTypes = [type]
        }
        panel.canCreateDirectories = true
        // Where exports last went.
        panel.directoryURL = model.panelFolder(.export)
        // The format: the file as it is, or an export (16-bit, OpenEXR,
        // JPEG at a quality, ProRes, HEVC 10-bit, a sound's WAV or AAC
        // cut at its marks...).
        let choices = model.exportChoices
        let options = choices.isEmpty ? nil
            : SaveOptions(choices: choices, originalExt: ext, kind: kind,
                          panel: panel, picture: model.savedPicture,
                          seconds: model.exportSeconds)
        panel.accessoryView = options?.view
        options?.apply()
        let scripted = script != nil
        let finish = { (response: NSApplication.ModalResponse) in
            guard response == .OK,
                  let url = scripted ? scriptedDestination : panel.url
            else { return }
            model.rememberPanel(.export, chose: url)
            model.save(to: url, as: options?.choice ?? .original,
                       quality: options?.jpegQuality,
                       video: options?.videoEncoding)
            options?.remember()
        }
        if let script, let options {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
                script(panel, options)
            }
        }
        // Valtz's window: the main one, else the one showing.
        let window = [NSApp.mainWindow, NSApp.keyWindow].compactMap { $0 }
            .first { $0.isVisible && !($0 is NSPanel) }
            ?? NSApp.windows.first {
                $0.isVisible && $0.canBecomeMain && !($0 is NSPanel)
            }
        if let window, window.attachedSheet == nil {
            panel.beginSheetModal(for: window, completionHandler: finish)
        } else {
            panel.begin(completionHandler: finish)
        }
    }
}

/// What the save panel's options say: the format, and its quality -- a
/// PNG's or a TIFF's depth, a JPEG's quality, with about the file size
/// it makes; a movie's encoding (VideoEncodingRows). The last ones
/// chosen come back the next time (not in a scripted snapshot run).
///
/// Its controls are AppKit's: a pop-up, a segmented control, a slider. A
/// SwiftUI picker in a save panel's accessory view never opened its menu
/// -- a click on it did nothing -- so the format could not be changed.
@MainActor
final class SaveOptions: NSObject {
    /// The formats listed: PNG and TIFF once each, their depth apart.
    let formats: [ExportChoice]
    let originalExt: String
    /// What is saved: "image", "video" or "audio". Only a picture has
    /// options under its format (a depth, a quality).
    let kind: String
    private(set) var format: ExportChoice
    /// PNG and TIFF: 16 bits per channel, or 8.
    private(set) var deep: Bool
    /// JPEG: 1...100.
    private(set) var quality: Double
    /// About how large the JPEG will be, at `quality`.
    private(set) var estimate: Int64?
    /// The panel's accessory view.
    let view: NSView
    private weak var panel: NSSavePanel?
    private let picture: CGImage?
    private var estimating = 0
    private var estimateTimer: Timer?

    private let formatMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let optionLabel = NSTextField(labelWithString: "")
    private let depth = NSSegmentedControl(
        labels: [String(localized: "8 bits"), String(localized: "16 bits")],
        trackingMode: .selectOne, target: nil, action: nil)
    private let slider = NSSlider(value: 90, minValue: 1, maxValue: 100,
                                  target: nil, action: nil)
    private let qualityValue = NSTextField(labelWithString: "")
    private let sizeLabel = NSTextField(labelWithString: "")
    private let note = NSTextField(labelWithString: "")
    private let qualityRow: NSStackView
    private let options: NSStackView
    /// A movie's encoding, under its format.
    let video: VideoEncodingRows?
    private let grid: NSGridView
    private let box: NSView

    init(choices: [ExportChoice], originalExt: String, kind: String,
         panel: NSSavePanel, picture: CGImage?, seconds: Double? = nil) {
        var seen: Set<ExportChoice> = []
        let formats = choices.map(\.format).filter {
            seen.insert($0).inserted
        }
        self.formats = formats
        self.originalExt = originalExt
        self.kind = kind
        self.panel = panel
        self.picture = picture
        let d = Self.keeps ? UserDefaults.standard : nil
        let last = d?.string(forKey: Self.key(kind))
            .flatMap(ExportChoice.init(rawValue:))
        format = last.flatMap { formats.contains($0) ? $0 : nil }
            ?? formats[0]
        deep = d?.object(forKey: Self.deepKey) as? Bool ?? true
        let q = d?.double(forKey: Self.qualityKey) ?? 0
        quality = q >= 1 ? min(100, q) : 90

        let small = NSFont.systemFont(ofSize: NSFont.smallSystemFontSize)
        let least = NSTextField(labelWithString: String(localized: "Least"))
        let best = NSTextField(labelWithString: String(localized: "Best"))
        for l in [least, best, sizeLabel] {
            l.font = small
            l.textColor = .secondaryLabelColor
        }
        qualityValue.font = .monospacedDigitSystemFont(
            ofSize: NSFont.systemFontSize, weight: .regular)
        qualityValue.alignment = .right
        qualityValue.widthAnchor.constraint(equalToConstant: 28).isActive = true
        sizeLabel.font = .monospacedDigitSystemFont(
            ofSize: NSFont.smallSystemFontSize, weight: .regular)
        slider.widthAnchor.constraint(equalToConstant: 170).isActive = true
        qualityRow = NSStackView(views: [least, slider, best, qualityValue,
                                         sizeLabel])
        qualityRow.spacing = 8
        note.textColor = .secondaryLabelColor
        depth.toolTip = String(localized: "Bits per channel: 16 keeps every shade the picture has; 8 makes a smaller file")
        options = NSStackView(views: [depth, qualityRow, note])
        options.orientation = .horizontal
        options.alignment = .centerY

        let formatLabel = NSTextField(labelWithString: String(localized: "Format:"))
        var rows: [[NSView]] = [[formatLabel, formatMenu]]
        if kind == "image" {
            rows.append([optionLabel, options])
        }
        video = kind == "video" ? VideoEncodingRows(seconds: seconds) : nil
        if let video { rows += video.rows }
        let grid = NSGridView(views: rows)
        grid.column(at: 0).xPlacement = .trailing
        grid.rowAlignment = .firstBaseline
        grid.rowSpacing = 10
        grid.columnSpacing = 8
        for r in 0..<grid.numberOfRows {
            grid.row(at: r).height = 24
            grid.row(at: r).yPlacement = .center
        }
        let box = NSView()
        self.grid = grid
        self.box = box
        grid.translatesAutoresizingMaskIntoConstraints = false
        box.addSubview(grid)
        // At least the panel's usual width; wider when a movie's rows
        // need it (they were cut at the left).
        NSLayoutConstraint.activate([
            grid.topAnchor.constraint(equalTo: box.topAnchor, constant: 12),
            grid.bottomAnchor.constraint(equalTo: box.bottomAnchor,
                                         constant: -12),
            grid.centerXAnchor.constraint(equalTo: box.centerXAnchor),
            grid.leadingAnchor.constraint(greaterThanOrEqualTo:
                                              box.leadingAnchor,
                                          constant: 20),
            box.widthAnchor.constraint(greaterThanOrEqualToConstant: 520),
        ])
        view = box
        super.init()

        for f in formats {
            formatMenu.addItem(withTitle: f.label(originalExt: originalExt))
        }
        formatMenu.selectItem(at: formats.firstIndex(of: format) ?? 0)
        formatMenu.target = self
        formatMenu.action = #selector(formatChanged)
        depth.selectedSegment = deep ? 1 : 0
        depth.target = self
        depth.action = #selector(depthChanged)
        slider.doubleValue = quality
        slider.isContinuous = true
        slider.target = self
        slider.action = #selector(qualityChanged)
        video?.onChange = { [weak self] in self?.fit() }
        box.frame.size = box.fittingSize
    }

    /// The accessory as tall as its rows shown.
    private func fit() {
        video?.hideRows(in: grid)
        box.layoutSubtreeIfNeeded()
        box.setFrameSize(box.fittingSize)
    }

    /// The request's "video": a movie's encoding, nil for the encoder's
    /// own (or a picture's, a sound's).
    var videoEncoding: [String: Any]? { format.video ? video?.request : nil }

    /// What is written: the format at its depth.
    var choice: ExportChoice { format.deep(deep) }
    var jpegQuality: Int? {
        choice == .jpeg ? Int(quality.rounded()) : nil
    }

    // As the controls set them -- for a scripted snapshot run too.
    func setFormat(_ f: ExportChoice) {
        guard let i = formats.firstIndex(of: f.format) else { return }
        formatMenu.selectItem(at: i)
        formatChanged()
    }

    func setDeep(_ on: Bool) {
        depth.selectedSegment = on ? 1 : 0
        depthChanged()
    }

    func setQuality(_ q: Double) {
        slider.doubleValue = q
        qualityChanged()
    }

    @objc private func formatChanged() {
        let i = formatMenu.indexOfSelectedItem
        guard formats.indices.contains(i) else { return }
        format = formats[i]
        apply()
    }

    @objc private func depthChanged() {
        deep = depth.selectedSegment == 1
        apply()
    }

    @objc private func qualityChanged() {
        quality = slider.doubleValue.rounded()
        qualityValue.stringValue = "\(Int(quality))"
        estimateSoon()
    }

    /// The panel follows the format -- its type and the name's extension
    /// -- and the row under the format shows what it can be set to.
    func apply() {
        let ext = choice.ext ?? originalExt
        if let panel {
            if let type = UTType(filenameExtension: ext) {
                panel.allowedContentTypes = [type]
            }
            let base = (panel.nameFieldStringValue as NSString)
                .deletingPathExtension
            panel.nameFieldStringValue = base + "." + ext
        }
        let jpeg = format == .jpeg
        depth.isHidden = !format.hasDepth
        qualityRow.isHidden = !jpeg
        note.isHidden = format.hasDepth || jpeg
        optionLabel.stringValue = format.hasDepth
            ? String(localized: "Depth:")
            : jpeg ? String(localized: "Quality:") : ""
        note.stringValue = switch format {
        case .original:
            String(localized: "The file as it is, nothing re-encoded")
        case .pdf:
            String(localized: "Shapes and words stay sharp at any size; every page in one file")
        default:
            String(localized: "Every value the picture holds, highlights included")
        }
        qualityValue.stringValue = "\(Int(quality))"
        video?.show(format)
        fit()
        estimateSoon()
    }

    /// A JPEG of the picture, at the quality, made in memory and measured
    /// -- ImageIO, as vpipe writes it -- a moment after the slider stops.
    /// Off the main thread, and answered through the run loop, which
    /// turns while the panel is up.
    private func estimateSoon() {
        estimateTimer?.invalidate()
        estimating += 1
        guard choice == .jpeg, let picture else {
            estimate = nil
            sizeLabel.stringValue = ""
            return
        }
        let ticket = estimating
        let q = quality / 100
        let t = Timer(timeInterval: 0.12, repeats: false) { [weak self] _ in
            DispatchQueue.global(qos: .userInitiated).async {
                let bytes = Self.jpegBytes(picture, quality: q)
                CFRunLoopPerformBlock(CFRunLoopGetMain(),
                                      CFRunLoopMode.commonModes.rawValue) {
                    MainActor.assumeIsolated {
                        guard let self, ticket == self.estimating else {
                            return
                        }
                        self.estimate = bytes
                        self.sizeLabel.stringValue = bytes.map {
                            String(localized: "about \($0.formatted(ByteCountFormatStyle(style: .file)))")
                        } ?? ""
                    }
                }
                CFRunLoopWakeUp(CFRunLoopGetMain())
            }
        }
        RunLoop.main.add(t, forMode: .common)
        estimateTimer = t
    }

    nonisolated private static func jpegBytes(_ img: CGImage,
                                              quality: Double) -> Int64? {
        let data = NSMutableData()
        guard let dst = CGImageDestinationCreateWithData(
            data as CFMutableData, UTType.jpeg.identifier as CFString, 1,
            nil) else { return nil }
        CGImageDestinationAddImage(dst, img, [
            kCGImageDestinationLossyCompressionQuality: quality,
        ] as CFDictionary)
        return CGImageDestinationFinalize(dst) ? Int64(data.length) : nil
    }

    func remember() {
        guard Self.keeps else { return }
        let d = UserDefaults.standard
        d.set(format.rawValue, forKey: Self.key(kind))
        d.set(deep, forKey: Self.deepKey)
        d.set(quality.rounded(), forKey: Self.qualityKey)
        video?.remember()
    }

    private static let imageKey = "save.imageFormat"
    private static let videoKey = "save.videoFormat"
    private static let soundKey = "save.soundFormat"

    /// The last format saved, kept per kind.
    private static func key(_ kind: String) -> String {
        kind == "audio" ? soundKey : kind == "video" ? videoKey : imageKey
    }
    private static let deepKey = "save.deep"
    private static let qualityKey = "save.jpegQuality"
    private static var keeps: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
    }
}

/// A menu item that runs a closure.
@MainActor
final class MenuAction: NSObject {
    private let run: () -> Void

    private init(_ run: @escaping () -> Void) { self.run = run }

    @objc private func fire() { run() }

    static func item(_ title: String, symbol: String,
                     run: @escaping () -> Void) -> NSMenuItem {
        let action = MenuAction(run)
        let item = NSMenuItem(title: title, action: #selector(fire),
                              keyEquivalent: "")
        item.target = action
        item.representedObject = action  // the item keeps it
        item.image = NSImage(systemSymbolName: symbol,
                             accessibilityDescription: nil)
        return item
    }
}

@MainActor
final class MenuAnchor {
    weak var view: NSView?
    /// The share sheet shown: kept while it is open.
    var picker: NSSharingServicePicker?
}

struct MenuAnchorView: NSViewRepresentable {
    let anchor: MenuAnchor

    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        anchor.view = v
        return v
    }

    func updateNSView(_ v: NSView, context: Context) { anchor.view = v }
}
