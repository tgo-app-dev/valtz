import SwiftUI

/// The window: the current mode full-bleed under a Preview-style title
/// bar (TitleBarContent), the inspector column on the right when asked
/// for, and a navigation panel that stays hidden until ☰ asks for it.
struct RootView: View {
    @Bindable var model: AppModel
    @Environment(\.colorScheme) private var scheme

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                main
                // A plain column rather than SwiftUI's .inspector, which
                // over-sizes the stage.
                if model.inspectorOpen && model.screen == .editor {
                    InspectorColumn(model: model)
                }
            }
            // The machine's load and thermal state, the window's width
            // (View › Show Status Bar).
            if model.showsStatusBar {
                StatusBar(model: model)
                    .transition(.move(edge: .bottom).combined(with: .opacity))
            }
        }
        // The title bar's bottom edge: while the pointer is over the title
        // bar, and always while the inspector or the markup toolbar sits
        // under it.
        .overlay(alignment: .top) {
            let editor = model.screen == .editor
            let shown = model.titlebarHover || model.screen == .log
                || (editor && (model.inspectorOpen || model.markupOpen
                               || model.promptImmersive))
            Rectangle()
                .fill(Color(nsColor: .separatorColor))
                .frame(height: 1)
                .opacity(shown ? 1 : 0)
                .animation(.easeInOut(duration: 0.2), value: shown)
                .allowsHitTesting(false)
        }
        .background {
            TitlebarHoverDetector { model.titlebarHover = $0 }
                .frame(width: 0, height: 0)
        }
        .onChange(of: model.searchText) { model.searchChanged() }
        // Unsaved changes: the dot in the window's close button.
        .onChange(of: model.document.dirty, initial: true) { _, dirty in
            NSApp.windows.first { $0.isVisible }?.isDocumentEdited =
                dirty && !model.isAnonymous
        }
        // The markup toolbar put away, or another picture on the stage:
        // what it had selected is let go -- not when the stage now shows
        // the picture it is on (the edited copy the first mark on a flat
        // picture makes: its new text lost the selection, and the focus).
        .onChange(of: model.markupOpen) { _, open in
            if !open { model.commitSelection() }
        }
        .onChange(of: model.stagePicture?.id) { _, id in
            if model.markup.selectionAsset != id { model.commitSelection() }
        }
        // The window, for a project's size (AppModel.editorWindow).
        .background(WindowReader { model.editorWindow = $0 })
        .navigationTitle(model.windowTitle)
        .toolbar { TitleBarContent(model: model, scheme: scheme) }
        .toolbarBackgroundVisibility(.hidden, for: .windowToolbar)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    /// The current mode -- under the markup toolbar, a row of its own
    /// below the title bar, as Preview's -- with the navigation panel
    /// over it. Immersive prompt editing takes the editor's place, under
    /// its own toolbar.
    private var main: some View {
        ZStack(alignment: .topLeading) {
            switch model.screen {
            case .editor:
                VStack(spacing: 0) {
                    // The Prompt Editor's toolbar, or the markup toolbar:
                    // the editor grows the prompt card itself (SimpleView).
                    if model.promptImmersive {
                        PromptBar(model: model)
                            .transition(.move(edge: .top)
                                .combined(with: .opacity))
                    } else if model.markupOpen {
                        MarkupBar(model: model)
                            .transition(.move(edge: .top)
                                .combined(with: .opacity))
                    }
                    SimpleView(model: model)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            case .log:
                LogView(model: model)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }

            if model.navOpen {
                // Click anywhere else to put the panel away.
                Color.black.opacity(0.001)
                    .onTapGesture { model.toggleNav() }
                NavPanel(model: model)
                    .padding(.top, 14)
                    .padding(.leading, 10)
                    .padding(.bottom, 10)
                    .transition(.move(edge: .leading)
                        .combined(with: .opacity))
            }
        }
    }
}

/// The inspector at the window's right, with its edge.
struct InspectorColumn: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 0) {
            Divider()
            InspectorView(model: model)
                .frame(width: 300)
        }
        .transition(.move(edge: .trailing).combined(with: .opacity))
    }
}

extension Color {
    /// The grey of Valtz's panels -- the composer's tray, the inspector:
    /// opaque, a shade off the window in either appearance.
    static let valtzPanel = Color(nsColor: NSColor(name: "valtz.panel") { a in
        a.bestMatch(from: [.darkAqua, .vibrantDark]) != nil
            ? NSColor(srgbRed: 0.165, green: 0.165, blue: 0.175, alpha: 1)
            : NSColor(srgbRed: 0.953, green: 0.953, blue: 0.961, alpha: 1)
    })
}

/// The ☰ column: a narrow glass strip of icons -- the screens (the
/// editor, the log) at the top, Settings (⌘,) at the foot -- set well
/// below the title bar's hairline.
struct NavPanel: View {
    @Bindable var model: AppModel
    @Environment(\.openSettings) private var openSettings

    var body: some View {
        VStack(spacing: 6) {
            ForEach(AppScreen.allCases) { s in
                NavRow(title: s.label, symbol: s.symbol,
                       selected: model.screen == s) {
                    model.show(s)
                }
            }
            Spacer(minLength: 20)
            NavRow(title: String(localized: "Settings"), symbol: "gearshape",
                   selected: false) {
                model.toggleNav()
                openSettings()
            }
        }
        .padding(6)
        .frame(width: 52)
        .frame(maxHeight: .infinity, alignment: .top)
        .glassEffect(.regular, in: RoundedRectangle(cornerRadius: 16,
                                                    style: .continuous))
    }
}

/// One of its icons; its name in the tooltip and to VoiceOver.
private struct NavRow: View {
    let title: String   // already localized
    let symbol: String
    let selected: Bool
    let action: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 16))
                .foregroundStyle(selected ? Color.accentColor : .secondary)
                .frame(width: 40, height: 36)
            .contentShape(Rectangle())
            .background(
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .fill(selected ? Color.accentColor.opacity(0.16)
                          : hovering ? Color.primary.opacity(0.06)
                          : .clear))
        }
        .buttonStyle(.plain)
        .help(title)
        .accessibilityLabel(Text(verbatim: title))
        .onHover { hovering = $0 }
        .animation(.smooth(duration: 0.15), value: hovering)
    }
}

/// The window a view is in, told when it has one.
private struct WindowReader: NSViewRepresentable {
    let found: (NSWindow) -> Void

    func makeNSView(context: Context) -> Probe {
        let v = Probe()
        v.found = found
        return v
    }

    func updateNSView(_ v: Probe, context: Context) {}

    final class Probe: NSView {
        var found: ((NSWindow) -> Void)?

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let window { found?(window) }
        }
    }
}
