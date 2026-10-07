import AppKit
import SwiftUI

/// The inspector (the title bar's ⓘ): a grey column on the window's
/// right, under the title bar, in Preview's small type. Its sections:
///   Information  what is on the stage: its pixels, how it was made, the
///                adjustments shown on it, where its file is;
///   Layers       the picture on the stage's layer stack (LayersSection);
///   Assets       everything the project holds, in folders -- its history
///                too: each result as made, what it was made from, to
///                compare on the stage (AssetsSection).
/// Shown ONE AT A TIME, picked in a navigation row and taking the whole
/// height (the default) -- or STACKED: each under a header that folds it
/// to that one row (a click), the open ones sharing the height, as an
/// image editor's panels; so the assets can be read beside the layers,
/// and dropped on the very layer they go to. An icon switches between
/// the two (`AppModel.inspectorStacked`): at the top right beside the
/// navigation row, or -- stacked, where the first row is the first
/// section's header -- centred at the foot.
struct InspectorView: View {
    @Bindable var model: AppModel

    /// The section shown: in the Prompt Editor, Layers or Assets -- what
    /// the prompt draws on, dragged into it -- whatever was asked for.
    private var section: InspectorTab {
        model.promptImmersive && model.inspectorTab != .layers
            ? .assets : model.inspectorTab
    }

    /// The sections offered: the Prompt Editor's two, else the three.
    private var sections: [InspectorTab] {
        model.promptImmersive ? [.layers, .assets] : InspectorTab.allCases
    }

    var body: some View {
        VStack(spacing: 0) {
            if model.inspectorStacked {
                stacked
                // The way back to one section at a time, at the foot.
                modeButton
                    .frame(maxWidth: .infinity)
                    .padding(.vertical, 6)
                    .overlay(alignment: .top) { Divider() }
            } else {
                HStack(spacing: 8) {
                    Picker("Section", selection: Binding(
                        get: { section },
                        set: { model.inspectorTab = $0 })) {
                        ForEach(sections) { t in
                            Label(t.label, systemImage: t.symbol).tag(t)
                        }
                    }
                    .pickerStyle(.segmented)
                    .labelsHidden()
                    // Made again when its sections change (the Prompt
                    // Editor's two, the stage's three): the segmented
                    // control keeps the widths it had, and the new labels
                    // were cut to "…".
                    .id(model.promptImmersive)
                    modeButton
                }
                .padding(.leading, 16)
                .padding(.trailing, 10)
                .padding(.top, 10)
                .padding(.bottom, 6)
                content(section, stacked: false)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .font(.system(size: 11))
        .controlSize(.small)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        // Tucked under the title bar: the grey starts below it, and
        // nothing scrolls up behind it.
        .background { Color.valtzPanel }
        .clipped()
    }

    /// One at a time, or stacked: the icon at the top right, or at the
    /// foot.
    private var modeButton: some View {
        let on = model.inspectorStacked
        return Button {
            withAnimation(AppModel.motion) { model.inspectorStacked.toggle() }
        } label: {
            Image(systemName: "rectangle.split.1x2")
                .font(.system(size: 13))
                .foregroundStyle(on ? Color.accentColor : Color.secondary)
                .frame(width: 24, height: 22)
                .background(RoundedRectangle(cornerRadius: 5)
                    .fill(on ? Color.accentColor.opacity(0.18) : .clear))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(on ? "Show one section at a time"
                 : "Stack the sections, each folding to its header")
        .accessibilityLabel(Text(on ? "Show one section at a time"
                                    : "Stack the sections"))
    }

    @ViewBuilder
    private func content(_ t: InspectorTab, stacked: Bool) -> some View {
        switch t {
        case .info: information
        case .layers: LayersSection(model: model, notes: !stacked)
        case .assets: AssetsSection(model: model, notes: !stacked)
        }
    }

    /// The sections stacked, each under its header; the open ones share
    /// the height by their weights -- the top edge of a header between
    /// two open ones is a divider, dragged to give one more of it.
    private var stacked: some View {
        StackedPanels {
            ForEach(sections) { t in
                let open = !model.inspectorFolded.contains(t)
                PanelHeader(tab: t, open: open) {
                    withAnimation(AppModel.motion) {
                        model.toggleInspectorFold(t)
                    }
                }
                .overlay(alignment: .top) {
                    if let pair = resizePair(at: t) {
                        PanelDivider(model: model, above: pair.above,
                                     below: pair.below)
                    }
                }
                // Over the section above it: its divider is on top.
                .zIndex(1)
                if open {
                    content(t, stacked: true)
                        .padding(.top, 4)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .clipped()
                        .onGeometryChange(for: CGFloat.self) {
                            $0.size.height
                        } action: { model.inspectorHeights[t] = $0 }
                        .layoutValue(key: PanelWeight.self,
                                     value: model.inspectorWeight(t))
                        .transition(.opacity)
                }
            }
        }
    }

    /// The two open sections the top edge of `t`'s header lies between:
    /// the last open one above it, the first open one from it down.
    private func resizePair(at t: InspectorTab)
        -> (above: InspectorTab, below: InspectorTab)? {
        guard let i = sections.firstIndex(of: t) else { return nil }
        let open = { (s: InspectorTab) in !model.inspectorFolded.contains(s) }
        guard let above = sections[..<i].last(where: open),
              let below = sections[i...].first(where: open) else {
            return nil
        }
        return (above, below)
    }

    private var information: some View {
        Group {
            if let a = model.currentAsset {
                ScrollView {
                    details(a)
                        .padding(.horizontal, 16)
                        .padding(.vertical, 14)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else if model.projectViews.isEmpty, model.projectId != nil {
                // An empty project: set up here, or by its first result.
                ScrollView {
                    VStack(alignment: .leading, spacing: 10) {
                        section("Project") {
                            Text("Its first result makes the project a still or a timeline. Or set it up now:")
                                .font(.system(size: 11))
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        ProjectSetupForm(model: model)
                    }
                    .padding(.horizontal, 16)
                    .padding(.vertical, 14)
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
            } else {
                ContentUnavailableView(
                    "Nothing to inspect", systemImage: "info.circle",
                    description: Text(
                        "Generate an image to see its details here."))
                    .controlSize(.small)
            }
        }
    }

    /// A composition's kind and frame, its rate and length; the project's
    /// with New Project…, another with Use as Project.
    @ViewBuilder
    private func composition(_ a: AssetDTO) -> some View {
        section(a.isProject ? "Project" : "Composition") {
            row("Type") {
                Text(a.kind == "audio" ? "Sound"
                     : a.isTimeline ? "Timeline" : "Still")
            }
            if a.kind != "audio", let f = a.ownFrame {
                row("Frame") { Text(verbatim: "\(f.w) × \(f.h)") }
            }
            if let r = a.compositionRate, a.kind != "audio" {
                row("Rate") {
                    Text(verbatim: String(format: "%g", r.fps) + " fps")
                }
            }
            if let n = a.length, let r = a.compositionRate {
                row("Length") {
                    Text(verbatim: String(format: "%.2f",
                                          Double(n) / r.fps) + " s")
                }
            }
            row("Layers") { Text(verbatim: String(a.layerStack.count)) }
            if !(a.transitions ?? []).isEmpty {
                row("Transitions") {
                    Text(verbatim: String((a.transitions ?? []).count))
                }
            }
        }
        // The project's: what exporting it writes, and its timeline's rate.
        if a.isProject {
            section("Output") {
                OutputFields(
                    kind: a.kind == "audio" ? .sound
                        : a.isTimeline ? .timeline : .still,
                    output: Binding(get: { model.projectOutput },
                                    set: { model.setProjectOutput($0) }),
                    rateLocked: a.layerStack.contains { !$0.isEmpty })
                Text("What exporting the project writes. Each asset keeps its own; clips at another rate are resampled to the project's.")
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        HStack {
            if a.isProject {
                NewProjectButton(model: model)
            } else {
                Button("Use as Project") { model.useAsProject(a) }
                    .help("Make this composition the project's")
            }
        }
        .controlSize(.small)
    }

    private func details(_ a: AssetDTO) -> some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(verbatim: a.name)
                .font(.system(size: 12, weight: .semibold))
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)

            if a.isComposition {
                composition(a)
                if a.kind != "audio" {
                    CanvasSection(model: model, asset: a)
                }
            } else if let info = a.info, a.kind == "audio" {
                section("Sound") {
                    if let s = info.seconds {
                        row("Duration") {
                            Text(verbatim: String(format: "%.2f", s) + " s")
                        }
                    }
                    row("Format") { Text(verbatim: info.codecName.isEmpty
                                         ? a.url?.pathExtension.uppercased()
                                             ?? "" : info.codecName) }
                }
            } else if let info = a.info {
                let f = info.frame
                section("Image") {
                    row("Dimensions") { Text(verbatim: "\(f.w) × \(f.h)") }
                    row("Color") { Text(verbatim: f.color.label) }
                    row("Depth") { Text("\(f.bits)-bit") }
                    row("Format") { Text(verbatim: info.codecName) }
                }
                if a.kind == "image" || a.kind == "video" {
                    CanvasSection(model: model, asset: a)
                }
                let exif = ExifRow.rows(info.exif ?? [:])
                if !exif.isEmpty {
                    section("EXIF") {
                        ForEach(exif) { r in
                            row(verbatim: r.label) {
                                Text(verbatim: r.value)
                                    .textSelection(.enabled)
                                    .fixedSize(horizontal: false,
                                               vertical: true)
                            }
                        }
                    }
                }
            }
            if a.isComposition {
                // What its layer 0 shows, and how that was made.
                let bg = a.layerStack.first { $0.id.isEmpty }?.source
                    .flatMap { id in model.assets.first { $0.id == id } }
                if let bg {
                    section("Layer 0") {
                        Text(verbatim: bg.name)
                            .lineLimit(2)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                }
                if let bg, let r = bg.recipe {
                    recipe(r, timing: bg.timing)
                }
            } else if let r = a.recipe {
                recipe(r, timing: a.timing, score: a.score)
            }
            if !model.adjustments.isIdentity {
                section("Adjustments") {
                    ForEach(ImageAdjustments.Key.allCases.filter {
                        model.adjustments[$0] != 0
                    }) { k in
                        row(Text(verbatim: k.label)) {
                            Text(verbatim: k.display(model.adjustments[k]))
                                .monospacedDigit()
                        }
                    }
                }
                Text("Shown on the stage, and included when you share or save. The original is kept.")
                    .font(.system(size: 10))
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if let url = a.url, a.standIn != true {
                section("File") {
                    row("Name") {
                        Text(verbatim: url.lastPathComponent)
                            .lineLimit(1)
                            .truncationMode(.middle)
                    }
                    if a.linked, let state = a.linkState {
                        row("Linked original") { Text(verbatim: state) }
                    }
                }
                Button("Show in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([url])
                }
            }
        }
    }

    @ViewBuilder
    private func recipe(_ r: RecipeDTO, timing: TimingDTO?,
                        score: String? = nil) -> some View {
        let song = r.op == "generate-audio"
        section("How it was made") {
            row("Operation") {
                switch r.op {
                case "edit-image": Text("Edit")
                case "generate-video": Text("Video")
                case "generate-audio": Text("Song")
                case "generate-speech": Text("Speech")
                case "modify": Text("Modified copy")
                case "capture": Text("Capture")
                case "instance": Text("Placed from an asset")
                case "project": Text("Project")
                default: Text("Generation")
                }
            }
            // What it was made from, by name: an original, what was
            // captured or placed.
            if ["modify", "capture", "instance"].contains(r.op),
               let from = r.inputs?.first(where: {
                   $0.role == "base" || $0.role == "own" })?.asset,
               let a = model.assets.first(where: { $0.id == from }) {
                row("From") {
                    Text(verbatim: a.name)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
            }
            if !r.model.isEmpty {
                row("Model") {
                    Text(verbatim: model.catalog.first { $0.id == r.model }?.name
                         ?? r.model)
                }
            }
            // A clip's run-time adapter (the Turbo LoRA), its length and
            // the picture it opened on.
            if let lora = r.params?.lora {
                row("Adapter") {
                    Text(verbatim: model.catalog.first { $0.id == lora }?.name
                         ?? lora)
                }
            }
            if let frames = r.params?.frames {
                row("Frames") { Text(verbatim: "\(frames)") }
            }
            if let first = r.inputs?.first(where: { $0.role == "first" })?
                .asset {
                row("First frame") {
                    Text(verbatim: model.assets.first { $0.id == first }?.name
                         ?? first)
                }
            }
            // A song's plan, and the most it was let run.
            if song, let p = r.params?.cot.flatMap(SongPlan.init) {
                row("Score") { Text(verbatim: p.label) }
            }
            if song, let m = r.params?.maxSeconds, m > 0 {
                row("Length") {
                    let t = AppModel.songTime(m)
                    Text("Up to \(t)")
                }
            }
            if let steps = r.params?.steps {
                row("Steps") { Text(verbatim: "\(steps)") }
            }
            if let seed = r.params?.seed {
                row("Seed") { Text(verbatim: seed).textSelection(.enabled) }
            }
            let refs = r.inputs?.filter { $0.role == "reference" }.count ?? 0
            if refs > 0 {
                row("References") { Text(verbatim: "\(refs)") }
            }
            if let d = r.createdDate {
                row("Created") {
                    Text(d, format: .dateTime.day().month().year()
                        .hour().minute())
                }
            }
            // How long it took, from Start (out of the queue) to the
            // result, and each phase of it.
            if let s = timing?.seconds {
                row("Generation time") {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: Self.duration(s)).monospacedDigit()
                        let sound = song || r.op == "generate-speech"
                        let phases = Self.phases(timing?.phases ?? [:],
                                                 kind: sound ? .audio : .image)
                        if !phases.isEmpty {
                            Text(verbatim: phases)
                                .font(.system(size: 10))
                                .foregroundStyle(.secondary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                }
            }
        }
        // Its prompt, as captured -- its media named by position in the
        // row (DESIGN §10c) -- to use again; or, made before prompts were
        // kept, the words the model read.
        if let pid = r.inputs?.first(where: { $0.role == "prompt" })?.asset,
           let pa = model.assets.first(where: { $0.id == pid }),
           let text = pa.text {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Prompt").foregroundStyle(.secondary)
                    Spacer()
                    Button("Use in Prompt") { model.usePrompt(pa) }
                        .buttonStyle(.link)
                        .help("Put this prompt in the box: its references bind to the row's media by position")
                }
                Text(verbatim: text)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } else if let prompt = r.params?.prompt, !prompt.isEmpty {
            VStack(alignment: .leading, spacing: 4) {
                Text("Prompt").foregroundStyle(.secondary)
                Text(verbatim: prompt)
                    .textSelection(.enabled)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        // The score the song followed (ABC): kept with it, to read, copy,
        // or edit and sing again.
        if let score, !score.isEmpty {
            VStack(alignment: .leading, spacing: 4) {
                HStack {
                    Text("Score").foregroundStyle(.secondary)
                    Spacer()
                    Button("Copy") {
                        NSPasteboard.general.clearContents()
                        NSPasteboard.general.setString(score, forType: .string)
                    }
                    .buttonStyle(.link)
                    .help("Copy the score (ABC notation)")
                }
                ScrollView {
                    Text(verbatim: score)
                        .font(.system(size: 10, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 160)
            }
        }
    }

    /// "15.1 sec", "1 min, 22 sec".
    static func duration(_ s: Double, narrow: Bool = false) -> String {
        let width: Duration.UnitsFormatStyle.UnitWidth =
            narrow ? .narrow : .abbreviated
        if s < 60 {
            return Duration.milliseconds(Int64((s * 1000).rounded()))
                .formatted(.units(allowed: [.seconds], width: width,
                                  fractionalPart: .show(length: 1)))
        }
        return Duration.seconds(s.rounded())
            .formatted(.units(allowed: [.hours, .minutes, .seconds],
                              width: width))
    }

    /// "Preparing 12.0s · Generating 51.2s · Decoding 9.1s": the phases
    /// in the order they run, a blink of one left out.
    static func phases(_ p: [String: Double],
                       kind: Modality = .image) -> String {
        JobPhase.order.compactMap { k in
            guard let s = p[k], s >= 0.05 else { return nil }
            return JobPhase.name(k, kind: kind) + " "
                + duration(s, narrow: true)
        }.joined(separator: " · ")
    }

    /// A titled group of label / value rows, labels right-aligned in one
    /// column as in Preview's inspector.
    private func section<Rows: View>(
        _ title: LocalizedStringKey, @ViewBuilder rows: () -> Rows
    ) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.system(size: 11, weight: .semibold))
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 8,
                 verticalSpacing: 4) {
                rows()
            }
        }
    }

    private func row<Value: View>(
        _ label: LocalizedStringKey, @ViewBuilder value: () -> Value
    ) -> some View {
        row(Text(label), value: value)
    }

    /// A label already in the UI's language (String(localized:)).
    private func row<Value: View>(
        verbatim label: String, @ViewBuilder value: () -> Value
    ) -> some View {
        row(Text(verbatim: label), value: value)
    }

    private func row<Value: View>(
        _ label: Text, @ViewBuilder value: () -> Value
    ) -> some View {
        GridRow {
            label
                .foregroundStyle(.secondary)
                .gridColumnAlignment(.trailing)
                .frame(minWidth: 72, alignment: .trailing)
            value()
        }
    }
}

/// One EXIF row of the inspector: the fields the core read (media/exif.h)
/// under labels a person reads, values formatted as a camera app shows
/// them -- 1/125 s, f/8, 35 mm. Only what the file has; in a fixed order,
/// the camera's story first, then who made the file.
struct ExifRow: Identifiable {
    let id: String
    let label: String
    let value: String

    static func rows(_ e: [String: ExifValue]) -> [ExifRow] {
        var out: [ExifRow] = []
        func add(_ id: String, _ label: String, _ value: String?) {
            if let value, !value.isEmpty {
                out.append(ExifRow(id: id, label: label, value: value))
            }
        }
        func text(_ k: String) -> String? { e[k]?.text }
        func num(_ k: String) -> Double? { e[k]?.number }

        add("camera", String(localized: "Camera"),
            joined(text("Make"), text("Model")))
        add("lens", String(localized: "Lens"),
            joined(text("LensMake"), text("LensModel")))
        add("taken", String(localized: "Taken"),
            text("DateTimeOriginal").map(date))
        add("exposure", String(localized: "Exposure"),
            num("ExposureTime").map(exposure))
        add("aperture", String(localized: "Aperture"),
            num("FNumber").map { "f/" + decimal($0, 1) })
        add("iso", String(localized: "ISO"),
            num("ISOSpeedRatings").map { decimal($0, 0) })
        add("focal", String(localized: "Focal length"),
            num("FocalLength").map { f in
                let mm = decimal(f, 1) + " mm"
                guard let ff = num("FocalLenIn35mmFilm"),
                      abs(ff - f) >= 0.5 else { return mm }
                return String(localized: "\(mm) (\(decimal(ff, 0)) mm in 35 mm)")
            })
        if let b = num("ExposureBiasValue"), b != 0 {
            add("bias", String(localized: "Exposure bias"),
                (b > 0 ? "+" : "−") + decimal(abs(b), 1) + " EV")
        }
        add("program", String(localized: "Program"),
            num("ExposureProgram").flatMap { program(Int($0)) })
        add("metering", String(localized: "Metering"),
            num("MeteringMode").flatMap { metering(Int($0)) })
        add("flash", String(localized: "Flash"),
            num("Flash").map { Int($0) & 1 == 1
                ? String(localized: "Fired")
                : String(localized: "Did not fire") })
        add("wb", String(localized: "White balance"),
            num("WhiteBalance").map { $0 == 1
                ? String(localized: "Manual")
                : String(localized: "Auto") })
        if let lat = num("GPSLatitude"), let lon = num("GPSLongitude") {
            add("location", String(localized: "Location"),
                coordinate(lat, "N", "S") + ", " + coordinate(lon, "E", "W"))
        }
        add("altitude", String(localized: "Altitude"),
            num("GPSAltitude").map { decimal($0, 0) + " m" })
        add("description", String(localized: "Description"),
            text("ImageDescription"))
        add("artist", String(localized: "Artist"), text("Artist"))
        add("copyright", String(localized: "Copyright"), text("Copyright"))
        add("software", String(localized: "Software"), text("Software"))
        add("modified", String(localized: "Modified"),
            text("DateTime").map(date))
        return out
    }

    /// "Canon" + "Canon EOS R5" reads "Canon EOS R5", not twice the make.
    private static func joined(_ make: String?, _ model: String?) -> String? {
        guard let model else { return make }
        guard let make, !model.lowercased().hasPrefix(make.lowercased())
        else { return model }
        return make + " " + model
    }

    /// EXIF's "2024:05:01 18:30:00", as the user's dates read.
    private static func date(_ s: String) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy:MM:dd HH:mm:ss"
        guard let d = f.date(from: s) else { return s }
        return d.formatted(date: .abbreviated, time: .shortened)
    }

    private static func exposure(_ t: Double) -> String {
        guard t > 0 else { return "" }
        if t < 1 { return "1/" + decimal(1 / t, 0) + " s" }
        return decimal(t, 1) + " s"
    }

    /// A number as text, no digit grouping, at most `places` decimals.
    private static func decimal(_ v: Double, _ places: Int) -> String {
        v.formatted(.number.grouping(.never)
            .precision(.fractionLength(0...places)))
    }

    private static func coordinate(_ v: Double, _ pos: String,
                                   _ neg: String) -> String {
        decimal(abs(v), 4) + "° " + (v < 0 ? neg : pos)
    }

    private static func program(_ p: Int) -> String? {
        switch p {
        case 1: String(localized: "Manual")
        case 2: String(localized: "Program AE")
        case 3: String(localized: "Aperture priority")
        case 4: String(localized: "Shutter priority")
        case 5: String(localized: "Creative")
        case 6: String(localized: "Action")
        case 7: String(localized: "Portrait")
        case 8: String(localized: "Landscape")
        default: nil
        }
    }

    private static func metering(_ m: Int) -> String? {
        switch m {
        case 1: String(localized: "Average")
        case 2: String(localized: "Center-weighted")
        case 3: String(localized: "Spot")
        case 4: String(localized: "Multi-spot")
        case 5: String(localized: "Pattern")
        case 6: String(localized: "Partial")
        default: nil
        }
    }
}

/// The stacked sections' column: each header at its own height, the
/// open sections (a `PanelWeight` each) sharing what is left by weight.
private struct StackedPanels: Layout {
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews,
                      cache: inout ()) -> CGSize {
        let w = proposal.width ?? 260
        let fixed = subviews.reduce(CGFloat(0)) { sum, v in
            v[PanelWeight.self] == nil
                ? sum + v.sizeThatFits(.init(width: w, height: nil)).height
                : sum
        }
        return CGSize(width: w, height: max(proposal.height ?? fixed, fixed))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize,
                       subviews: Subviews, cache: inout ()) {
        let w = bounds.width
        var fixed: CGFloat = 0
        var total = 0.0
        var own: [CGFloat?] = []
        for v in subviews {
            if let weight = v[PanelWeight.self] {
                total += weight
                own.append(nil)
            } else {
                let h = v.sizeThatFits(.init(width: w, height: nil)).height
                fixed += h
                own.append(h)
            }
        }
        let rest = max(0, bounds.height - fixed)
        var y = bounds.minY
        for (i, v) in subviews.enumerated() {
            let h = own[i] ?? (total > 0
                ? rest * CGFloat((v[PanelWeight.self] ?? 0) / total) : 0)
            v.place(at: CGPoint(x: bounds.minX, y: y), anchor: .topLeading,
                    proposal: .init(width: w, height: h))
            y += h
        }
    }
}

/// An open stacked section's share of the height; none: a header, at its
/// own.
private struct PanelWeight: LayoutValueKey {
    static let defaultValue: Double? = nil
}

/// The divider between two open stacked sections, the top edge of the
/// lower one's header: dragged, the one above takes height from the one
/// below (or gives it), neither under `least`; double-clicked, every
/// section's share is even again.
private struct PanelDivider: View {
    let model: AppModel
    let above: InspectorTab
    let below: InspectorTab
    /// At the drag's start: the two heights, and their weights.
    @State private var start: (ha: CGFloat, hb: CGFloat, wa: Double,
                               wb: Double)?
    private let least: CGFloat = 60

    var body: some View {
        // Inside the header's top edge: what lies over the section above
        // takes its own clicks.
        DividerHandle(changed: drag, ended: { start = nil },
                      reset: { model.inspectorWeights = [:] })
            .frame(height: 6)
            .frame(maxWidth: .infinity)
            .help("Drag to share the height between the sections; double-click to share it evenly")
    }

    private func drag(_ dy: CGFloat) {
        if start == nil {
            guard let ha = model.inspectorHeights[above],
                  let hb = model.inspectorHeights[below],
                  ha + hb > 0 else { return }
            start = (ha, hb, model.inspectorWeight(above),
                     model.inspectorWeight(below))
        }
        guard let s = start else { return }
        let sum = s.ha + s.hb
        let ha = min(max(s.ha + dy, min(least, sum / 2)),
                     sum - min(least, sum / 2))
        let wa = (s.wa + s.wb) * Double(ha / sum)
        var weights = model.inspectorWeights
        weights[above] = wa
        weights[below] = s.wa + s.wb - wa
        model.inspectorWeights = weights
    }
}

/// A divider's grip: an AppKit view, so the drag is the mouse's own --
/// down, dragged, up -- with the resize cursor over it. `changed` gets how
/// far it has moved down since the press; a double-click resets.
private struct DividerHandle: NSViewRepresentable {
    var changed: (CGFloat) -> Void
    var ended: () -> Void
    var reset: () -> Void

    func makeNSView(context: Context) -> Grip {
        let v = Grip()
        v.handle = self
        return v
    }

    func updateNSView(_ v: Grip, context: Context) { v.handle = self }

    final class Grip: NSView {
        var handle: DividerHandle?
        private var pressed: CGFloat?

        override func resetCursorRects() {
            addCursorRect(bounds, cursor: .resizeUpDown)
        }

        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

        override func mouseDown(with event: NSEvent) {
            if event.clickCount == 2 {
                pressed = nil
                handle?.reset()
                return
            }
            pressed = event.locationInWindow.y
        }

        override func mouseDragged(with event: NSEvent) {
            guard let y = pressed else { return }
            // Window coordinates grow upward; the drag counts down.
            handle?.changed(y - event.locationInWindow.y)
        }

        override func mouseUp(with event: NSEvent) {
            guard pressed != nil else { return }
            pressed = nil
            handle?.ended()
        }
    }
}

/// A stacked section's header: its fold, its icon, its name -- a click
/// folds it to this row, or opens it.
private struct PanelHeader: View {
    let tab: InspectorTab
    let open: Bool
    let toggle: () -> Void
    @State private var hovering = false

    var body: some View {
        Button(action: toggle) {
            HStack(spacing: 6) {
                Image(systemName: "chevron.right")
                    .font(.system(size: 9, weight: .semibold))
                    .rotationEffect(.degrees(open ? 90 : 0))
                    .foregroundStyle(.secondary)
                    .frame(width: 10)
                Image(systemName: tab.symbol)
                    .foregroundStyle(.secondary)
                Text(tab.label)
                    .font(.system(size: 11, weight: .semibold))
                Spacer(minLength: 0)
            }
            .padding(.horizontal, 12)
            .frame(height: 26)
            .frame(maxWidth: .infinity)
            .background(Color.primary.opacity(hovering ? 0.07 : 0.04))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .onHover { hovering = $0 }
        .overlay(alignment: .bottom) { Divider() }
        .help(open ? "Fold this section to its header" : "Open this section")
        .accessibilityValue(Text(open ? "Open" : "Folded"))
    }
}

/// The picture on the stage's LAYER STACK (core project::Layer), top
/// first: a row each, with its eye (shown or hidden), its picture, and
/// its name (double-click to rename). The selected layer is what the
/// Adjust and Crop panels change and where the next generation's picture
/// goes. A layer dragged into the prompt carries that layer alone, as it
/// shows; the stage's picture -- every layer shown -- carries the whole.
/// ⌘-click selects a second layer: the two can be MERGED into one. A
/// layer can be made the MASK of the one beneath it (its brightness
/// times its alpha): the mask's row is set in, with an arrow down to the
/// layer it masks, which carries a mask badge; released, it is a layer
/// again.
struct LayersSection: View {
    @Bindable var model: AppModel
    /// Its notes at the foot (the stacked inspector leaves them out).
    var notes = true

    var body: some View {
        if let pic = model.stageStack {
            let stack = pic.layerStack
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 2) {
                    tool("plus", "Add a layer above the selected one") {
                        model.addLayer()
                    }
                    tool("arrow.up", "Move the selected layer up",
                         disabled: !canMove(stack, up: true)) {
                        model.moveLayer(model.activeLayer, by: 1)
                    }
                    tool("arrow.down", "Move the selected layer down",
                         disabled: !canMove(stack, up: false)) {
                        model.moveLayer(model.activeLayer, by: -1)
                    }
                    tool("square.stack.3d.down.right",
                         model.mergeProblem.map { LocalizedStringKey($0) }
                            ?? "Merge the two selected layers into one",
                         disabled: model.mergeProblem != nil) {
                        model.mergeSelectedLayers()
                    }
                    let active = stack.first { $0.id == model.activeLayer }
                    let isMask = active?.mask == true
                    tool(isMask ? "theatermasks.fill" : "theatermasks",
                         isMask ? "Release the mask: a layer again"
                                : "Use as the mask of the layer below: it shows where this layer is bright",
                         disabled: model.isBottomLayer(model.activeLayer)) {
                        model.setLayerMask(model.activeLayer, !isMask)
                    }
                    Spacer()
                    Menu {
                        Button("Capture All Layers") {
                            model.captureStage(selectedOnly: false)
                        }
                        Button("Capture Selected Layers") {
                            model.captureStage(selectedOnly: true)
                        }
                        .disabled(model.selectedLayers.isEmpty)
                    } label: {
                        Image(systemName: "camera.viewfinder")
                            .frame(width: 22, height: 20)
                    }
                    .menuStyle(.borderlessButton)
                    .menuIndicator(.hidden)
                    .fixedSize()
                    .disabled(model.isGenerating)
                    .help("Capture the layers as an asset: their looks and what they show, frozen. Dragged back onto the stage, they are editable again.")
                    tool("minus", "Remove the selected layer",
                         disabled: !model.canRemoveLayer(model.activeLayer)) {
                        model.removeLayer(model.activeLayer)
                    }
                }
                .padding(.horizontal, 12)
                // A still's pages: the one shown, to the others, one more.
                if model.stagePicture != nil {
                    PageBar(model: model)
                        .padding(.horizontal, 12)
                }
                ScrollView {
                    VStack(spacing: 2) {
                        ForEach(Array(stack.enumerated()).reversed(),
                                id: \.element.id) { i, l in
                            LayerRow(model: model, layer: l,
                                     masked: i + 1 < stack.count
                                        && stack[i + 1].mask)
                        }
                    }
                    .padding(.horizontal, 8)
                }
                if notes {
                Group {
                    if pic.kind == "video" {
                        Text("Drop an asset on the stage to put it on a new layer at the playhead, or on a layer's row to show it there. Trim sets where the selected layer starts and what of it shows; its adjustments, crop and turn are keyed from its start. A layer showing a composition decomposes from its menu.")
                    } else if pic.isPaged {
                        Text("Each page shows the layers on it; a new layer, or an asset dropped on the stage, goes on the page shown — a layer's menu sets its pages. Adjustments, crop and turn are keyed by page, as a clip's by frame. Export writes a file a page.")
                    } else {
                        Text("New pictures and markup are made on the selected layer; an asset dropped on the stage goes on a new layer. ⌘-click a second layer to merge the two. A layer showing a composition decomposes from its menu. Drag a layer into the prompt to edit it alone; drag the picture on the stage, with every layer shown, to edit the whole.")
                    }
                }
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                    .padding(.horizontal, 14)
                    .padding(.bottom, 12)
                }
            }
        } else {
            ContentUnavailableView(
                "No layers", systemImage: "square.3.layers.3d",
                description: Text("A picture or a clip on the stage has layers."))
                .controlSize(.small)
        }
    }

    /// Above the bottom layer, with room to go.
    /// Up or down the stack. A picture's own bottom layer (its frame) and
    /// a clip's own stay at the bottom; in the project's pictures every
    /// layer moves, layer 0 too.
    private func canMove(_ stack: [LayerDTO], up: Bool) -> Bool {
        guard let i = stack.firstIndex(where: { $0.id == model.activeLayer })
        else { return false }
        let fixed = !(model.stageFramed && model.stageStack?.kind == "image")
        if fixed && i == 0 { return false }
        return up ? i < stack.count - 1 : i > (fixed ? 1 : 0)
    }

    private func tool(_ symbol: String, _ help: LocalizedStringKey,
                      disabled: Bool = false,
                      action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .frame(width: 22, height: 20)
                .contentShape(Rectangle())
        }
        .buttonStyle(.borderless)
        .disabled(disabled || model.isGenerating)
        .help(help)
    }
}

/// A still's PAGES (DESIGN §6a): the page the stage shows -- the arrows
/// go to the one before and after -- Add Page (after it: what runs across
/// it continues there) and Remove Page (with the layers only it had). A
/// still of one page has Add Page alone.
private struct PageBar: View {
    @Bindable var model: AppModel

    var body: some View {
        let n = model.stagePages
        let page = model.stagePage
        HStack(spacing: 2) {
            if n > 1 {
                Button {
                    model.goToPage(page - 1)
                } label: {
                    Image(systemName: "chevron.backward")
                        .frame(width: 20, height: 20)
                        .contentShape(Rectangle())
                }
                .disabled(page == 0)
                .help("Previous page")
                Text("Page \(String(page + 1)) of \(String(n))")
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(minWidth: 84)
                Button {
                    model.goToPage(page + 1)
                } label: {
                    Image(systemName: "chevron.forward")
                        .frame(width: 20, height: 20)
                        .contentShape(Rectangle())
                }
                .disabled(page + 1 >= n)
                .help("Next page")
            } else {
                Text("One page")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            Button {
                model.addPage()
            } label: {
                Label("Add Page", systemImage: "plus.rectangle.on.rectangle")
                    .labelStyle(.iconOnly)
                    .frame(width: 22, height: 20)
                    .contentShape(Rectangle())
            }
            .help(n > 1 ? "Add a page after this one: layers running across it continue there"
                        : "Add a page: the picture becomes a page, and layers on it continue on the next")
            if n > 1 {
                Button {
                    model.removePage()
                } label: {
                    Label("Remove Page", systemImage: "minus.rectangle")
                        .labelStyle(.iconOnly)
                        .frame(width: 22, height: 20)
                        .contentShape(Rectangle())
                }
                .help("Remove this page, and the layers only it has")
            }
        }
        .buttonStyle(.borderless)
        .disabled(model.isGenerating)
    }
}

/// One layer: eye, picture, name. A click selects it; a double-click on
/// the name renames it; a drag carries it, alone, as it shows.
/// A layer's look at a glance: an adjustment, a placement (crop, scale,
/// offset) and a turn, each a small mark while it changes the layer -- in
/// the accent when it is keyed (changes over the clip).
private struct LookMarks: View {
    let look: AppModel.LayerLook

    var body: some View {
        HStack(spacing: 3) {
            if look.adjust {
                mark("slider.horizontal.3", "Adjusted")
            }
            if look.place {
                mark("crop", "Cropped or moved")
            }
            if look.turn {
                mark("rotate.right", "Rotated")
            }
            if look.keyed {
                mark("diamond.fill", "Keyed: changes over the clip")
                    .foregroundStyle(Color.accentColor)
            }
        }
        .font(.system(size: 9))
        .foregroundStyle(.secondary)
    }

    private func mark(_ symbol: String, _ help: LocalizedStringKey)
        -> some View {
        Image(systemName: symbol)
            .help(help)
            .accessibilityLabel(Text(help))
    }
}

private struct LayerRow: View {
    @Bindable var model: AppModel
    let layer: LayerDTO
    /// The layer above it is its mask.
    var masked = false
    @State private var image: CGImage?
    @State private var renaming = false
    @State private var name = ""
    @State private var dropTarget = false
    @FocusState private var nameFocused: Bool

    /// What it is beside its name: a mask, or what it shows when that is
    /// not a plain picture -- a markup, a composition, a sound.
    private var shows: LocalizedStringKey? {
        if layer.mask { return "Mask" }
        if layer.isMarkup { return "Markup" }
        guard let src = model.source(of: layer) else { return nil }
        if src.kind == "audio" { return "Sound" }
        if src.isTimeline { return "Composition" }
        if src.isComposition { return "Still" }
        if src.kind == "video" { return "Clip" }
        return nil
    }

    var body: some View {
        let selected = model.activeLayer == layer.id
        let inSelection = model.selectedLayers.contains(layer.id)
        // A still's page shows some layers: the others are set back.
        let here = model.isLayerOnPage(layer)
        HStack(spacing: 8) {
            if layer.mask {
                // Set in, pointing down to the layer it masks.
                Image(systemName: "arrow.turn.left.down")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(width: 10)
                    .help("Masks the layer below")
            }
            Button {
                model.setLayerVisible(layer.id, !layer.visible)
            } label: {
                Image(systemName: layer.visible ? "eye" : "eye.slash")
                    .foregroundStyle(layer.visible ? .primary : .secondary)
                    .frame(width: 18)
            }
            .buttonStyle(.borderless)
            .help(layer.visible ? "Hide this layer" : "Show this layer")
            ZStack {
                RoundedRectangle(cornerRadius: 3)
                    .fill(Color.primary.opacity(0.06))
                if let image {
                    Image(decorative: image, scale: 1)
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                } else if layer.isEmpty {
                    Image(systemName: "square.dashed")
                        .foregroundStyle(.tertiary)
                }
            }
            .frame(width: 34, height: 34)
            .clipShape(RoundedRectangle(cornerRadius: 3))
            .overlay(alignment: .bottomTrailing) {
                if masked {
                    Image(systemName: "theatermasks.fill")
                        .font(.system(size: 9))
                        .padding(2)
                        .background(.background, in: Circle())
                        .offset(x: 4, y: 4)
                        .help("Masked by the layer above")
                }
            }
            if renaming {
                TextField("Name", text: $name)
                    .textFieldStyle(.roundedBorder)
                    .focused($nameFocused)
                    .onSubmit(commitName)
                    .onChange(of: nameFocused) { _, f in
                        if !f { commitName() }
                    }
            } else {
                VStack(alignment: .leading, spacing: 1) {
                    Text(verbatim: layer.title)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .foregroundStyle(layer.visible ? .primary
                                                       : .secondary)
                        .onTapGesture(count: 2) {
                            name = layer.name.isEmpty ? layer.title
                                                      : layer.name
                            renaming = true
                            nameFocused = true
                        }
                    if let kind = shows {
                        Text(kind)
                            .font(.system(size: 9))
                            .foregroundStyle(.secondary)
                    }
                    if let pages = model.pageSpanText(layer) {
                        Text(verbatim: pages)
                            .font(.system(size: 9))
                            .foregroundStyle(.secondary)
                    }
                }
            }
            Spacer(minLength: 0)
            LookMarks(look: model.layerLook(layer.id))
        }
        .opacity(here ? 1 : 0.45)
        .padding(.horizontal, 6)
        .padding(.vertical, 4)
        .padding(.leading, layer.mask ? 10 : 0)
        .background(RoundedRectangle(cornerRadius: 6)
            .fill(selected ? Color.accentColor.opacity(0.18)
                  : inSelection ? Color.accentColor.opacity(0.09)
                  : .clear))
        .contentShape(Rectangle())
        // ⌘- or ⇧-click: in or out of the selection, to merge two.
        .onTapGesture {
            let mods = NSEvent.modifierFlags
            if mods.contains(.command) || mods.contains(.shift) {
                model.toggleLayerSelection(layer.id)
            } else {
                model.selectLayer(layer.id)
            }
        }
        .contextMenu {
            if !model.isBottomLayer(layer.id) {
                Button(layer.mask ? "Release Mask"
                                  : "Use as Mask of Layer Below") {
                    model.setLayerMask(layer.id, !layer.mask)
                }
            }
            // A layer showing a composition: its layers in its place.
            if model.showsComposition(layer) {
                Button("Decompose") { model.decomposeLayer(layer.id) }
                    .help("Replace this layer by the composition's own layers, placed where it shows them")
            }
            if model.selectedLayers.count == 2,
               model.selectedLayers.contains(layer.id) {
                Button("Merge Selected Layers") {
                    model.mergeSelectedLayers()
                }
                .disabled(model.mergeProblem != nil)
            }
            // A still's pages: which of them it is on.
            if model.pagedOnStage {
                Divider()
                let page = String(model.stagePage + 1)
                Button("Only on Page \(page)") {
                    model.setLayerPages(layer.id, .thisPage)
                }
                Button("From Page \(page) On") {
                    model.setLayerPages(layer.id, .fromHere)
                }
                Button("On Every Page") {
                    model.setLayerPages(layer.id, .every)
                }
            }
        }
        // A picture or a clip dropped on it -- a file, or an asset of the
        // list (which arrives as a URL too): what it shows.
        .dropDestination(for: URL.self) { urls, _ in
            guard model.isPlacedLayer(layer.id), let url = urls.first else {
                return false
            }
            if let id = AppModel.draggedAssets([url]).first {
                model.setLayerSource(layer.id, asset: id)
                return true
            }
            guard url.isFileURL else { return false }
            model.setLayerSource(layer.id, from: url)
            return true
        } isTargeted: { dropTarget = $0 }
        .overlay {
            if dropTarget {
                RoundedRectangle(cornerRadius: 6)
                    .strokeBorder(Color.accentColor, lineWidth: 2)
            }
        }
        .onDrag {
            guard let url = model.layerFile(layer.id) else {
                return NSItemProvider()
            }
            return NSItemProvider(object: url as NSURL)
        }
        .task(id: "\(layer.id)-\(layer.source ?? "")-\(layer.own)-\(layer.markup?.raster ?? "")-\(layer.markup?.objects.hashValue ?? 0)") {
            image = await thumbnail()
        }
    }

    private func commitName() {
        guard renaming else { return }
        renaming = false
        let n = name.trimmingCharacters(in: .whitespaces)
        if n != layer.title { model.renameLayer(layer.id, n) }
    }

    /// Its picture's thumbnail: the picture's own, the one it shows, or
    /// -- markup -- the layer alone, drawn by the core.
    private func thumbnail() async -> CGImage? {
        guard let core = model.core, let project = model.projectId,
              let pic = model.stageStack?.id else { return nil }
        if layer.isMarkup {
            let out = AppModel.shareRoot.appendingPathComponent(
                "layer-\(pic)-\(layer.id).png")
            let id = layer.id
            return await Task.detached(priority: .utility) {
                try? FileManager.default.createDirectory(
                    at: AppModel.shareRoot,
                    withIntermediateDirectories: true)
                guard core.flatten(project: project, asset: pic,
                                   to: out.path, only: id).ok else {
                    return nil
                }
                return AppModel.thumbnailImage(out, maxPixels: 96)
            }.value
        }
        guard let id = layer.own ? pic : layer.source else { return nil }
        return await Task.detached(priority: .utility) {
            // What the layer shows: the file itself, not its look.
            core.thumbnail(project: project, asset: id, maxPixels: 96,
                           plain: true)
                .flatMap { AppModel.loadImage($0) }
        }.value
    }
}

/// The project set up (DESIGN §6a): a STILL (a picture project), a
/// TIMELINE (clips) or SOUND alone, at a size -- the project's
/// composition from now on.
struct ProjectSetupForm: View {
    @Bindable var model: AppModel
    var done: (() -> Void)?
    @State private var kind = 0       // 0 still, 1 timeline, 2 sound
    @State private var width = 1024
    @State private var height = 1024
    /// What exporting the project writes: Rec. 709, 24 fps, stereo at
    /// 48 kHz unless chosen otherwise.
    @State private var output = OutputSettingsDTO()

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Picker("Type", selection: $kind) {
                Text("Still").tag(0)
                Text("Timeline").tag(1)
                Text("Sound").tag(2)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .onChange(of: kind) { _, k in
                // A clip's usual frame, a picture's.
                if k == 1, width == 1024, height == 1024 {
                    (width, height) = (832, 480)
                } else if k == 0, width == 832, height == 480 {
                    (width, height) = (1024, 1024)
                }
            }
            if kind != 2 {
                HStack(spacing: 4) {
                    TextField("Width", value: $width, format: .number.grouping(.never))
                        .frame(width: 64)
                    Text(verbatim: "×").foregroundStyle(.secondary)
                    TextField("Height", value: $height, format: .number.grouping(.never))
                        .frame(width: 64)
                    Text("pixels").foregroundStyle(.secondary)
                }
                .textFieldStyle(.roundedBorder)
                .monospacedDigit()
            } else {
                Text("No picture: sounds alone, mixed.")
                    .font(.system(size: 11))
                    .foregroundStyle(.secondary)
            }
            OutputFields(kind: ProjectKind(rawValue: kind) ?? .still,
                         output: $output)
            Button("Set Up Project") {
                model.setUpProject(ProjectKind(rawValue: kind) ?? .still,
                                   width: max(1, width),
                                   height: max(1, height), output: output)
                done?()
            }
            .keyboardShortcut(.defaultAction)
            .disabled(kind != 2 && (width < 1 || height < 1
                                    || width > 32768 || height > 32768))
        }
        .controlSize(.small)
    }
}

/// New Project…: a new project composition, set up in a popover; the one
/// there now stays among the assets.
struct NewProjectButton: View {
    @Bindable var model: AppModel
    @State private var open = false

    var body: some View {
        Button("New Project…") { open = true }
            .help("Set up a new project composition; this one stays an asset")
            .popover(isPresented: $open, arrowEdge: .bottom) {
                VStack(alignment: .leading, spacing: 8) {
                    Text("New Project")
                        .font(.headline)
                    ProjectSetupForm(model: model) { open = false }
                }
                .padding(14)
                .frame(width: 260)
            }
    }
}
