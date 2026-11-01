import SwiftUI

/// The window's toolbar, the system's own: the sidebar's toggle (the
/// split view's), the title -- the anonymous session's renamed in place,
/// a project's with "Edited" under it and its proxy icon (RootView's
/// WindowTitle) -- then, docked right, in groups:
///
///   zoom       zoom out, the level (a click: actual size), fit, zoom in;
///              the first to go when the window narrows
///   modes      the compare's row and markup's, above the stage
///   activity   what runs, and the machine's load (ActivityButton); on
///              every screen, the last to go
///   document   share (Save…, then the system's Share…), the inspector
///
/// What acts on the stage is hidden on the Atlas and the Log, never
/// greyed; markup and the inspector are hidden under view only too, and
/// markup in the Prompt Editor.
/// Every item is a Label: its name is VoiceOver's and the overflow
/// menu's, and the toolbar's customisation sheet lists it.
struct EditorToolbar: CustomizableToolbarContent {
    @Bindable var model: AppModel
    let layout: WorkspaceLayout

    var body: some CustomizableToolbarContent {
        ToolbarSpacer(.flexible)

        if layout.editorTools {
            ToolbarItem(id: "zoom", placement: .primaryAction) {
                ZoomGroup(model: model)
                    .disabled(model.windowGivenOver)
            }
            .visibilityPriority(.low)

            ToolbarSpacer(.fixed)

            ToolbarItem(id: "compare", placement: .primaryAction) {
                Toggle(isOn: Binding(
                    get: { model.compareOn },
                    set: { _ in model.toggleCompare() })) {
                    Label("Compare", systemImage: "square.split.2x1")
                }
                .help(model.compareOn
                      ? "Hide the compare: A alone"
                      : "Compare: the picture before beside this one, or one dropped on A or B")
                .disabled(!model.canToggleCompare || model.windowGivenOver)
            }

            if !model.viewOnly && !model.promptImmersive {
                ToolbarItem(id: "markup", placement: .primaryAction) {
                    Toggle(isOn: Binding(
                        get: { model.markupOpen },
                        set: { on in
                            withAnimation(AppModel.motion) {
                                model.markupOpen = on
                            }
                        })) {
                        Label("Markup", systemImage: "pencil.tip.crop.circle")
                    }
                    .help(model.markupOpen ? "Hide the markup toolbar"
                                           : "Show the markup toolbar")
                    .shortcutHint("⇧⌘A")
                    // An agent at work: nothing drawn on the project.
                    .disabled(model.agentHolds || model.windowGivenOver)
                }
            }

            ToolbarSpacer(.fixed)
        }

        // On every screen, and in reach while the window is given over:
        // what runs, and the machine's load, are worth watching then
        // most of all. Its popover disables its own buttons.
        ToolbarItem(id: "activity", placement: .primaryAction) {
            ActivityButton(model: model)
        }
        .visibilityPriority(.high)

        if layout.editorTools {
            ToolbarSpacer(.fixed)

            ToolbarItem(id: "share", placement: .primaryAction) {
                // Its Save… (Export…): ⌘E.
                ShareButton(model: model)
                    .shortcutHint("⌘E")
                    .disabled(model.windowGivenOver)
            }

            if !model.viewOnly {
                ToolbarItem(id: "inspector", placement: .primaryAction) {
                    Toggle(isOn: Binding(
                        get: { model.chrome.inspectorShown },
                        set: { on in
                            withAnimation(AppModel.motion) {
                                model.chrome.inspectorShown = on
                            }
                        })) {
                        Label("Inspector", systemImage: "sidebar.trailing")
                    }
                    .help(model.chrome.inspectorShown
                          ? "Hide the inspector (⌃⌘I)"
                          : "Show the inspector (⌃⌘I)")
                    .shortcutHint("⌃⌘I")
                    .disabled(model.windowGivenOver)
                }
            }
        }
    }
}

/// The zoom group, the system's segmented look:
///
///   ⊖ │ 154% │ fit │ ⊕
///
/// The level is a button: a click goes to actual size (1:1), and it
/// reads 100% there. Fit is pressed in while the viewer is fitting (it
/// fits again as the window, the columns or the image change); pressed
/// again, it lets go and leaves the zoom where it is.
struct ZoomGroup: View {
    @Bindable var model: AppModel

    var body: some View {
        let v = model.currentViewer
        let shown = v.zoom != nil
        ControlGroup {
            Button { model.zoom(.zoomOut) } label: {
                Label("Zoom Out", systemImage: "minus.magnifyingglass")
            }
            .help("Zoom out (⌘−)")
            .shortcutHint("⌘−")
            Button { model.zoom(.actualSize) } label: {
                Label {
                    Text(verbatim: shown ? v.zoomText : "100%")
                        .monospacedDigit()
                } icon: {
                    Image(systemName: "1.magnifyingglass")
                }
                .labelStyle(.titleOnly)
            }
            .accessibilityLabel(Text("Actual Size"))
            .accessibilityValue(Text(verbatim: shown ? v.zoomText : ""))
            .help("Zoom level — click for actual pixels (⌘0)")
            .shortcutHint("⌘0")
            Toggle(isOn: Binding(
                get: { shown && v.fitting },
                set: { _ in model.toggleFit() })) {
                Label("Zoom to Fit",
                      systemImage: "arrow.up.left.and.down.right.magnifyingglass")
            }
            .help(v.fitting && shown
                  ? "Fitting the window — click to hold the zoom (⌘9)"
                  : "Zoom to fit, and keep fitting (⌘9)")
            .shortcutHint("⌘9")
            Button { model.zoom(.zoomIn) } label: {
                Label("Zoom In", systemImage: "plus.magnifyingglass")
            }
            .help("Zoom in (⌘+)")
            .shortcutHint("⌘+")
        } label: {
            Label("Zoom", systemImage: "magnifyingglass")
        }
        .disabled(!model.canZoom)
    }
}
