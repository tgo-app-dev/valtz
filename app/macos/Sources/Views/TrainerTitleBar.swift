import AppKit
import SwiftUI

/// The title bar while the LoRA TRAINER is shown (DESIGN §8c): no
/// project's. Its name is the run's -- "LoRA" and the name -- whose panel
/// renames it and sets where its folder goes; its controls are the
/// training's: Start (Resume) / Pause, Stop, the checkpoint saved now
/// (its name and step), the previews sampled now.
struct TrainerTitleField: View {
    @Bindable var trainer: TrainerModel
    @State private var hovering = false
    @State private var open = false
    @Environment(\.controlActiveState) private var active

    var body: some View {
        HStack(spacing: 6) {
            Text("LoRA")
                .foregroundStyle(.secondary)
            Text(verbatim: trainer.name.isEmpty
                 ? String(localized: "Untitled") : trainer.name)
                .foregroundStyle(active == .inactive ? .secondary : .primary)
            Image(systemName: "chevron.down")
                .font(.system(size: 10, weight: .semibold))
                .foregroundStyle(.secondary)
                .opacity(hovering || open ? 1 : 0)
        }
        .font(.system(size: 15, weight: .semibold))
        .lineLimit(1)
        .padding(.horizontal, 6)
        .padding(.vertical, 3)
        .background(RoundedRectangle(cornerRadius: 6)
            .fill(Color.primary.opacity(hovering || open ? 0.07 : 0)))
        .contentShape(Rectangle())
        .onHover { hovering = $0 }
        .onTapGesture { open.toggle() }
        .appKitPopover(isPresented: $open, arrowEdge: .bottom) {
            TrainerFilePanel(trainer: trainer)
        }
        .help(Text("Its name, and where it is saved"))
        .animation(.smooth(duration: 0.15), value: hovering)
    }
}

/// The name's panel: the run's name (its file's), and WHERE its folder
/// is -- a run kept, its own; a new one, the folder chosen (the models
/// folder's trained/ by default).
private struct TrainerFilePanel: View {
    @Bindable var trainer: TrainerModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            TextField("Name", text: $trainer.name,
                      prompt: Text("its file's name"))
                .textFieldStyle(.roundedBorder)
                .font(.body.weight(.semibold))
                .disabled(trainer.training)
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 8,
                 verticalSpacing: 8) {
                GridRow {
                    Text("Where").foregroundStyle(.secondary)
                        .gridColumnAlignment(.trailing)
                    whereMenu
                }
            }
            Text(trainer.currentRun != nil
                 ? "A run of this name is kept there: Start trains on it."
                 : "A new run's folder goes there, named as the LoRA is.")
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(14)
        .frame(width: 300)
        .controlSize(.small)
    }

    private var whereMenu: some View {
        let here = trainer.whereShown
        return Menu {
            Button {
                let run = trainer.currentRun.map {
                    URL(fileURLWithPath: $0.folder)
                }
                NSWorkspace.shared.activateFileViewerSelecting(
                    [run ?? URL(fileURLWithPath: here)])
            } label: {
                Label("Show in Finder", systemImage: "folder")
            }
            Divider()
            Button("The Models Folder") { trainer.folder = "" }
                .disabled(trainer.currentRun != nil || trainer.folder.isEmpty)
            Button("Other…") { choose() }
                .disabled(trainer.currentRun != nil)
        } label: {
            Label(FileManager.default.displayName(atPath: here),
                  systemImage: "folder.fill")
        }
        .disabled(trainer.training)
        .help(here)
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = String(localized: "Choose")
        panel.message = String(localized: "Choose where the LoRA's folder goes.")
        panel.directoryURL = URL(fileURLWithPath: trainer.whereShown)
        guard panel.runModal() == .OK, let url = panel.url else { return }
        trainer.folder = url.path
    }
}

/// The training's controls, a glass capsule each as the editor's groups
/// -- after the stage's zoom, as the editor's title bar has it.
struct TrainerTitleControls: View {
    @Bindable var trainer: TrainerModel
    /// Edit › Find (⌘F): the search field takes the keyboard.
    var find = 0

    var body: some View {
        HStack(spacing: 8) {
            TrainerZoomGroup(trainer: trainer)
            HStack(spacing: 0) {
                TitleBarButton(symbol: "photo", title: "Sample",
                               help: LocalizedStringKey(sampleHelp),
                               on: trainer.stageShows == .sample) {
                    trainer.stageShows = .sample
                }
                TitleBarButton(symbol: "sparkles", title: "Preview",
                               help: LocalizedStringKey(previewHelp),
                               on: trainer.stageShows == .preview) {
                    trainer.stageShows = .preview
                }
            }
            .titleBarCapsule()
            HStack(spacing: 0) {
                if trainer.training {
                    TitleBarButton(symbol: "pause.fill", title: "Pause",
                                   help: "Pause: its state is kept, and Resume trains on from there") {
                        trainer.pause()
                    }
                } else {
                    TitleBarButton(symbol: "play.fill",
                                   title: trainer.resumable ? "Resume"
                                                            : "Start Training",
                                   help: trainer.blocker.map {
                                       LocalizedStringKey($0)
                                   } ?? (trainer.resumable
                                         ? "Resume the training from where it was paused"
                                         : "Encode the pictures, then train")) {
                        trainer.start()
                    }
                    .disabled(trainer.blocker != nil)
                }
                TitleBarButton(symbol: "stop.fill", title: "Stop Training",
                               help: "Stop: the run ends, its adapter kept -- Start then trains afresh") {
                    trainer.endRun()
                }
                .disabled(!trainer.training && !trainer.resumable)
            }
            .titleBarCapsule()
            HStack(spacing: 2) {
                SaveNowButton(trainer: trainer)
                TitleBarButton(symbol: "photo.badge.arrow.down",
                               title: "Preview Now",
                               help: "The preview prompts sampled now with the adapter as it is") {
                    trainer.command("preview")
                }
                .disabled(trainer.phase != "train" || !trainer.training
                          || trainer.commanding)
            }
            .titleBarCapsule()
            TrainerSearchField(trainer: trainer, find: find)
        }
    }

    /// What each shows, said in its tooltip: what the stage has of it.
    private var sampleHelp: String {
        trainer.selectedItem.map {
            String(localized: "The stage shows the sample selected: \($0.name)")
        } ?? String(localized: "The stage shows the sample selected in Samples")
    }
    private var previewHelp: String {
        let p = trainer.shownPreview ?? trainer.previews.last
        return p.map {
            String(localized: "The stage shows a preview: \(URL(fileURLWithPath: $0).lastPathComponent)")
        } ?? String(localized: "The stage shows a preview -- previews come at each checkpoint")
    }
}

/// The samples searched -- their file names and captions -- at the title
/// bar's right, as the editor's search is; typing in it shows Samples.
private struct TrainerSearchField: View {
    @Bindable var trainer: TrainerModel
    let find: Int
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
            TextField("Search Samples", text: $trainer.query)
                .textFieldStyle(.plain)
                .focused($focused)
                .onExitCommand { trainer.query = "" }
            if !trainer.query.isEmpty {
                Button {
                    trainer.query = ""
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
        .frame(width: 200, height: 36)
        .glassEffect(.regular, in: Capsule())
        .contentShape(Capsule())
        .onTapGesture { focused = true }
        .onChange(of: trainer.query) { _, q in
            if !q.isEmpty { trainer.tab = .samples }
        }
        .onChange(of: find) { focused = true }
        .help("Search the samples by file name and caption")
    }
}

/// Save a checkpoint now -- named with its step, as its file is.
private struct SaveNowButton: View {
    @Bindable var trainer: TrainerModel
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled

    private var can: Bool {
        trainer.training && trainer.phase == "train" && !trainer.commanding
    }

    var body: some View {
        Button {
            trainer.command("save")
        } label: {
            HStack(spacing: 6) {
                Image(systemName: "square.and.arrow.down")
                    .font(.system(size: 14))
                Text(verbatim: label)
                    .font(.callout.monospacedDigit())
                    .lineLimit(1)
            }
            .padding(.horizontal, 10)
            .frame(height: 28)
            .background(Capsule().fill(Color.primary.opacity(
                hovering && can ? 0.06 : 0)))
            .contentShape(Capsule())
        }
        .buttonStyle(.plain)
        .foregroundStyle(can ? .primary : .tertiary)
        .disabled(!can)
        .help("Save a checkpoint now, with its state, between two steps")
        .onHover { hovering = $0 }
    }

    private var label: String {
        let n = trainer.name.isEmpty ? "LoRA" : trainer.name
        return trainer.training && trainer.phase == "train"
            ? String(format: "%@-%06d", n, trainer.done) : n
    }
}

/// The trainer stage's zoom, the editor's ZoomGroup in look and keys:
///   ⊖ │ 1:1 │ 154% │ fit │ ⊕
private struct TrainerZoomGroup: View {
    @Bindable var trainer: TrainerModel

    var body: some View {
        let actual = trainer.stageZoom == 1
        let fitting = trainer.stageZoom == nil
        HStack(spacing: 0) {
            TitleBarButton(symbol: "minus.magnifyingglass",
                           title: "Zoom Out", help: "Zoom out (⌘−)") {
                trainer.zoom(.zoomOut)
            }
            .shortcutHint("⌘−")
            divider
            TitleBarButton(symbol: "1.magnifyingglass", title: "Actual Size",
                           help: "Actual pixels (⌘0)", on: actual) {
                trainer.zoom(.actualSize)
            }
            .shortcutHint("⌘0")
            if !actual {
                divider
                Text(verbatim: "\(Int((trainer.stageScale * 100).rounded()))%")
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .contentTransition(.numericText())
                    .frame(minWidth: 44)
                    .padding(.horizontal, 2)
                    .help("Zoom level")
            }
            divider
            TitleBarButton(symbol: "arrow.up.left.and.down.right.magnifyingglass",
                           title: "Zoom to Fit",
                           help: "Zoom to fit, and keep fitting (⌘9)",
                           on: fitting) {
                trainer.zoom(.fit)
            }
            .shortcutHint("⌘9")
            divider
            TitleBarButton(symbol: "plus.magnifyingglass",
                           title: "Zoom In", help: "Zoom in (⌘+)") {
                trainer.zoom(.zoomIn)
            }
            .shortcutHint("⌘+")
        }
        .titleBarCapsule()
        .disabled(trainer.stagePath == nil)
        .animation(.smooth(duration: 0.2), value: actual)
    }

    private var divider: some View {
        Divider().frame(height: 16).padding(.horizontal, 1)
    }
}
