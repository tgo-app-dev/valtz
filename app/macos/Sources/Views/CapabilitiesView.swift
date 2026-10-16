import AppKit
import SwiftUI

/// Settings › Capabilities: the model families as a tree (core
/// Controller::capability_tree). A family shows what it makes -- video,
/// video edits, pictures, picture edits, help with prompts, upscaling --
/// each icon checked where it works here now; under it, every resource it
/// runs with: its models, previews, LoRAs, branches and VAEs. Each can be
/// downloaded into the models folder Valtz manages, or LINKED where it
/// already is -- vpipe's models, ComfyUI's, Draw Things', anywhere.
struct CapabilitiesView: View {
    @Bindable var model: AppModel
    @State private var expanded: Set<String> = []

    var body: some View {
        ScrollViewReader { scroller in
        List {
            SettingsHeader(page: .capabilities)
                .background(Color.primary.opacity(0.04),
                            in: RoundedRectangle(cornerRadius: 12,
                                                 style: .continuous))
                .listRowSeparator(.hidden)
                .padding(.bottom, 6)
            HStack {
                if let hw = model.hardware {
                    Text("\(hw.chip) · \(hw.ramGb) GB · \(model.engine)")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button("Rescan") { model.rescanModels() }
                    .controlSize(.small)
                    .help("Look for models again")
            }
            ForEach(model.capabilityTree?.families ?? []) { f in
                DisclosureGroup(isExpanded: Binding(
                    get: { expanded.contains(f.id) },
                    set: { if $0 { expanded.insert(f.id) }
                           else { expanded.remove(f.id) } })) {
                    ForEach(f.members) { m in
                        MemberRow(model: model, member: m)
                    }
                } label: {
                    // The whole row opens and closes the family, not only
                    // the wedge: a List's DisclosureGroup turns only on its
                    // wedge on macOS, and its row takes a tap gesture
                    // itself -- a button still gets the click.
                    Button { toggle(f.id) } label: {
                        FamilyRow(family: f)
                            .padding(.leading, 6)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                }
                .id(f.id)
            }
        }
        .listStyle(.inset)
        .onAppear {
            model.refreshMachine()
            // The first family open, the rest closed.
            if expanded.isEmpty, let f = model.capabilityTree?.families.first {
                expanded = [f.id]
            }
            openAsked(scroller)
        }
        // A family asked for (a preset's LoRA to download): opened.
        .onChange(of: model.capabilitiesFamily) { _, _ in
            openAsked(scroller)
        }
        // A gated model's Download: its license and the access token.
        .sheet(item: $model.gatedDownload) { m in
            GatedDownloadSheet(model: model, member: m)
        }
        // A compress asked for: what it replaces, what it costs.
        .alert(compressTitle, isPresented: Binding(
            get: { model.compressAsk != nil },
            set: { if !$0 { model.compressAsk = nil } })) {
            Button("Compress") {
                if let ask = model.compressAsk {
                    model.compress(model: ask.member.model, bits: ask.bits)
                }
                model.compressAsk = nil
            }
            Button("Cancel", role: .cancel) { model.compressAsk = nil }
        } message: {
            Text(verbatim: compressMessage)
        }
        }
    }

    private var compressTitle: String {
        guard let ask = model.compressAsk else { return "" }
        return String(localized: "Compress \(ask.member.name) to \(String(ask.bits))-bit?")
    }

    /// What the copy replaces and costs: the download deleted once the
    /// copy is checked; pictures a little different, 4 bits more so; the
    /// original a download away.
    private var compressMessage: String {
        guard let ask = model.compressAsk,
              let w = ask.member.compress?.widths.first(where: {
                  $0.bits == ask.bits })
        else { return "" }
        let style = ByteCountFormatStyle(style: .file)
        let now = ask.member.bytes.formatted(style)
        let copy = w.bytes.formatted(style)
        let quality = ask.bits == 4
            ? String(localized: "At 4 bits its results lose some detail and follow the prompt a little less closely.")
            : String(localized: "At 8 bits its results are very close to the original's.")
        return String(localized: "Its \(now) download is replaced by a copy of about \(copy), made here and checked before the download is deleted. \(quality) To have the original back, download it again.")
    }

    /// The family asked for opened, and scrolled to the top: one far down
    /// the list (Listening) is otherwise opened out of sight.
    private func openAsked(_ scroller: ScrollViewProxy) {
        guard let f = model.capabilitiesFamily else { return }
        withAnimation(AppModel.motion) { _ = expanded.insert(f) }
        model.capabilitiesFamily = nil
        DispatchQueue.main.async {
            withAnimation(AppModel.motion) {
                scroller.scrollTo(f, anchor: .top)
            }
        }
    }

    private func toggle(_ id: String) {
        withAnimation(AppModel.motion) {
            if expanded.contains(id) { expanded.remove(id) }
            else { expanded.insert(id) }
        }
    }
}

/// A family: its name and what it makes, each feature an icon -- checked
/// where it works here now.
private struct FamilyRow: View {
    let family: CapabilityTree.Family

    var body: some View {
        HStack(spacing: 10) {
            Text(verbatim: family.name).font(.body.weight(.semibold))
            Spacer()
            HStack(spacing: 6) {
                ForEach(family.features) { FeatureBadge(feature: $0) }
            }
        }
        .padding(.vertical, 2)
    }
}

private struct FeatureBadge: View {
    let feature: CapabilityTree.Feature

    var body: some View {
        Image(systemName: Self.symbol(feature.feature))
            .font(.system(size: 13))
            .foregroundStyle(feature.available ? Color.accentColor
                                               : Color.secondary.opacity(0.6))
            .frame(width: 26, height: 22)
            .background(RoundedRectangle(cornerRadius: 5)
                .fill(feature.available ? Color.accentColor.opacity(0.12)
                                        : Color.primary.opacity(0.04)))
            .overlay(alignment: .bottomTrailing) {
                if feature.available {
                    Image(systemName: "checkmark.circle.fill")
                        .font(.system(size: 9))
                        .foregroundStyle(.white, .green)
                        .offset(x: 3, y: 3)
                }
            }
            .help(Self.name(feature.feature) + " — " + Self.why(feature.why))
            .accessibilityLabel(Text(verbatim: Self.name(feature.feature)))
            .accessibilityValue(Text(verbatim: Self.why(feature.why)))
    }

    static func symbol(_ f: String) -> String {
        switch f {
        case "video-gen": "film"
        case "video-edit": "film.stack"
        case "image-gen": "photo"
        case "image-edit": "wand.and.stars"
        case "audio-gen": "music.note"
        case "speech-gen": "waveform.and.person.filled"
        case "helper": "brain"
        case "video-upscale": "arrow.up.left.and.arrow.down.right"
        case "image-upscale": "plus.magnifyingglass"
        case "audio-transcribe": "captions.bubble"
        case "video-summary": "text.below.photo"
        case "audio-separate": "music.mic"
        default: "questionmark"
        }
    }

    static func name(_ f: String) -> String {
        switch f {
        case "video-gen": String(localized: "Video generation")
        case "video-edit": String(localized: "Video editing")
        case "image-gen": String(localized: "Image generation")
        case "image-edit": String(localized: "Image editing")
        case "audio-gen": String(localized: "Music generation")
        case "speech-gen": String(localized: "Speech generation, voice cloning")
        case "helper": String(localized: "Agentic helper")
        case "video-upscale": String(localized: "Video upscaling")
        case "image-upscale": String(localized: "Image upscaling")
        case "audio-transcribe": String(localized: "Transcription, sound events")
        case "video-summary": String(localized: "Video summary")
        case "audio-separate": String(localized: "Vocals and accompaniment")
        default: f
        }
    }

    static func why(_ w: String) -> String {
        switch w {
        case "ready": String(localized: "ready")
        case "download": String(localized: "download a model to enable")
        case "memory": String(localized: "needs more memory")
        default: String(localized: "engine support in progress")
        }
    }
}

/// One resource: whether it is here, what it is, how big, where it is
/// kept -- and what can be done: download it, link it where it already
/// is, undo a link, show it in the Finder, open its page.
private struct MemberRow: View {
    @Bindable var model: AppModel
    let member: CapabilityTree.Member

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            row
            // Why its last download failed (a gated one refused, ...).
            if let why = model.downloadFailures[member.model],
               member.state != "installed", fetching == nil {
                Label {
                    Text(verbatim: why)
                } icon: {
                    Image(systemName: "exclamationmark.triangle.fill")
                }
                .font(.caption)
                .foregroundStyle(.orange)
                .padding(.leading, 26)
                .textSelection(.enabled)
            }
            // Its files as last checked (a link made or refused), and its
            // last compress: what was found, what it freed.
            if let check = model.modelChecks[member.model] {
                NotesView(notes: check.notes) {
                    model.modelChecks[member.model] = nil
                }
            }
            if let note = model.compressNotes[member.model], fetching == nil {
                NotesView(notes: [note]) {
                    model.compressNotes[member.model] = nil
                }
            }
        }
    }

    private var row: some View {
        let m = member
        return HStack(spacing: 10) {
            stateIcon
            VStack(alignment: .leading, spacing: 1) {
                Text(verbatim: m.label)
                Text(verbatim: m.name)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            .help(m.notes.isEmpty ? m.hfPath : m.notes)
            roleTag
            Spacer(minLength: 8)
            Text(verbatim: size)
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
            if !m.source.isEmpty {
                Text(verbatim: where_)
                    .font(.caption)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1)
                    .background(.quaternary, in: Capsule())
                    .help(m.path)
            }
            // A compressed copy in its download's place.
            if let c = m.compressed {
                Text(verbatim: String(localized: "\(String(c.bits))-bit"))
                    .font(.caption.weight(.medium))
                    .foregroundStyle(Color.accentColor)
                    .padding(.horizontal, 6)
                    .padding(.vertical, 1)
                    .background(Color.accentColor.opacity(0.12),
                                in: Capsule())
                    .help(String(localized: "Compressed to \(String(c.bits)) bits in place of its \(c.originalBytes.formatted(ByteCountFormatStyle(style: .file))) download"))
            }
            actions
        }
        .padding(.vertical, 2)
    }

    @ViewBuilder
    private var stateIcon: some View {
        switch member.state {
        case "installed":
            Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
                .help("Installed")
        case "partial":
            Image(systemName: "exclamationmark.circle")
                .foregroundStyle(.orange)
                .help("A download was interrupted")
        default:
            Image(systemName: "circle.dashed").foregroundStyle(.secondary)
                .help("Not installed")
        }
    }

    private var roleTag: some View {
        let t: String = switch member.role {
        case "assistant": String(localized: "Agentic helper")
        case "preview": String(localized: "Preview")
        case "lora": String(localized: "LoRA")
        case "branch": String(localized: "Branch")
        case "vae": String(localized: "VAE")
        case "encoder": String(localized: "Encoder")
        case "prompt": String(localized: "Prompt guide")
        default: String(localized: "Model")
        }
        return Text(verbatim: t)
            .font(.caption2.weight(.medium))
            .foregroundStyle(.secondary)
            .padding(.horizontal, 5)
            .padding(.vertical, 1)
            .overlay(Capsule().strokeBorder(.quaternary))
    }

    /// Its size: as it is on disk, or about what a download would take.
    private var size: String {
        let style = ByteCountFormatStyle(style: .file)
        if member.state == "installed" && member.bytes > 0 {
            return member.bytes.formatted(style)
        }
        let est = Int64(member.diskGb * 1e9).formatted(style)
        return String(localized: "about \(est)")
    }

    private var where_: String {
        switch member.source {
        case "valtz": String(localized: "Valtz")
        case "vpipe": String(localized: "vpipe")
        case "link": String(localized: "Linked")
        default: ""
        }
    }

    /// A download of it -- or, a quantized variant, its quantizing; a
    /// download, its compressing -- still running.
    private var fetching: JobInfo? {
        model.jobs.values.first {
            ($0.purpose == "download" || $0.purpose == "quantize"
             || $0.purpose == "compress")
                && $0.model == member.model
                && ($0.state == "queued" || $0.state == "running")
        }
    }

    @ViewBuilder
    private var actions: some View {
        let m = member
        HStack(spacing: 6) {
            if let job = fetching {
                ProgressView(value: job.phase?.fraction)
                    .controlSize(.small)
                    .frame(width: 70)
                Text(verbatim: job.phase?.percent
                     ?? (job.purpose == "quantize"
                         ? String(localized: "Quantizing…")
                         : job.purpose == "compress"
                         ? String(localized: "Compressing…") : ""))
                    .font(.caption.monospacedDigit())
                    // Its own width: the row squeezed it to nothing.
                    .fixedSize()
                    .help(job.phase?.caption(kind: .image) ?? job.title)
            } else if let q = m.quantize, m.state != "installed" {
                // Made here from its source -- no download of its own.
                Button("Quantize") { model.quantize(model: m.model) }
                    .disabled(!q.ready || !m.fits)
                    .help(q.ready
                          ? String(localized: "Quantize \(q.fromName) into it: \(String(q.bits))-bit, groups of \(String(q.groupSize)), in the models folder Valtz manages")
                          : String(localized: "Download \(q.fromName) first: this is quantized from it — or link a pack already made"))
            } else if m.state != "installed" {
                Button(m.state == "partial" ? "Resume" : "Download") {
                    // A gated one asks for its license and a token first.
                    if m.isGated {
                        model.gatedDownload = m
                    } else {
                        model.download(model: m.model)
                    }
                }
                .disabled(!m.fits)
                .help(!m.fits ? "Needs a Mac with more memory"
                      : m.isGated
                      ? "Gated on Hugging Face: accept its license there, then download it with your access token"
                      : "Download into the models folder Valtz manages")
                if m.isGated {
                    Image(systemName: "lock.fill")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .help("Gated on Hugging Face: its license is accepted there first")
                }
            }
            if m.state == "installed", let c = m.compress, fetching == nil {
                compressMenu(c)
            }
            Menu {
                ForEach(LinkPlace.found) { place in
                    Button(place.title) { link(from: place.url) }
                }
                Divider()
                Button("Choose a File or Folder…") { link(from: nil) }
                if m.state == "installed" {
                    Divider()
                    Button("Check Files") { model.checkModel(m.model) }
                }
                if m.source == "link" {
                    Divider()
                    Button("Unlink") { model.unlinkModel(m.model) }
                }
            } label: {
                Image(systemName: "link")
            }
            .menuStyle(.borderlessButton)
            .fixedSize()
            .help("Use it from where it already is — its files checked first")
            if m.state == "installed" && !m.path.isEmpty {
                Button {
                    NSWorkspace.shared.activateFileViewerSelecting(
                        [URL(fileURLWithPath: m.path)])
                } label: { Image(systemName: "folder") }
                    .buttonStyle(.borderless)
                    .help("Show in Finder")
            }
            if let url = URL(string: m.url) {
                Link(destination: url) {
                    Image(systemName: "arrow.up.right.square")
                }
                // As the icons beside it, not the system's link blue.
                .foregroundStyle(.secondary)
                .help("Open its page on Hugging Face")
            }
        }
        .controlSize(.small)
    }

    /// COMPRESS (DESIGN §8b): its download replaced by a copy at 8 or 4
    /// bits, what each would take -- confirmed first; compressed, the way
    /// back (the original downloaded again); not Valtz's own download,
    /// greyed, saying why.
    @ViewBuilder
    private func compressMenu(_ c: CapabilityTree.Compress) -> some View {
        let m = member
        let style = ByteCountFormatStyle(style: .file)
        Menu {
            if let done = m.compressed {
                Text("Compressed to \(String(done.bits))-bit")
                Button("Download the Original Again (\(done.originalBytes.formatted(style)))") {
                    if m.isGated {
                        model.gatedDownload = m
                    } else {
                        model.download(model: m.model)
                    }
                }
            } else {
                Text("Compress to free disk space")
                ForEach(c.widths) { w in
                    Button("\(String(w.bits))-bit · about \(w.bytes.formatted(style)), \(w.saves.formatted(style)) freed") {
                        model.compressAsk = (m, w.bits)
                    }
                }
            }
        } label: {
            Image(systemName: "arrow.down.right.and.arrow.up.left")
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .disabled(!c.ready && m.compressed == nil)
        .help(compressHelp(c))
    }

    private func compressHelp(_ c: CapabilityTree.Compress) -> String {
        if let done = member.compressed {
            return String(localized: "Compressed to \(String(done.bits)) bits: download the original again to have it back")
        }
        switch c.why {
        case "link": return String(localized: "Linked from elsewhere: Valtz compresses only the models it downloaded, replacing the download")
        case "vpipe": return String(localized: "In vpipe's models folder: Valtz compresses only the models it downloaded, replacing the download")
        case "engine": return String(localized: "The engine here cannot compress models")
        default: return String(localized: "Compress to 8 or 4 bits to free disk space: the download is replaced by a smaller copy")
        }
    }

    /// A file or folder chosen -- starting in `from` -- linked.
    private func link(from: URL?) {
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = member.quantize != nil
            ? String(localized: "Choose a quantized \(member.label) already made: the folder holding it (vpipe's prepare-moss-tts-v1.5 writes local/MOSS-TTS-v1.5-8bit).")
            : String(localized: "Choose the weights for \(member.label): a .safetensors file or the folder holding them.")
        if let from { panel.directoryURL = from }
        if panel.runModal() == .OK, let url = panel.url {
            model.linkModel(member.model, to: url)
        }
    }
}

/// Notes under a model's row -- its files checked, its compress -- each
/// with its level's sign; closed by its ×.
private struct NotesView: View {
    let notes: [ModelCheck.Note]
    let close: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 6) {
            VStack(alignment: .leading, spacing: 2) {
                ForEach(Array(notes.prefix(5).enumerated()), id: \.offset) {
                    _, n in
                    Label {
                        Text(verbatim: n.text)
                            .fixedSize(horizontal: false, vertical: true)
                    } icon: {
                        Image(systemName: Self.symbol(n.level))
                            .foregroundStyle(Self.color(n.level))
                    }
                }
            }
            Spacer(minLength: 4)
            Button(action: close) {
                Image(systemName: "xmark").font(.caption2)
            }
            .buttonStyle(.borderless)
            .help("Dismiss")
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .textSelection(.enabled)
        .padding(.leading, 26)
    }

    static func symbol(_ level: String) -> String {
        switch level {
        case "error": "xmark.octagon.fill"
        case "warning": "exclamationmark.triangle.fill"
        default: "checkmark.seal.fill"
        }
    }

    static func color(_ level: String) -> Color {
        switch level {
        case "error": .red
        case "warning": .orange
        default: .green
        }
    }
}

/// Where models often already are: vpipe's, ComfyUI's and Draw Things'
/// models folders, as this Mac has them.
private struct LinkPlace: Identifiable {
    let title: String
    let url: URL
    var id: String { url.path }

    static var found: [LinkPlace] {
        let home = FileManager.default.homeDirectoryForCurrentUser
        let all: [(String, String)] = [
            (String(localized: "In vpipe's Models…"), "vpipe/models"),
            (String(localized: "In ComfyUI's Models…"), "ComfyUI/models"),
            (String(localized: "In ComfyUI's Models…"),
             "Documents/ComfyUI/models"),
            (String(localized: "In Draw Things' Models…"),
             "Library/Containers/com.liuliu.draw-things/Data/Documents/Models"),
        ]
        var seen: Set<String> = []
        return all.compactMap { title, rel in
            let url = home.appendingPathComponent(rel)
            guard FileManager.default.fileExists(atPath: url.path),
                  seen.insert(title).inserted else { return nil }
            return LinkPlace(title: title, url: url)
        }
    }
}

/// A GATED model's download (Settings › Capabilities): its publisher asks
/// for its license to be accepted on Hugging Face first, by the person's
/// own account, and the files then come only with that account's access
/// token. The sheet says how, and takes the token -- for this download
/// alone: it goes through the core into the fetch, in memory, and is kept
/// nowhere (not on disk, not in the settings, not in the log), so the next
/// gated model asks for it again.
struct GatedDownloadSheet: View {
    @Bindable var model: AppModel
    let member: CapabilityTree.Member
    @State private var token = ""
    @Environment(\.dismiss) private var dismiss

    private static let tokensPage =
        URL(string: "https://huggingface.co/settings/tokens")!

    private var ready: Bool {
        !token.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack(alignment: .top, spacing: 12) {
                Image(systemName: "lock.shield")
                    .font(.system(size: 30))
                    .foregroundStyle(.secondary)
                VStack(alignment: .leading, spacing: 4) {
                    Text("\(member.name) is a gated model")
                        .font(.headline)
                    Text("Its publisher asks you to accept its license on Hugging Face before you download it.")
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    if !member.license.isEmpty {
                        Text("License: \(member.license)")
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            VStack(alignment: .leading, spacing: 12) {
                step(1, "Sign in to Hugging Face and accept the license on the model's page.") {
                    if let url = URL(string: member.url) {
                        Link(destination: url) {
                            Label("Open Model Page", systemImage: "arrow.up.right.square")
                        }
                    }
                }
                step(2, "Create an access token that can read (a Read token is enough), and copy it.") {
                    Link(destination: Self.tokensPage) {
                        Label("Open Access Tokens", systemImage: "arrow.up.right.square")
                    }
                }
                step(3, "Paste the token here:") {
                    SecureField("Access token", text: $token,
                                prompt: Text(verbatim: "hf_…"))
                        .labelsHidden()
                        .textFieldStyle(.roundedBorder)
                        .frame(width: 300)
                        .onSubmit(download)
                }
            }
            HStack(alignment: .top, spacing: 8) {
                Image(systemName: "hand.raised")
                    .foregroundStyle(.secondary)
                Text("Your access token is used for this download only. Valtz sends it to Hugging Face with the download's requests and does not store it anywhere — not on disk, not in its settings, not in the log — so you paste it again for the next gated model.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            .padding(10)
            .background(.quaternary.opacity(0.5),
                        in: RoundedRectangle(cornerRadius: 8))
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { close() }
                    .keyboardShortcut(.cancelAction)
                Button("Download", action: download)
                    .keyboardShortcut(.defaultAction)
                    .disabled(!ready)
            }
        }
        .padding(20)
        .frame(width: 480)
        .onAppear {
            if let t = model.snapshotGatedToken {
                token = t
                model.snapshotGatedToken = nil
            }
        }
        // Gone with the sheet, however it closes.
        .onDisappear { token = "" }
    }

    private func step<Content: View>(
        _ n: Int, _ text: LocalizedStringKey,
        @ViewBuilder _ content: () -> Content
    ) -> some View {
        HStack(alignment: .firstTextBaseline, spacing: 10) {
            Text(verbatim: "\(n)")
                .font(.callout.weight(.semibold).monospacedDigit())
                .frame(width: 20, height: 20)
                .background(.quaternary, in: Circle())
            VStack(alignment: .leading, spacing: 6) {
                Text(text)
                    .fixedSize(horizontal: false, vertical: true)
                content()
            }
        }
    }

    private func download() {
        guard ready else { return }
        model.downloadGated(member, token: token)
        token = ""
        dismiss()
    }

    private func close() {
        token = ""
        model.gatedDownload = nil
        dismiss()
    }
}
