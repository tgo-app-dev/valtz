import SwiftUI
import UniformTypeIdentifiers

/// The chips under the prompt (DESIGN §6, the dock): each a setting's
/// name and its value -- "Model  Auto (Krea 2 Turbo)", "Shape  16:9 ·
/// Large", "Length  5.2 s" -- that opens a popover of its own to change
/// it. They are always in sight, so what Start will make reads at a
/// glance, and they wrap onto a second line where the row is narrow. A
/// chip that does not apply is left out, never greyed: Length on an
/// image, Shape on a sound, all but the Agent's on the Agent.
enum GenerateChip: String, CaseIterable, Identifiable, Sendable {
    case model, shape, length, favor, seed, agent
    var id: String { rawValue }
}

extension AppModel {
    /// The chips the row shows now, in order.
    var generateChips: [GenerateChip] {
        if activeModality == .agent { return [.agent] }
        var c: [GenerateChip] = [.model]
        if !speaks && activeModality != .audio { c.append(.shape) }
        if activeModality == .audio
            || (activeModality == .video && modalityRuns) {
            c.append(.length)
        }
        // A script of voices (Drama 3) has nothing to favor: no steps,
        // no weights to choose.
        if !(speaks && speechSpeakers > 1) { c.append(.favor) }
        c.append(.seed)
        return c
    }

    /// A chip's name, as the row and VoiceOver say it.
    func chipLabel(_ c: GenerateChip) -> String {
        switch c {
        case .model: String(localized: "Model")
        case .shape: String(localized: "Shape")
        case .length:
            speaks && speechSpeakers > 1 ? String(localized: "Voices")
                                         : String(localized: "Length")
        case .favor: String(localized: "Favor")
        case .seed: String(localized: "Seed")
        case .agent: String(localized: "Agent")
        }
    }

    /// A chip's value: the summary the composer's tray once showed in one
    /// line, split per chip.
    func chipValue(_ c: GenerateChip) -> String {
        switch c {
        case .model: return modelChipValue
        case .shape:
            // A size typed by hand: the size; a preset, its shape and
            // class -- "16:9 · Large", "Square · Draft".
            if customSize != nil {
                let d = dimensions
                return "\(d.width) × \(d.height)"
            }
            let shape = orientation == .square ? Orientation.square.label
                : aspectRatio.label(orientation)
            let size = sizeChoice?.label(activeModality) ?? ""
            return shape + " · " + size
        case .length:
            if speaks {
                return speechSpeakers > 1 ? speechVoicesSummary
                                          : speechLengthText
            }
            return activeModality == .audio ? songLengthText
                                            : clipDurationText
        case .favor: return preference.label
        case .seed:
            return seedText.isEmpty ? String(localized: "Random") : seedText
        case .agent:
            let steps = (agentSettings?.review ?? true)
                ? String(localized: "Ask First")
                : String(localized: "Run on Their Own")
            return agentBackendLabel + " · " + steps
        }
    }

    /// What runs: "Edit · Qwen-Image 2.1", "Compose · …", "From picture ·
    /// MiniMax H3 FL2VA", "First and last frames · …" or the model --
    /// Auto's pick named ("Auto (Krea 2 Turbo)") while Auto is chosen.
    var modelChipValue: String {
        guard modalityRuns else {
            return String(localized: "Not available yet")
        }
        // A clip from references (Ref2VA): continuing one, or drawing on
        // pictures, clips and sounds.
        if usesReferences {
            let name = modelName(videoReferenceModel)
            return promptAttachments.contains(where: \.continues)
                ? String(localized: "Continue · \(name)")
                : String(localized: "References · \(name)")
        }
        // Sound: a song, or speech -- in the row's voice.
        if activeModality == .audio {
            let name = modelName(audioModel)
            return speaks && speechVoice != nil
                ? String(localized: "Voice · \(name)") : name
        }
        // A clip opening on a picture -- and ending on another (FL2VA).
        if let k = keyframes {
            return k.last != nil
                ? String(localized: "First and last frames · \(imageModelName)")
                : String(localized: "From picture · \(imageModelName)")
        }
        guard willEdit else {
            return modelChoice.isEmpty ? ModelPicker.autoLabel(self)
                                       : modelName(modelChoice)
        }
        // The base fills the size set here (its own, unless changed);
        // pictures without one are composed into a picture of it.
        return baseAttachment != nil
            ? String(localized: "Edit · \(editModelName)")
            : String(localized: "Compose · \(editModelName)")
    }

    /// A chip pressed: its popover opened, or -- open as the press began
    /// (its popover shuts itself on that very press) -- shut.
    func pressChip(_ c: GenerateChip) {
        let wasOpen = chipOpenAtPress == c || openChip == c
        chipOpenAtPress = nil
        openChip = wasOpen ? nil : c
    }

    /// Notes, at each press, which chip's popover was open (installed
    /// once, by the row).
    func watchChipPresses() {
        guard chipPressWatch == nil else { return }
        chipPressWatch = NSEvent.addLocalMonitorForEvents(
            matching: .leftMouseDown
        ) { [weak self] event in
            MainActor.assumeIsolated {
                if let self { self.chipOpenAtPress = self.openChip }
            }
            return event
        }
    }

    /// A chip's popover, as a binding: open while it is the chip open.
    func chipBinding(_ c: GenerateChip) -> Binding<Bool> {
        Binding { [weak self] in
            self?.openChip == c
        } set: { [weak self] open in
            guard let self else { return }
            if open {
                openChip = c
            } else if openChip == c {
                openChip = nil
            }
        }
    }

    /// Attach: media into the row -- or, for a song, lyrics from a text
    /// file. The row's paperclip, and the Prompt Editor's.
    func attachFromPanel() {
        let panel = NSOpenPanel()
        panel.directoryURL = panelFolder(.attach)
        // A song's: lyrics, a text file a song (vpipe's songs-from-lyrics),
        // into the prompt after its words.
        if activeModality == .audio {
            panel.allowedContentTypes = [.plainText, .text]
            panel.message = String(localized: "Choose a lyrics file")
            if panel.runModal() == .OK, let url = panel.url {
                rememberPanel(.attach, chose: url)
                withAnimation(Self.motion) { addLyrics(from: url) }
            }
            return
        }
        panel.allowsMultipleSelection = true
        // A clip draws on sounds too (Ref2VA: a voice, a song).
        panel.allowedContentTypes = activeModality == .video
            ? [.image, .movie, .audio] : [.image, .movie]
        if panel.runModal() == .OK {
            rememberPanel(.attach, chose: panel.urls.first)
            addReferences(panel.urls)
        }
    }
}

/// The row under the prompt: the chips, wrapping where the card is
/// narrow, then -- on the row's right, its last line -- Attach, the
/// assistant and Start.
struct GenerateChipRow: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(alignment: .bottom, spacing: VZ.Space.m) {
            ChipFlow(spacing: VZ.Space.xs + 2, lineSpacing: VZ.Space.xs + 2) {
                ForEach(model.generateChips) { chip($0) }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            HStack(spacing: VZ.Space.xs) {
                AttachMenu(model: model, size: 26)
                assistant
                StartButton(model: model)
                    .padding(.leading, VZ.Space.xs)
            }
            .foregroundStyle(.secondary)
            .fixedSize()
        }
        .onAppear { model.watchChipPresses() }
        // Custom's options shut with Favor's popover.
        .onChange(of: model.openChip) { _, c in
            if c != .favor { model.showsTuning = false }
        }
    }

    @ViewBuilder
    private func chip(_ c: GenerateChip) -> some View {
        let open = model.chipBinding(c)
        // Favor's is AppKit's popover, semi-transient: it stays open
        // while Valtz is left, for weights dragged in from the Finder.
        ChipMenu(label: model.chipLabel(c), value: model.chipValue(c),
                 isOpen: open, appKit: c == .favor,
                 press: { model.pressChip(c) }) {
            switch c {
            case .model: ModelChipPopover(model: model)
            case .shape: ChipForm { ShapeRows(model: model) }
            case .length: ChipForm { LengthRows(model: model) }
            case .favor: FavorPopover(model: model)
            case .seed: SeedPopover(model: model)
            case .agent: ChipForm { AgentSettingsRows(model: model) }
            }
        }
        // The memory card's suggestion rings the chip it points at.
        .suggestMark(model.memorySuggests(c))
        .transition(.opacity)
    }

    /// The assistant: the prompt improved, or the agent's suggestion.
    private var assistant: some View {
        Button(action: model.assistantPressed) {
            Group {
                if model.enhanceJob != nil {
                    ProgressView().controlSize(.small)
                } else {
                    Image(systemName: "sparkles")
                }
            }
            .frame(width: 26, height: 26)
            .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .disabled(!model.assistantEnabled)
        .help(model.assistantHelp)
        .accessibilityLabel(Text("Assistant"))
    }
}

/// The chips laid out left to right, onto another line where the next
/// would not fit; a chip wider than the row is given the row (its value
/// is cut in the middle).
struct ChipFlow: Layout {
    var spacing: CGFloat
    var lineSpacing: CGFloat

    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews,
                      cache: inout ()) -> CGSize {
        let width = proposal.width ?? .infinity
        let lines = arrange(subviews, width: width)
        let height = lines.last.map { $0.y + $0.height } ?? 0
        let widest = lines.map(\.width).max() ?? 0
        return CGSize(width: proposal.width ?? widest, height: height)
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize,
                       subviews: Subviews, cache: inout ()) {
        for line in arrange(subviews, width: bounds.width) {
            for item in line.items {
                subviews[item.index].place(
                    at: CGPoint(x: bounds.minX + item.x,
                                y: bounds.minY + line.y
                                    + (line.height - item.size.height) / 2),
                    proposal: ProposedViewSize(item.size))
            }
        }
    }

    private struct Line {
        var y: CGFloat
        var height: CGFloat = 0
        var width: CGFloat = 0
        var items: [(index: Int, x: CGFloat, size: CGSize)] = []
    }

    private func arrange(_ subviews: Subviews, width: CGFloat) -> [Line] {
        var lines: [Line] = []
        var line = Line(y: 0)
        for (i, v) in subviews.enumerated() {
            var s = v.sizeThatFits(.unspecified)
            if s.width > width {
                s = v.sizeThatFits(ProposedViewSize(width: width,
                                                    height: nil))
            }
            let x = line.items.isEmpty ? 0 : line.width + spacing
            if !line.items.isEmpty, x + s.width > width {
                lines.append(line)
                line = Line(y: line.y + line.height + lineSpacing)
                line.items.append((i, 0, s))
                line.width = s.width
            } else {
                line.items.append((i, x, s))
                line.width = x + s.width
            }
            line.height = max(line.height, s.height)
        }
        if !line.items.isEmpty { lines.append(line) }
        return lines
    }
}

/// A chip's popover: its rows in a grouped form, the popover's width.
struct ChipForm<Rows: View>: View {
    var width: CGFloat = 400
    @ViewBuilder var rows: () -> Rows

    var body: some View {
        Form { rows() }
            .formStyle(.grouped)
            .scrollDisabled(true)
            .frame(width: width)
            .fixedSize(horizontal: false, vertical: true)
    }
}

/// Model ▾: Auto -- the core's pick for what Start does -- or a model;
/// with nothing here that makes the modality, what to download.
private struct ModelChipPopover: View {
    @Bindable var model: AppModel

    var body: some View {
        ChipForm {
            ModelRows(model: model)
        }
    }
}

private struct ModelRows: View {
    @Bindable var model: AppModel

    var body: some View {
        Section {
            ModelPicker(model: model)
        } footer: {
            if !model.modalityRuns {
                if model.activeModality == .audio {
                    Text("Audio generation needs YuE2 or MiniMax-Music3 (songs), or Drama 3 or MOSS-TTS (speech): download one in Settings › Capabilities.")
                } else {
                    Text("Video generation needs MiniMax H3 FL2VA or Ref2VA: download one in Settings › Capabilities.")
                }
            }
        }
    }
}

/// Shape ▾: a picture's or a clip's orientation, its aspect ratio, its
/// size -- a class, or one typed.
struct ShapeRows: View {
    @Bindable var model: AppModel

    var body: some View {
        Section {
            Picker("Orientation", selection: orientation) {
                ForEach(Orientation.allCases) { o in
                    Label(o.label, systemImage: o.symbol).tag(o)
                }
            }
            .pickerStyle(.segmented)
            // A preset that is not square has a ratio to pick; a custom
            // size has its own.
            if model.showsAspectRatio {
                Picker("Aspect Ratio", selection: $model.aspectRatio) {
                    ForEach(AspectRatio.allCases) { r in
                        Text(verbatim: r.label(model.orientation)).tag(r)
                    }
                }
                .pickerStyle(.segmented)
            }
            Picker("Size", selection: sizeChoice) {
                ForEach(model.sizeClasses) { c in
                    Text(verbatim: c.label(model.activeModality))
                        .tag(Optional(c))
                        .help(model.activeModality == .image ? c.tip : "")
                }
            }
            .pickerStyle(.segmented)
            .suggestMark(model.memorySuggests(.shape))
            LabeledContent("Pixels") {
                SizeField(model: model)
            }
        } footer: {
            // Past 768P (DESIGN §4q): drafted there and regenerated at the
            // size -- or, its refiner not here, what would.
            if let note = model.regenerateNote {
                Text(note)
            }
        }
    }

    /// Shown as chosen, or as a custom size is shaped; choosing one with a
    /// custom size turns it, or goes back to a preset.
    private var orientation: Binding<Orientation> {
        Binding {
            model.shownOrientation
        } set: { o in
            withAnimation(AppModel.motion) { model.setOrientation(o) }
        }
    }

    private var sizeChoice: Binding<SizeClass?> {
        Binding {
            model.sizeChoice
        } set: { c in
            withAnimation(AppModel.motion) { model.sizeChoice = c }
        }
    }
}

/// Length ▾: a clip's length (and, drawn from a song, its soundtrack);
/// speech's length, or a script's voices; a song's score, length and
/// lyrics.
struct LengthRows: View {
    @Bindable var model: AppModel

    var body: some View {
        if model.speaks {
            speechRows
        } else if model.activeModality == .audio {
            songRows
        } else {
            clipRows
        }
    }

    /// A clip's length: a preset in seconds, or one typed in the field
    /// under them (frames or seconds); either is made as the nearest
    /// length the model makes, which the field shows.
    @ViewBuilder
    private var clipRows: some View {
        Section {
            Picker("Length", selection: clipChoice) {
                ForEach(AppModel.clipLengths, id: \.self) { s in
                    Text("\(s) s").tag(Optional(s))
                }
            }
            .pickerStyle(.segmented)
            .suggestMark(model.memorySuggests(.length))
            LabeledContent("Frames or Seconds") {
                ClipLengthField(model: model)
            }
        }
        // A clip from references with a song among them: its soundtrack
        // is the song as it is (a music video), or the one generated with
        // the picture.
        if model.hasSongReference {
            Section {
                Picker("Soundtrack", selection: $model.keepSongSound) {
                    Text("The song").tag(true)
                        .help("The song in the prompt, as it is, under the clip")
                    Text("Generated").tag(false)
                        .help("The sound made with the clip")
                }
                .pickerStyle(.segmented)
            }
        }
    }

    /// Speech's rows (MOSS-TTS, DESIGN §4g): whose voice it speaks in --
    /// the prompt's sound -- and about how long it runs. A script of
    /// several speakers (Drama 3): its voices, one a speaker; it decides
    /// its own length.
    @ViewBuilder
    private var speechRows: some View {
        if model.speechSpeakers > 1 {
            Section {
                LabeledContent("Voices") {
                    Text(verbatim: model.speechVoiceText)
                        .lineLimit(3)
                        .help("Each sound (or clip) in the prompt is a speaker's voice, in order -- the first is the first name to speak. It is cloned from its first seconds and the words said in them, which Valtz hears first when it has no transcript")
                }
            } footer: {
                Text("Directions in brackets steer each line -- [sighs], [whispering], <emphasis>…</emphasis> -- and a scene's names are its speakers. The model decides the length")
            }
        } else {
            Section {
                LabeledContent("Voice") {
                    Text(verbatim: model.speechVoiceText)
                        .lineLimit(2)
                        .help("A sound (or a clip) in the prompt is the voice: up to its first 12 seconds are cloned")
                }
                Picker("Length", selection: speechLength) {
                    Text("Auto").tag(0)
                        .help("As long as the words take")
                    ForEach(AppModel.speechLengths, id: \.self) { s in
                        Text("\(s) s").tag(s)
                            .help(String(localized: "About \(s) seconds: the model paces the words to it"))
                    }
                }
                .pickerStyle(.segmented)
            } footer: {
                Text("The prompt's fields (Instruction, Quality, Language …) steer the whole of it")
            }
        }
    }

    /// A song's rows: the score it plans first, how long it may run, and
    /// what it will sing -- the prompt's lyrics, under section headers.
    @ViewBuilder
    private var songRows: some View {
        // The score it plans first (YuE2); a model that plans none
        // (MiniMax-Music3) has no such row.
        if model.songPlansScore {
            Section {
                Picker("Score", selection: $model.songPlan) {
                    ForEach(SongPlan.allCases) { p in
                        Text(verbatim: p.label).tag(p).help(p.hint)
                    }
                }
                .pickerStyle(.segmented)
            } footer: {
                Text(verbatim: model.songPlan.hint)
            }
        }
        // The model decides the length; a choice here is the most it may
        // run (a song past it is cut there).
        Section {
            Picker("Length", selection: songLength) {
                Text("Auto").tag(0)
                    .help("As long as the model makes it")
                ForEach(AppModel.songLengths, id: \.self) { s in
                    let t = AppModel.songTime(Double(s))
                    Text(verbatim: t).tag(s)
                        .help(String(localized: "At most \(t): a song past it is cut there"))
                }
            }
            .pickerStyle(.segmented)
            LabeledContent("Lyrics") {
                HStack(spacing: VZ.Space.s) {
                    Text(verbatim: model.songLyricsText)
                        .lineLimit(1)
                    Button("Add Lyrics…", action: model.attachFromPanel)
                        .help("Add a lyrics file to the prompt: a text file, sectioned with [Verse] and [Chorus]")
                }
            }
        } footer: {
            Text(verbatim: model.songLengthHint)
        }
    }

    /// The preset in use; nil while the length is typed.
    private var clipChoice: Binding<Int?> {
        Binding {
            model.clipSeconds
        } set: { s in
            guard let s else { return }
            model.clipSeconds = s
            model.clipCustomFrames = nil
        }
    }

    /// Speech's Length row: 0 is Auto.
    private var speechLength: Binding<Int> {
        Binding {
            model.speechSeconds ?? 0
        } set: { s in
            model.speechSeconds = s == 0 ? nil : s
        }
    }

    /// A song's Length row: 0 is Auto.
    private var songLength: Binding<Int> {
        Binding {
            model.songSeconds ?? 0
        } set: { s in
            model.songSeconds = s == 0 ? nil : s
        }
    }
}

/// Favor ▾: fewer steps, the model's, more -- or Tune, one's own
/// settings, whose options open under it in the same popover. As large
/// as it lays out, up to the screen's room, then it scrolls.
private struct FavorPopover: View {
    @Bindable var model: AppModel
    @State private var size: CGSize = .zero

    var body: some View {
        ScrollView(.vertical) {
            // As tall as it lays out at its width: measured with none
            // proposed, Tune's wrapping notes were a line each.
            AtIdealWidth(minWidth: 360) {
                VStack(alignment: .leading, spacing: VZ.Space.m) {
                    FavorRows(model: model)
                    if model.preference == .custom {
                        Divider()
                        TuningPanel(model: model, embedded: true)
                    }
                }
                .padding(VZ.Space.l)
            }
            .onGeometryChange(for: CGSize.self) { $0.size }
                action: { size = $0 }
        }
        .scrollBounceBehavior(.basedOnSize)
        .frame(width: size.width > 0 ? size.width : nil,
               height: size.height > 0 ? min(size.height, room) : nil)
        .animation(AppModel.motion, value: model.preference)
    }

    /// 680 points at most, or what the screen leaves over the dock,
    /// where it stands.
    private var room: CGFloat {
        guard let w = NSApp.mainWindow, let screen = w.screen?.visibleFrame
        else { return 680 }
        let above = w.frame.maxY - screen.minY - 140
        return max(320, min(680, above))
    }
}

/// Favor's choice and what it does.
private struct FavorRows: View {
    @Bindable var model: AppModel

    var body: some View {
        LabeledContent("Favor") {
            Picker("Favor", selection: preference) {
                ForEach(Preference.allCases) { p in
                    Text(verbatim: p.label).tag(p).help(p.hint)
                }
            }
            .labelsHidden()
            .pickerStyle(.segmented)
            .fixedSize()
        }
        Text(verbatim: model.preferenceHint)
            .font(.vzCallout)
            .foregroundStyle(.secondary)
            .fixedSize(horizontal: false, vertical: true)
    }

    private var preference: Binding<Preference> {
        Binding { model.preference } set: { model.choosePreference($0) }
    }
}

/// Seed ▾: a number typed, or one picked at random; empty, a random one
/// each time.
private struct SeedPopover: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: VZ.Space.s) {
            TextField("Random", text: $model.seedText)
                .textFieldStyle(.roundedBorder)
                .frame(width: 160)
                .accessibilityLabel(Text("Seed"))
            Button {
                model.seedText = String(Int.random(in: 0..<2_147_483_647))
            } label: {
                Image(systemName: "dice")
            }
            .buttonStyle(.borderless)
            .help("Pick a seed")
            .accessibilityLabel(Text("Pick a seed"))
        }
        .padding(VZ.Space.l)
    }
}

/// The Prompt Editor's settings (its toolbar's button): every chip's
/// rows in one popover, until Phase 5 gives the editor the dock's own
/// chips.
struct GenerateSettingsList: View {
    @Bindable var model: AppModel
    @State private var size: CGSize = .zero

    var body: some View {
        ScrollView(.vertical) {
            AtIdealWidth(minWidth: 420) {
                Form {
                    ForEach(model.generateChips) { c in
                        switch c {
                        case .model: ModelRows(model: model)
                        case .shape: ShapeRows(model: model)
                        case .length: LengthRows(model: model)
                        case .favor:
                            Section {
                                FavorRows(model: model)
                                if model.preference == .custom {
                                    TuningPanel(model: model, embedded: true)
                                }
                            }
                        case .seed:
                            Section {
                                TextField("Seed", text: $model.seedText,
                                          prompt: Text("Random"))
                            }
                        case .agent: AgentSettingsRows(model: model)
                        }
                    }
                }
                .formStyle(.grouped)
                .scrollDisabled(true)
            }
            .onGeometryChange(for: CGSize.self) { $0.size }
                action: { size = $0 }
        }
        .scrollBounceBehavior(.basedOnSize)
        .frame(width: size.width > 0 ? size.width : nil,
               height: size.height > 0 ? min(size.height, 680) : nil)
        .animation(AppModel.motion, value: model.showsTuning)
    }
}

/// The paperclip: a menu -- a file browsed for (a song's lyrics in the
/// Audio tab), or a sound recorded from the microphone or the system's
/// audio, or a still or a clip from the camera (AttachCapture), each
/// into the prompt's row. The chip row's, and the Prompt Editor's.
struct AttachMenu: View {
    @Bindable var model: AppModel
    var size: CGFloat = 22

    var body: some View {
        Menu {
            Button(model.activeModality == .audio ? "Add Lyrics…"
                                                  : "Browse…") {
                model.attachFromPanel()
            }
            Divider()
            Section("Record Sound") {
                Button("From the Microphone") {
                    model.attachSound(system: false)
                }
                Button("From the System’s Audio") {
                    model.attachSound(system: true)
                }
                // Takes recorded onto the timeline on the stage as it
                // plays (VoiceOver.swift): the audio target's alone.
                if model.activeModality == .audio {
                    Button("Voice-over…") { model.openVoiceOver() }
                        .disabled(!model.canVoiceOver)
                        .help("Record over the timeline or sound on the stage as it plays: each take goes on it where it was heard")
                }
            }
            .disabled(model.activeModality == .image)
            Section("Camera") {
                Button("Take a Photo or Video…") { model.attachCamera() }
            }
        } label: {
            Image(systemName: "paperclip")
                .frame(width: size, height: size)
                .contentShape(Rectangle())
        }
        .menuStyle(.borderlessButton)
        .menuIndicator(.hidden)
        .fixedSize()
        // File › Attach Media…: its Browse….
        .shortcutHint("⇧⌘I")
        // An agent at work: nothing imported into the project it holds.
        .disabled(model.attachCapture != nil || model.agentHolds)
        .help("Attach: a file, a sound recorded, a photo or a video from the camera")
        .accessibilityLabel(Text("Attach"))
    }
}

extension View {
    /// A setting to change for a refused generation to fit (the memory
    /// card's suggestion): ringed in orange while it is as it failed.
    func suggestMark(_ on: Bool) -> some View {
        self
            .background {
                if on {
                    Capsule()
                        .strokeBorder(Color.orange, lineWidth: 1.5)
                        .background(Color.orange.opacity(0.12),
                                    in: Capsule())
                        .padding(.horizontal, -3)
                        .padding(.vertical, -2)
                }
            }
            .help(on ? String(localized: "Lower this for the clip to fit in memory") : "")
    }

    /// Start's look in the capture card -- prominent glass, a capsule,
    /// large: Record, Take Photo, Stop and Attach, over the stage.
    func startButtonStyle() -> some View {
        buttonStyle(.glassProminent)
            .buttonBorderShape(.capsule)
            .controlSize(.large)
    }

    /// Start pressed by ⌘↩ (`count`, AppModel.startFlash): the button
    /// lit up once, a lighter colour -- a key shows no press as a click
    /// does. Laid over the button in its `shape`; without one, the
    /// button itself brightened.
    func startFlash(_ count: Int, in shape: AnyShape? = nil) -> some View {
        modifier(StartFlash(count: count, shape: shape))
    }
}

private struct StartFlash: ViewModifier {
    let count: Int
    let shape: AnyShape?
    @State private var lit = false

    func body(content: Content) -> some View {
        Group {
            if let shape {
                content.overlay {
                    shape.fill(Color.white.opacity(lit ? 0.3 : 0))
                        .allowsHitTesting(false)
                }
            } else {
                content.brightness(lit ? 0.15 : 0)
            }
        }
        .onChange(of: count) {
            withAnimation(.easeOut(duration: 0.08)) {
                lit = true
            } completion: {
                withAnimation(.easeIn(duration: 0.3)) { lit = false }
            }
        }
    }
}

/// Start -- always: a task runs now, or after the tasks before it (the
/// TASK QUEUE, DESIGN §3a) -- and, while the stage watches a task, its
/// progress and Stop beside it.
struct StartButton: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 0) {
            stop(shown: model.isGenerating)
            Button(action: model.startPressed) {
                Text("Start")
                    .frame(minWidth: 56)
            }
            .buttonStyle(.borderedProminent)
            .buttonBorderShape(.capsule)
            .keyboardShortcut(.return, modifiers: .command)
            .shortcutHint("⌘↩")
            .disabled(!model.canStart)
            .startFlash(model.startFlash, in: AnyShape(Capsule()))
            .help(model.tasks.isEmpty
                  ? "Start (⌘↩)"
                  : "Start (⌘↩): queued after the tasks before it")
        }
        .controlSize(.regular)
    }

    /// Stop is always THERE, shown while the stage watches a task -- never
    /// put in and taken out. Taken out with a transition of its own, it
    /// was now and then left on screen after its task ended (refused for
    /// memory, something on the stage): its arc going round, a click on it
    /// doing nothing, the view a removal had not let go of. Its look, its
    /// place and whether it takes a click are now what the state says,
    /// whatever an animation does on the way.
    private func stop(shown: Bool) -> some View {
        Button(action: model.stop) {
            ZStack {
                ActivityRing(fraction: progress, running: shown)
                Image(systemName: "stop.fill")
                    .font(.system(size: 7))
            }
        }
        .buttonStyle(.bordered)
        .buttonBorderShape(.circle)
        .keyboardShortcut(".", modifiers: .command)
        .help("Stop the task on the stage (⌘.)")
        .accessibilityLabel(Text("Stop"))
        .shortcutHint("⌘.")
        .disabled(!shown)
        .environment(\.shortcutHintsHidden, !shown)
        .scaleEffect(shown ? 1 : 0.02)
        .opacity(shown ? 1 : 0)
        .animation(shown ? Self.arrives : Self.goes, value: shown)
        .allowsHitTesting(shown)
        .accessibilityHidden(!shown)
        .padding(.trailing, 6)
        .fixedSize()
        // Hidden, it takes no room: the row's motion moves Start's
        // neighbours as it comes and goes.
        .frame(width: shown ? nil : 0)
    }

    /// The phase's count; a spinner while nothing is counted.
    private var progress: Double? { model.generationPhase?.fraction }

    /// Stop forms where it stands: magnified from a dot at its centre,
    /// a little past its size, settling -- quicker than the motion it
    /// arrives with, which moves the row around it. Going, it shrinks
    /// back into the dot, fading.
    private static let arrives = Animation.spring(
        duration: 0.32 * AppModel.animationScale, bounce: 0.35)
    private static let goes = Animation.easeIn(
        duration: 0.15 * AppModel.animationScale)
}

/// The model field: AUTO first -- the core's pick for what Start does
/// (generate, or edit when pictures are attached), named beside it --
/// then the installed models for the modality. One field for both: a
/// chosen model generates, and edits if it can.
struct ModelPicker: View {
    @Bindable var model: AppModel

    var body: some View {
        Picker("Model", selection: $model.modelChoice) {
            Text(verbatim: Self.autoLabel(model)).tag("")
            let options = model.modelOptions(for: model.activeModality)
            if !options.isEmpty { Divider() }
            ForEach(options, id: \.model) { o in
                Text(verbatim: Self.label(o)).tag(o.model)
            }
        }
    }

    /// "Auto (Krea 2 Turbo)": what Auto would run for the prompt as it
    /// is -- its edit pick when pictures are attached, else its generate
    /// pick.
    static func autoLabel(_ model: AppModel) -> String {
        // A clip from references: Auto's video edit pick (Ref2VA); sound
        // in a voice the row holds, its audio edit pick (speech).
        let edits = model.activeModality == .video ? model.usesReferences
            : model.activeModality == .audio ? model.speechVoice != nil
                && !model.autoPick("edit").isEmpty
            : model.hasEditPictures && !model.autoPick("edit").isEmpty
        let op = edits ? "edit" : "generate"
        let id = model.autoPick(op)
        // A quantized variant goes by its source's name.
        let name = model.modelName(model.baseModel(id))
        if id.isEmpty { return String(localized: "Auto") }
        return model.autoWaits(op)
            ? String(localized: "Auto (\(name) · Downloading)")
            : String(localized: "Auto (\(name))")
    }

    /// An option's name -- "Krea 2 Turbo · Downloading" while it downloads.
    static func label(_ o: ModelOption) -> String {
        o.downloading == true && o.state != "installed"
            ? String(localized: "\(o.name) · Downloading") : o.name
    }
}

/// A clip's length, as it will be made ("124 frames · 5.2 s"); type
/// another -- "100 frames", "7.5 s" -- to make it custom. It shows the
/// length again on Return or when it loses focus, as the model makes it,
/// or as it was if it could not be read; and follows the presets unless
/// a length is being typed.
private struct ClipLengthField: View {
    @Bindable var model: AppModel
    @State private var text = ""
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("Frames or seconds", text: $text)
            .textFieldStyle(.roundedBorder)
            .labelsHidden()
            .font(.callout.monospacedDigit())
            .multilineTextAlignment(.center)
            .frame(width: 150)
            .focused($focused)
            .onSubmit(commit)
            .onChange(of: focused) { _, now in
                if !now { commit() }
            }
            .onAppear { show(model.clipLengthText) }
            .onChange(of: model.clipLengthText) { _, s in
                if text == lastShown { show(s) }
            }
            .help("Type a length: frames (124 frames) or seconds (5 s)")
    }

    private func show(_ s: String) {
        text = s
        lastShown = s
    }

    private func commit() {
        if text != lastShown {
            withAnimation(AppModel.motion) {
                if !model.setClipLength(text) { NSSound.beep() }
            }
        }
        show(model.clipLengthText)
    }
}

/// The output size, as it will be made; type another to make it custom:
/// "1200x800", "1200 × 800" or "1200*800". It shows the size again on
/// Return or when it loses focus -- snapped to the model's grid, or as it
/// was if it could not be read -- and follows the other controls unless a
/// size is being typed.
private struct SizeField: View {
    @Bindable var model: AppModel
    @State private var text = ""
    /// What the field last showed: text that differs is being typed.
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("Width × height", text: $text)
            .textFieldStyle(.roundedBorder)
            .labelsHidden()
            .font(.callout.monospacedDigit())
            .multilineTextAlignment(.center)
            .frame(width: 120)
            .focused($focused)
            .onSubmit(commit)
            .onChange(of: focused) { _, now in
                if !now { commit() }
            }
            .onAppear { show(shown) }
            .onChange(of: shown) { _, s in
                if text == lastShown { show(s) }
            }
            .help("Type a size: width × height, as 1200x800 or 1200*800")
    }

    private func show(_ s: String) {
        text = s
        lastShown = s
    }

    /// Pixel sizes are identifiers, not quantities: no digit grouping.
    private var shown: String {
        let d = model.dimensions
        return "\(d.width) × \(d.height)"
    }

    private func commit() {
        if text != lastShown {
            withAnimation(AppModel.motion) {
                if !model.setCustomSize(text) { NSSound.beep() }
            }
        }
        show(shown)
    }
}
