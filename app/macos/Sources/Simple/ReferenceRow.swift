import SwiftUI
import UniformTypeIdentifiers

/// The prompt's STAGED MEDIA, in a row over its text (split from it by a
/// hairline, and still while the text scrolls): each picture or clip put
/// into the prompt, as a thumbnail, in the order the model gets them --
/// the base (the picture to edit) leads, ringed in the accent; each
/// picture carries the number the model calls it by.
///
///   click (released on it)   the stage previews it; again, or a click
///                            in the text, and the stage is as it was
///   drag within the row      the order the model gets them in
///   drag into the text       a mention there (released there, the
///                            stage stays as it is)
///   pencil badge             the picture to edit, or no longer
///   ×                        out of the prompt, its mentions with it
struct ReferenceRow: View {
    @Bindable var model: AppModel
    /// The thumbnail being dragged, for the row's reordering.
    @State private var dragging: UUID?

    static let thumbHeight: CGFloat = 44
    /// The row, its padding included.
    static let height: CGFloat = 62

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(model.promptAttachments) { item in
                    ReferenceThumb(model: model, item: item,
                                   dragging: $dragging)
                        // A capture's card flying into it: hidden until
                        // it lands, where it says it is.
                        .opacity(model.capturedItem == item.id ? 0 : 1)
                        .onGeometryChange(for: CGRect.self) {
                            $0.frame(in: .named(ComposerStack.cardSpace))
                        } action: { f in
                            if model.capturedItem == item.id {
                                model.capturedThumbFrame = f
                            }
                        }
                }
            }
            .padding(.leading, 14)
            // Clear stands at the row's end.
            .padding(.trailing, 46)
            .padding(.top, 9)
            .padding(.bottom, 9)
        }
        .frame(height: Self.height)
        // Fewer references, for a clip refused for memory to fit.
        .overlay {
            if model.suggestsFewerReferences {
                RoundedRectangle(cornerRadius: 10, style: .continuous)
                    .strokeBorder(Color.orange, lineWidth: 1.5)
                    .padding(4)
                    .allowsHitTesting(false)
                    .help("Fewer references for the clip to fit in memory")
            }
        }
        // Files dropped on the row are staged, mentioned nowhere; so are
        // assets dragged from the list.
        .dropDestination(for: URL.self) { urls, _ in
            // Assets of the list arrive as URLs too.
            let ids = AppModel.draggedAssets(urls)
            if !ids.isEmpty {
                model.addAssetReferences(ids)
                return true
            }
            let files = urls.filter(\.isFileURL)
            guard !files.isEmpty else { return false }
            // The stage itself (its drag carries a file too): captured.
            if files.count == 1, model.isStageDrag(files[0]) {
                model.addStageToPrompt()
                return true
            }
            model.addReferences(files)
            return true
        }
        .dropDestination(for: String.self) { items, _ in
            if items.contains(AppModel.stagePrefix) {
                model.addStageToPrompt()
                return true
            }
            let ids = items.compactMap { AppModel.draggedAsset($0) }
            guard !ids.isEmpty else { return false }
            model.addAssetReferences(ids)
            return true
        }
    }
}

private struct ReferenceThumb: View {
    @Bindable var model: AppModel
    let item: PromptAttachment
    @Binding var dragging: UUID?
    @State private var hovering = false

    private var h: CGFloat { ReferenceRow.thumbHeight }

    var body: some View {
        let thumb = model.referenceThumbs[item.id]
        let focused = model.focusedReference == item.id
        let number = model.referenceNumber(item)
        let badge = model.referenceBadge(item)
        picture(thumb)
            .frame(width: width(thumb), height: h)
            .clipShape(RoundedRectangle(cornerRadius: 8, style: .continuous))
            .overlay {
                RoundedRectangle(cornerRadius: 8, style: .continuous)
                    .strokeBorder(item.isBase ? Color.accentColor
                                  : Color.black.opacity(0.15),
                                  lineWidth: item.isBase ? 2 : 0.5)
            }
            .overlay(alignment: .bottomLeading) {
                // What the model calls it: a picture's number, or a clip's
                // reference ("P1", "V1", "A1").
                if let badge {
                    Text(verbatim: badge)
                        .font(.system(size: 9, weight: .bold))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 3)
                        .frame(minWidth: 14, minHeight: 14)
                        .background(Capsule().fill(item.isBase
                            ? Color.accentColor : .black.opacity(0.6)))
                        .padding(3)
                } else if item.kind == "video" {
                    Image(systemName: "play.fill")
                        .font(.system(size: 7))
                        .foregroundStyle(.white)
                        .frame(width: 14, height: 14)
                        .background(Circle().fill(.black.opacity(0.55)))
                        .padding(3)
                }
            }
            .overlay(alignment: .topTrailing) {
                if item.kind == "image"
                    && (item.isBase || hovering || hinted) {
                    pencil
                        // The edit-base suggestion, pointing at it.
                        .appKitPopover(isPresented: Binding(
                            get: { hinted },
                            set: { if !$0 { model.baseHint = nil } }),
                                       arrowEdge: .top) {
                            BaseHint()
                        }
                }
            }
            .overlay(alignment: .topLeading) {
                if hovering { remove }
            }
            // The clip carried on from: its tail is what the new one
            // continues.
            .overlay(alignment: .topTrailing) {
                if item.continues {
                    Image(systemName: "arrow.right.to.line")
                        .font(.system(size: 8, weight: .bold))
                        .foregroundStyle(.white)
                        .frame(width: 16, height: 16)
                        .background(Circle().fill(Color.accentColor))
                        .padding(3)
                        .help("Continued: the new clip carries on from its end")
                }
            }
            // The one the stage previews: lifted, a bar under it.
            .overlay(alignment: .bottom) {
                if focused {
                    Capsule()
                        .fill(Color.accentColor)
                        .frame(width: 16, height: 3)
                        .offset(y: 6)
                }
            }
            .scaleEffect(focused ? 1.06 : 1)
            .contentShape(Rectangle())
            .onHover { hovering = $0 }
            // On release only: a drag that ends elsewhere (in the text)
            // is not a click.
            .onTapGesture { model.toggleReferenceFocus(item.id) }
            .onDrag {
                dragging = item.id
                return NSItemProvider(object:
                    (PromptTextView.referencePrefix + item.id.uuidString)
                        as NSString)
            } preview: {
                picture(thumb)
                    .frame(width: width(thumb), height: h)
                    .clipShape(RoundedRectangle(cornerRadius: 8,
                                                style: .continuous))
            }
            .onDrop(of: [.utf8PlainText],
                    delegate: ReorderDrop(target: item.id, model: model,
                                          dragging: $dragging))
            .help(help(number))
            .contextMenu {
                if item.kind == "image" {
                    Button(item.isBase ? "Not the Picture to Edit"
                                       : "Picture to Edit") {
                        model.toggleBase(item.id)
                    }
                }
                Button("Remove from Prompt") {
                    model.removeReference(item.id)
                }
            }
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(Text(verbatim: name))
            .accessibilityAddTraits(.isButton)
            .accessibilityAddTraits(focused ? .isSelected : [])
            .animation(.smooth(duration: 0.15), value: hovering)
    }

    @ViewBuilder
    private func picture(_ cg: CGImage?) -> some View {
        if let cg {
            Image(decorative: cg, scale: 1)
                .resizable()
                .scaledToFill()
        } else {
            ZStack {
                Color.primary.opacity(0.06)
                Image(systemName: item.kind == "video" ? "film"
                                  : item.kind == "audio" ? "waveform"
                                  : item.kind == "image" ? "photo" : "doc")
                    .foregroundStyle(.secondary)
            }
        }
    }

    /// As wide as its picture's shape, within reason.
    private func width(_ cg: CGImage?) -> CGFloat {
        guard let cg, cg.height > 0 else { return h }
        let a = CGFloat(cg.width) / CGFloat(cg.height)
        return h * min(max(a, 0.75), 1.6)
    }

    private var hinted: Bool { model.baseHint == item.id && !item.isBase }

    /// The base toggle: accent on the base, faint on the others.
    private var pencil: some View {
        Button {
            model.toggleBase(item.id)
        } label: {
            Image(systemName: "pencil")
                .font(.system(size: 8, weight: .bold))
                .foregroundStyle(.white.opacity(item.isBase ? 1 : 0.85))
                .frame(width: 16, height: 16)
                .background(Circle().fill(item.isBase
                    ? Color.accentColor : .black.opacity(0.4)))
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .padding(3)
        .help(item.isBase ? "The picture to edit — click to edit none"
                          : "Make this the picture to edit")
    }

    private var remove: some View {
        Button {
            model.removeReference(item.id)
        } label: {
            Image(systemName: "xmark")
                .font(.system(size: 7, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: 15, height: 15)
                .background(Circle().fill(.black.opacity(0.55)))
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .padding(3)
        .help("Remove from the prompt")
    }

    private var name: String {
        model.asset(forFile: item.url)?.name ?? item.url.lastPathComponent
    }

    private func help(_ number: Int?) -> String {
        if item.isBase {
            return String(localized: "\(name) — the picture to edit. Click to see it on the stage; drag it into the text to mention it.")
        }
        if let number {
            return String(localized: "\(name) — picture \(String(number)). Click to see it on the stage; drag it into the text to mention it, or along the row to change the order.")
        }
        return String(localized: "\(name). Click to see it on the stage; drag it into the text to mention it.")
    }
}

/// A thumbnail dragged along the row: the others make room as it passes,
/// and it lands where it is let go.
/// The EDIT-BASE SUGGESTION: what the pencil on a picture of the row does
/// -- marked, the model changes that picture; unmarked, it draws from it.
private struct BaseHint: View {
    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "pencil.circle.fill")
                .font(.system(size: 22))
                .foregroundStyle(Color.accentColor)
            VStack(alignment: .leading, spacing: 4) {
                Text("The picture to edit?")
                    .font(.headline)
                Text("If this is the picture to change, click the pencil to mark it. Unmarked, it is a reference the model draws from.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .padding(14)
        .frame(width: 290, alignment: .leading)
    }
}

private struct ReorderDrop: DropDelegate {
    let target: UUID
    let model: AppModel
    @Binding var dragging: UUID?

    func validateDrop(info: DropInfo) -> Bool {
        dragging != nil && info.hasItemsConforming(to: [.utf8PlainText])
    }

    @MainActor
    func dropEntered(info: DropInfo) {
        guard let d = dragging, d != target,
              let to = model.promptAttachments.firstIndex(where: {
                  $0.id == target }) else { return }
        model.moveReference(d, to: to)
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: .move)
    }

    @MainActor
    func performDrop(info: DropInfo) -> Bool {
        dragging = nil
        return true
    }
}

extension AppModel {
    /// The row's pictures changed: the first picture staged in an image
    /// prompt -- not marked the base -- gets the suggestion, once its
    /// thumbnail is there (the pencil sits where it ends up); a picture
    /// gone, or marked, takes it with it.
    func suggestBase(was: [UUID], now: [UUID]) {
        if let h = baseHint, !now.contains(h) { baseHint = nil }
        guard was.isEmpty, let first = now.first,
              activeModality == .image,
              promptAttachments.first(where: { $0.id == first })?.isBase
                  == false else { return }
        Task { @MainActor [weak self] in
            for _ in 0..<40 where self?.referenceThumbs[first] == nil {
                try? await Task.sleep(for: .milliseconds(50))
            }
            try? await Task.sleep(for: .milliseconds(150))
            guard let self, self.activeModality == .image,
                  let item = self.promptAttachments.first(where: {
                      $0.id == first }), !item.isBase else { return }
            self.baseHint = first
        }
    }
}
