import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// The title bar, after Preview's: ☰, then the name of what is open
/// (the window title), then -- docked right -- groups of controls, each
/// its own glass capsule (drawn here, TitleBarControls):
///
///   start over (anonymous sessions) clear the session, delete its files
///   zoom       zoom out, actual size, the zoom level, fit, zoom in --
///              divided like Preview's; 1:1 and fit press in to show
///              the state they hold
///   compare    the compare under the stage, on or off (pressed in on):
///              a result turns it on only when it changed what was there
///   ground     the light bulb: the app light or dark, for proofing
///   markup     the markup toolbar
///   document   the inspector; share (a menu: Save…, then the system's
///              Share… -- AirDrop, Messages, Mail…)
///   search     the search field (TitleBarSearchField)
struct TitleBarContent: ToolbarContent {
    @Bindable var model: AppModel
    let scheme: ColorScheme

    var body: some ToolbarContent {
        ToolbarItem(placement: .navigation) {
            Button(action: model.toggleNav) {
                Label("Navigation", systemImage: "line.3.horizontal")
            }
            .help(model.navOpen ? "Hide navigation" : "Show navigation")
            .disabled(model.fleetServing != nil)
        }

        // The name, drawn here rather than by the window (whose title is
        // kept for the Window menu), so simple mode's can be renamed.
        ToolbarItem(placement: .navigation) {
            WindowTitleField(model: model)
                .disabled(model.fleetServing != nil)
        }
        .sharedBackgroundVisibility(.hidden)

        // Nothing else pushes the controls right: the window's title is
        // ours (WindowTitleField), and the search field is in the item.
        ToolbarSpacer(.flexible, placement: .primaryAction)

        // The groups draw their own glass capsules, in one item: with any
        // custom view among them, the toolbar folds every item after it
        // into one shared capsule and drops the spacers between groups.
        // They act on the editor: on the log there are none (an item
        // added and removed -- a hosted item does not redraw its content
        // for a change of screen).
        if model.screen == .editor {
            ToolbarItem(placement: .primaryAction) {
                TitleBarControls(model: model, scheme: scheme)
                    .disabled(model.fleetServing != nil)
            }
            .sharedBackgroundVisibility(.hidden)
        }
    }
}

/// The name of what is open, after ☰. Simple mode's session name renames
/// with a click: Return (or clicking away) keeps the new name, Escape --
/// or a blank one -- keeps the old. A project's name is shown as is.
struct WindowTitleField: View {
    @Bindable var model: AppModel
    @State private var draft = ""
    @State private var hovering = false
    @State private var clickWatch: Any?
    @State private var probe = ViewBox()
    @FocusState private var focused: Bool
    @Environment(\.controlActiveState) private var active

    private let font = Font.system(size: 15, weight: .semibold)
    private var renamable: Bool { model.isAnonymous }

    var body: some View {
        Group {
            if model.renamingTitle && renamable {
                // Sized by its text, like the name it replaces.
                ZStack(alignment: .leading) {
                    Text(verbatim: draft.isEmpty ? " " : draft + "  ")
                        .font(font).hidden()
                    TextField("Name", text: $draft)
                        .textFieldStyle(.plain)
                        .font(font)
                        .focused($focused)
                        .onSubmit { model.renameSession(draft) }
                        .onExitCommand { model.renamingTitle = false }
                }
                .frame(minWidth: 120, alignment: .leading)
                .padding(.horizontal, 6)
                .padding(.vertical, 3)
                .background(FrameProbe(box: probe))
                .background(RoundedRectangle(cornerRadius: 6)
                    .fill(Color(nsColor: .textBackgroundColor)))
                .overlay(RoundedRectangle(cornerRadius: 6)
                    .strokeBorder(Color.accentColor.opacity(0.6)))
                .onAppear {
                    draft = model.anonymousName
                    focused = true
                    clickWatch = watchClicksAway()
                }
                .onDisappear {
                    if let clickWatch { NSEvent.removeMonitor(clickWatch) }
                    clickWatch = nil
                }
                .onChange(of: focused) { _, f in
                    if !f && model.renamingTitle { model.renameSession(draft) }
                }
            } else {
                HStack(spacing: 6) {
                    Text(verbatim: model.windowTitle)
                        .foregroundStyle(active == .inactive ? .secondary
                                                             : .primary)
                    // Changes not saved yet (DESIGN §5b).
                    if model.document.dirty && !model.isAnonymous {
                        Text("Edited")
                            .foregroundStyle(.secondary)
                    }
                }
                    .font(font)
                    .lineLimit(1)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 3)
                    .background(RoundedRectangle(cornerRadius: 6)
                        .fill(Color.primary.opacity(
                            hovering && renamable ? 0.07 : 0)))
                    .contentShape(Rectangle())
                    .onHover { hovering = $0 }
                    .onTapGesture {
                        if renamable { model.renamingTitle = true }
                    }
                    .help(renamable ? Text("Click to rename")
                                    : Text(verbatim: ""))
            }
        }
        .animation(.smooth(duration: 0.15), value: hovering)
    }

    /// A click anywhere but the field -- the title bar around it, the
    /// stage, the prompt -- ends the renaming, keeping the name typed (as
    /// Return does); the click goes on to what it hit. Most of the window
    /// takes no focus of its own, so without this the field stayed open
    /// wherever one clicked -- and it could be open without the focus at
    /// all (the prompt takes it as the window appears).
    private func watchClicksAway() -> Any? {
        let box = probe
        return NSEvent.addLocalMonitorForEvents(
            matching: [.leftMouseDown, .rightMouseDown, .otherMouseDown]
        ) { event in
            guard let window = event.window, let field = box.view,
                  field.window === window else { return event }
            let inField = field.convert(field.bounds, to: nil)
                .insetBy(dx: -2, dy: -2)
                .contains(event.locationInWindow)
            guard !inField else { return event }
            MainActor.assumeIsolated {
                // The field's own focus, given up: its change keeps the
                // name (onChange below); without it, kept here.
                if let editor = window.firstResponder as? NSTextView,
                   editor.isFieldEditor,
                   let f = editor.delegate as? NSView,
                   f.isDescendant(of: field.superview ?? field) {
                    window.makeFirstResponder(nil)
                }
                if model.renamingTitle { model.renameSession(draft) }
            }
            return event
        }
    }
}

/// Where a SwiftUI view is in its window, for AppKit's events: a plain
/// view behind it, kept in a box.
final class ViewBox {
    weak var view: NSView?
}

struct FrameProbe: NSViewRepresentable {
    let box: ViewBox

    func makeNSView(context: Context) -> NSView {
        let v = NSView()
        box.view = v
        return v
    }

    func updateNSView(_ v: NSView, context: Context) { box.view = v }
}

/// Start Over (anonymous sessions only): the session -- prompt, result,
/// settings, adjustments -- is cleared and its files deleted from disk,
/// and a new one begins. Asks first when there is something to lose;
/// waits while something is running.
struct StartOverButton: View {
    @Bindable var model: AppModel
    @State private var confirming = false

    var body: some View {
        TitleBarButton(symbol: "arrow.counterclockwise", title: "Start Over",
                       help: "Start over — clear this session and delete its images") {
            if model.sessionHasWork {
                confirming = true
            } else {
                model.startOver()
            }
        }
        .titleBarCapsule()
        .disabled(model.isBusy)
        .confirmationDialog("Start over?", isPresented: $confirming) {
            Button("Start Over", role: .destructive) { model.startOver() }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("The prompt, the result and the settings are cleared, and this session's images are deleted from disk. Save anything you want to keep first.")
        }
    }
}

/// The groups right of the title, each its own glass capsule.
struct TitleBarControls: View {
    @Bindable var model: AppModel
    let scheme: ColorScheme

    var body: some View {
        GlassEffectContainer(spacing: 10) {
            HStack(spacing: 10) {
                if model.isAnonymous {
                    StartOverButton(model: model)
                }
                editorGroups
            }
        }
        // Its ideal size, always: the toolbar sizes the item from it.
        .fixedSize()
    }

    /// What acts on the editor: zoom, the ground, markup, the inspector
    /// and share, the search.
    @ViewBuilder
    private var editorGroups: some View {
                ZoomGroup(model: model)

                TitleBarButton(
                    symbol: "square.split.2x1", title: "Compare",
                    help: model.compareOn
                        ? "Hide the compare: A alone"
                        : "Compare: the picture before beside this one, or one dropped on A or B",
                    on: model.compareOn) {
                    model.toggleCompare()
                }
                .disabled(!model.canToggleCompare)
                .titleBarCapsule()

                TitleBarButton(
                    symbol: scheme == .light ? "lightbulb.max.fill" : "lightbulb",
                    title: "Light or Dark",
                    help: scheme == .light ? "Proof on a dark background"
                                           : "Proof on a light background") {
                    model.setAppearance(dark: scheme == .light)
                }
                .titleBarCapsule()

                TitleBarButton(
                    symbol: "pencil.tip.crop.circle", title: "Markup",
                    help: model.markupOpen ? "Hide the markup toolbar"
                                           : "Show the markup toolbar",
                    on: model.markupOpen) {
                    withAnimation(AppModel.motion) { model.markupOpen.toggle() }
                }
                .titleBarCapsule()

                HStack(spacing: 2) {
                    TitleBarButton(
                        symbol: "info.circle", title: "Inspector",
                        help: model.inspectorOpen ? "Hide the inspector (⌥⌘I)"
                                                  : "Show the inspector (⌥⌘I)",
                        on: model.inspectorOpen) {
                        withAnimation(AppModel.motion) {
                            model.inspectorOpen.toggle()
                        }
                    }
                    ShareButton(model: model)
                }
                .titleBarCapsule()

                TitleBarSearchField(model: model)
    }
}

/// The search field, a glass capsule like the groups beside it. It is
/// ours rather than `.searchable`'s: the system field brings its own
/// flexible gap, which left the groups stranded mid-bar. ⌘F focuses it;
/// Escape clears it.
struct TitleBarSearchField: View {
    @Bindable var model: AppModel
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
            TextField("Search", text: $model.searchText)
                .textFieldStyle(.plain)
                .focused($focused)
                .onExitCommand { model.searchText = "" }
            if !model.searchText.isEmpty {
                Button {
                    model.searchText = ""
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.plain)
                .help("Clear the search")
            }
        }
        .font(.system(size: 13))
        .padding(.horizontal, 12)
        .frame(width: 220, height: 36)
        .glassEffect(.regular, in: Capsule())
        .contentShape(Capsule())
        .onTapGesture { focused = true }
        .onChange(of: model.findRequest) { focused = true }
    }
}

extension View {
    /// A title-bar group's glass capsule, the toolbar's own height.
    func titleBarCapsule() -> some View {
        padding(.horizontal, 4)
            .frame(height: 36)
            .glassEffect(.regular, in: Capsule())
    }
}

/// The zoom group, one glass capsule divided like Preview's:
///
///   ⊖ │ 1:1 │ 154% │ fit │ ⊕
///
/// The level shows between 1:1 and fit, except at 1:1 -- then 1:1 is
/// pressed in instead. Fit is pressed in while the viewer is fitting (it
/// fits again as the window, the panels or the image change); pressing
/// it again releases it and leaves the zoom where it is.
struct ZoomGroup: View {
    @Bindable var model: AppModel

    var body: some View {
        let v = model.currentViewer
        let shown = v.zoom != nil
        HStack(spacing: 0) {
            TitleBarButton(symbol: "minus.magnifyingglass",
                        title: "Zoom Out", help: "Zoom out (⌘−)") {
                model.zoom(.zoomOut)
            }
            divider
            TitleBarButton(symbol: "1.magnifyingglass", title: "Actual Size",
                        help: "Actual pixels (⌘0)",
                        on: shown && v.isActualSize) {
                model.zoom(.actualSize)
            }
            if shown && !v.isActualSize {
                divider
                Text(verbatim: v.zoomText)
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
                    .frame(minWidth: 44)
                    .padding(.horizontal, 2)
                    .help("Zoom level")
                    .transition(.opacity)
            }
            divider
            TitleBarButton(symbol: "arrow.up.left.and.down.right.magnifyingglass",
                        title: "Zoom to Fit",
                        help: v.fitting && shown
                            ? "Fitting the window — click to hold the zoom (⌘9)"
                            : "Zoom to fit, and keep fitting (⌘9)",
                        on: shown && v.fitting) {
                model.toggleFit()
            }
            divider
            TitleBarButton(symbol: "plus.magnifyingglass",
                        title: "Zoom In", help: "Zoom in (⌘+)") {
                model.zoom(.zoomIn)
            }
        }
        .titleBarCapsule()
        .disabled(!model.canZoom)
        .animation(.smooth(duration: 0.2), value: v.isActualSize)
        .animation(.smooth(duration: 0.2), value: shown)
    }

    private var divider: some View {
        Divider().frame(height: 16).padding(.horizontal, 1)
    }
}

/// A title-bar button: a symbol that highlights on hover and, `on`,
/// shows pressed in (1:1 at actual size, fit while fitting, a panel that
/// is open).
struct TitleBarButton: View {
    let symbol: String
    let title: LocalizedStringKey
    let help: LocalizedStringKey
    var on = false
    /// A short text in place of the symbol ("A", "B").
    var text: String?
    /// The text bold and underlined: the compare side showing the
    /// current state.
    var emphasized = false
    let action: () -> Void
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        Button(action: action) {
            label
                .frame(width: 28, height: 28)
                .background {
                    Circle().fill(Color.primary.opacity(
                        on ? 0.14 : hovering && enabled ? 0.06 : 0))
                }
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(enabled ? .primary : .tertiary)
        .accessibilityLabel(Text(title))
        .accessibilityValue(emphasized ? Text("Current") : Text(verbatim: ""))
        .accessibilityAddTraits(on ? .isSelected : [])
        .help(help)
        .onHover { hovering = $0 }
        .animation(.smooth(duration: 0.15), value: hovering)
        .animation(.smooth(duration: 0.15), value: on)
    }

    @ViewBuilder
    private var label: some View {
        if let text {
            Text(verbatim: text)
                .font(.system(size: 14,
                              weight: emphasized ? .heavy : .regular))
                .underline(emphasized)
        } else {
            Image(systemName: symbol).font(.system(size: 15))
        }
    }
}

/// Tells when the pointer is over the window's title bar, so its bottom
/// edge can show (Preview's hairline). A tracking area on the window's
/// frame view over the title-bar strip -- tracking areas see the pointer
/// under the toolbar's own views, which a SwiftUI hover in the content
/// does not -- kept to that strip as the window resizes.
struct TitlebarHoverDetector: NSViewRepresentable {
    let onChange: (Bool) -> Void

    func makeNSView(context: Context) -> TrackerView {
        let v = TrackerView()
        v.onChange = onChange
        return v
    }

    func updateNSView(_ v: TrackerView, context: Context) {
        v.onChange = onChange
    }

    final class TrackerView: NSView {
        var onChange: (Bool) -> Void = { _ in }
        private var area: NSTrackingArea?
        private weak var host: NSView?
        private var observers: [NSObjectProtocol] = []
        private var inside = false

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            for o in observers { NotificationCenter.default.removeObserver(o) }
            observers = []
            guard let window else { return }
            for name in [NSWindow.didResizeNotification,
                         NSWindow.didEnterFullScreenNotification,
                         NSWindow.didExitFullScreenNotification] {
                observers.append(NotificationCenter.default.addObserver(
                    forName: name, object: window, queue: .main) {
                        [weak self] _ in
                        MainActor.assumeIsolated { self?.track() }
                    })
            }
            DispatchQueue.main.async { [weak self] in self?.track() }
        }

        /// The tracking area over the strip above the content layout rect.
        private func track() {
            if let area, let host { host.removeTrackingArea(area) }
            area = nil
            guard let window, let frameView = window.contentView?.superview
            else { return }
            let layout = window.contentLayoutRect
            let strip = CGRect(x: 0, y: layout.maxY,
                               width: window.frame.width,
                               height: max(0, window.frame.height - layout.maxY))
            guard strip.height > 0 else { return }
            let a = NSTrackingArea(
                rect: frameView.convert(strip, from: nil),
                options: [.mouseEnteredAndExited, .activeAlways],
                owner: self, userInfo: nil)
            frameView.addTrackingArea(a)
            area = a
            host = frameView
        }

        override func mouseEntered(with event: NSEvent) { set(true) }
        override func mouseExited(with event: NSEvent) { set(false) }

        private func set(_ on: Bool) {
            guard on != inside else { return }
            inside = on
            onChange(on)
        }

        isolated deinit {
            if let area, let host { host.removeTrackingArea(area) }
            for o in observers { NotificationCenter.default.removeObserver(o) }
        }
    }
}

/// Share: a menu -- Save… first, then the system's Share… (AirDrop,
/// Messages, Mail, Photos, Notes…) -- for the image or video on screen,
/// adjustments included. Save… is Valtz's own item, not a service handed
/// to the share sheet: the sheet runs out of process, and an item added
/// to it never called back.
struct ShareButton: View {
    let model: AppModel
    @State private var anchor = MenuAnchor()

    var body: some View {
        TitleBarButton(symbol: "square.and.arrow.up", title: "Share",
                       help: "Save or share the image", action: openMenu)
            // The menu opens under the button itself; the anchor is an
            // AppKit view in the button's place.
            .background(MenuAnchorView(anchor: anchor))
            .disabled(model.currentAsset?.url == nil)
            .onChange(of: model.shareRequest) { openMenu() }
    }

    /// The menu open now (a scripted snapshot closes it).
    static weak var openedMenu: NSMenu?

    private func openMenu() {
        guard model.currentAsset?.url != nil else { return }
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
            // No anchor in the title bar: under its right end.
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
        guard model.currentAsset?.url != nil else { return }
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
        note.stringValue = format == .original
            ? String(localized: "The file as it is, nothing re-encoded")
            : String(localized: "Every value the picture holds, highlights included")
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
