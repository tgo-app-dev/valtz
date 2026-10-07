import SwiftUI

/// The Prompt Editor's toolbar: a narrow row under the title bar's
/// hairline, as the markup toolbar's, its controls centred over the
/// prompt card. Markdown's bold, italic and underline on the selection
/// (⌘B, ⌘I, ⌘U in the text), and whether Markdown is drawn as it reads;
/// what Start makes and the model it runs -- the generation card's own
/// model field, one choice (the assistant will go by it too) -- and the
/// card's settings in a popover; attach, the assistant, Start. The prompt card's own corner button leaves.
struct PromptBar: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 12) {
            HStack(spacing: 2) {
                format(.bold, symbol: "bold",
                       help: String(localized: "Bold (⌘B)"))
                format(.italic, symbol: "italic",
                       help: String(localized: "Italic (⌘I)"))
                format(.underline, symbol: "underline",
                       help: String(localized: "Underline (⌘U)"))
            }
            markdownToggle
            Divider().frame(height: 18)
            Picker("Create", selection: Binding(
                get: { model.modality },
                set: { m in
                    withAnimation(AppModel.motion) { model.setModality(m) }
                })) {
                ForEach(Modality.allCases) { m in
                    Image(systemName: m.symbol)
                        .accessibilityLabel(Text(m.label))
                        .tag(m)
                }
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            .help("What Start makes")
            ModelPicker(model: model)
                .help("The model Start runs -- the generation card's, one choice")
            settingsButton
            Divider().frame(height: 18)
            HStack(spacing: 2) {
                AttachMenu(model: model)
                Button(action: model.assistantPressed) {
                    Group {
                        if model.enhanceJob != nil {
                            ProgressView().controlSize(.mini)
                        } else {
                            Image(systemName: "sparkles")
                        }
                    }
                    .frame(width: 26, height: 22)
                    .contentShape(Rectangle())
                }
                .disabled(!model.assistantEnabled)
                .help(model.assistantHelp)
            }
            .buttonStyle(.borderless)
            startButton
        }
        .controlSize(.small)
        .padding(.horizontal, 12)
        .frame(height: 34)
        .frame(maxWidth: .infinity)
        .background(.bar)
        .overlay(alignment: .bottom) { Divider() }
    }

    /// The generation card's settings -- size, length, Favor, seed -- in
    /// a popover; Tune's options open inside it.
    private var settingsButton: some View {
        Button {
            model.showsGenerationSettings.toggle()
        } label: {
            Image(systemName: "slider.horizontal.3")
                .frame(width: 26, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .help("Generation settings: what the generation card sets")
        .accessibilityLabel(Text("Generation settings"))
        .popover(isPresented: $model.showsGenerationSettings,
                 arrowEdge: .bottom) {
            GenerationSettingsPopover(model: model)
        }
        // Shut, it takes Tune's options with it.
        .onChange(of: model.showsGenerationSettings) { _, open in
            if !open { model.showsTuning = false }
        }
    }

    /// One style's button: on the text view showing the prompt.
    private func format(_ style: MarkdownStyle, symbol: String,
                        help: String) -> some View {
        Button {
            guard let tv = model.promptTextView else { return }
            tv.window?.makeFirstResponder(tv)
            tv.toggleMarkdown(style)
        } label: {
            Image(systemName: symbol)
                .frame(width: 26, height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .help(help)
        .accessibilityLabel(Text(verbatim: help))
    }

    /// Markdown drawn as it reads, or the text as typed.
    private var markdownToggle: some View {
        let on = model.promptMarkdown
        return Button {
            model.promptMarkdown.toggle()
        } label: {
            Label("Markdown", systemImage: on ? "eye"
                  : "chevron.left.forwardslash.chevron.right")
                .labelStyle(.titleAndIcon)
                .padding(.horizontal, 6)
                .frame(height: 22)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .background(RoundedRectangle(cornerRadius: 5)
            .fill(on ? Color.accentColor.opacity(0.22) : .clear))
        .foregroundStyle(on ? Color.accentColor : Color.primary)
        .help(on ? "Markdown drawn as it reads: click to see it as typed"
                 : "Markdown as typed: click to draw it as it reads")
    }

    /// Start (⌘↩) -- queued behind the tasks before it -- and Stop (⌘.)
    /// beside it while the stage watches a task.
    @ViewBuilder
    private var startButton: some View {
        if model.isGenerating {
            Button(action: model.stop) {
                HStack(spacing: 6) {
                    ProgressView(value: model.generationPhase?.fraction)
                        .progressViewStyle(.circular)
                        .controlSize(.mini)
                    Text("Stop")
                }
            }
            .keyboardShortcut(".", modifiers: .command)
            .help("Stop the task on the stage (⌘.)")
        }
        Button("Start", action: model.start)
            .buttonStyle(.borderedProminent)
            .keyboardShortcut(.return, modifiers: .command)
            .disabled(!model.canStart)
            .help(model.tasks.isEmpty
                  ? "Start (⌘↩)"
                  : "Start (⌘↩): queued after the tasks before it")
    }
}
