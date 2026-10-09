import SwiftUI

/// SwiftUI host for `CompareCanvas`.
struct CompareCanvasView: NSViewRepresentable {
    var imageA: CGImage?
    var imageB: CGImage?
    /// The same pixels as GPU surfaces, when drawn into them: shown as
    /// they are.
    var surfaceA: IOSurface? = nil
    var surfaceB: IOSurface? = nil
    var fitA = CanvasFit.identity
    var fitB = CanvasFit.identity
    var wipeLine = true
    var labelA: String
    var labelB: String
    var mode: CompareMode
    var fitRequest: Int
    var actualSizeRequest: Int
    var zoomInRequest = 0
    var zoomOutRequest = 0
    var unfitRequest = 0
    var fitMargin: CGFloat = 0.98
    var background: NSColor? = NSColor(white: 0.11, alpha: 1)
    /// A's crop (CompareCanvas.cropA), and the guides.
    var crop: CropPlacement? = nil
    var guides = false
    /// The markup toolbar's: active, its cursor, what it shows over the
    /// picture, and where its pointer and keys go.
    var markupActive = false
    /// The brush's radius (canvas pixels) while it or the eraser is the
    /// tool, else 0; its softness.
    var brushRadius: Double = 0
    var brushSoftness: Double = 0
    var markupCursor: NSCursor = .crosshair
    var markupOverlay = MarkupOverlay()
    var markupOrigin = CGPoint.zero
    var onMarkupPointer: (MarkupPointer) -> Void = { _ in }
    var onMarkupKey: (MarkupKey) -> Void = { _ in }
    var canMarkupKey: (MarkupKey) -> Bool = { _ in false }
    var onViewport: (CompareCanvas.Viewport) -> Void
    var onModeFlip: (CompareMode) -> Void = { _ in }

    final class Coordinator {
        var fit = 0
        var actual = 0
        var zoomIn = 0
        var zoomOut = 0
        var unfit = 0
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> CompareCanvas {
        let v = CompareCanvas(frame: .zero)
        v.onViewportChange = { vp in
            DispatchQueue.main.async { onViewport(vp) }
        }
        return v
    }

    func updateNSView(_ v: CompareCanvas, context: Context) {
        v.onModeFlip = { m in DispatchQueue.main.async { onModeFlip(m) } }
        v.onMarkupPointer = { e in onMarkupPointer(e) }
        v.onMarkupKey = { k in onMarkupKey(k) }
        v.canMarkupKey = { k in canMarkupKey(k) }
        v.markupActive = markupActive
        v.brushRadius = brushRadius
        v.brushSoftness = brushSoftness
        v.markupCursor = markupCursor
        v.markupOverlay = markupOverlay
        v.markupOrigin = markupOrigin
        v.fitMargin = fitMargin
        v.showsGuides = guides
        v.canvasBackground = background
        v.labelTextA = labelA
        v.labelTextB = labelB
        v.mode = mode
        v.showsWipeLine = wipeLine
        v.fitB = fitB
        v.fitA = fitA
        v.imageB = imageB
        v.imageA = imageA
        v.surfaceB = surfaceB
        v.surfaceA = surfaceA
        v.cropA = crop
        if context.coordinator.fit != fitRequest {
            context.coordinator.fit = fitRequest
            v.fitNow()
        }
        if context.coordinator.actual != actualSizeRequest {
            context.coordinator.actual = actualSizeRequest
            v.actualSize()
        }
        if context.coordinator.zoomIn != zoomInRequest {
            context.coordinator.zoomIn = zoomInRequest
            v.zoom(by: 1.5)
        }
        if context.coordinator.zoomOut != zoomOutRequest {
            context.coordinator.zoomOut = zoomOutRequest
            v.zoom(by: 1 / 1.5)
        }
        if context.coordinator.unfit != unfitRequest {
            context.coordinator.unfit = unfitRequest
            v.stopFitting()
        }
    }
}

/// A running generation, in a glass capsule: what it is doing and how
/// far along -- the bar fills as vpipe counts (a denoise per transformer
/// block, a decode per tile) and moves on its own while nothing is
/// counted yet -- and the time since Start, which keeps moving when
/// nothing else does.
/// A clip's export, over the stage while it runs: the frames it writes,
/// small, as they go -- the preview a generation shows -- with its
/// progress and Stop. It takes the pointer: the stage under it is not
/// worked on meanwhile.
struct ExportOverlay: View {
    @Bindable var model: AppModel
    var small = false

    var body: some View {
        ZStack(alignment: .bottom) {
            Color(nsColor: .textBackgroundColor)
            if let img = model.exportShow?.preview {
                Image(decorative: img, scale: 1)
                    .resizable()
                    .interpolation(.medium)
                    .aspectRatio(contentMode: .fit)
            } else {
                ProgressView()
                    .controlSize(.small)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            if small {
                ProgressView(value: model.exportPhase?.fraction)
                    .progressViewStyle(.linear)
                    .padding(8)
                    .help(model.exportCaption)
            } else {
                ExportStatus(model: model)
                    .padding(.bottom, 10)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture {}
    }
}

/// An export's bar, caption ("Exporting · 34%"), time and Stop, as
/// GenerationStatus shows a generation's.
struct ExportStatus: View {
    @Bindable var model: AppModel

    var body: some View {
        HStack(spacing: 10) {
            ProgressView(value: model.exportPhase?.fraction)
                .progressViewStyle(.linear)
                .frame(width: 140)
            Text(model.exportCaption)
                .monospacedDigit()
                .lineLimit(1)
            if let show = model.exportShow {
                Text(show.file)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                TimelineView(.periodic(from: show.started, by: 1)) { ctx in
                    Text(GenerationStatus.timing(start: show.started,
                                                 now: ctx.date,
                                                 ends: show.ends))
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                }
            }
            Button("Stop", action: model.stopExport)
        }
        .controlSize(.small)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .glassEffect(.regular, in: Capsule())
    }
}

struct GenerationStatus: View {
    @Bindable var model: AppModel
    var cancel: (() -> Void)? = nil

    var body: some View {
        HStack(spacing: 10) {
            ProgressView(value: model.generationPhase?.fraction)
                .progressViewStyle(.linear)
                .frame(width: 140)
            Text(model.generationCaption)
                .monospacedDigit()
                .lineLimit(1)
            if let start = model.generationStarted {
                TimelineView(.periodic(from: start, by: 1)) { ctx in
                    Text(Self.timing(start: start, now: ctx.date,
                                     ends: model.generationEnds))
                        .monospacedDigit()
                        .foregroundStyle(.secondary)
                        .help(model.generationEnds == nil
                              ? String(localized: "Time since Start")
                              : String(localized: "Time since Start, of the estimated total"))
                }
            }
            if let cancel {
                Button("Cancel", action: cancel)
            }
        }
        .controlSize(.small)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .glassEffect(.regular, in: Capsule())
    }

    /// "0:42", or "0:42 of ~1:30" once the core has an estimate (past 5%
    /// of a count): the total, to the second under a minute and to five
    /// seconds above. An estimate the time has passed is not shown.
    static func timing(start: Date, now: Date, ends: Date?) -> String {
        let spent = now.timeIntervalSince(start)
        guard let ends, ends > now else { return elapsed(spent) }
        var total = ends.timeIntervalSince(start)
        if total >= 60 { total = (total / 5).rounded() * 5 }
        let e = elapsed(spent)
        let t = elapsed(total)
        return String(localized: "\(e) of ~\(t)",
                      comment: "elapsed time of the estimated total")
    }

    /// "1:05".
    static func elapsed(_ s: TimeInterval) -> String {
        Duration.seconds(max(0, s.rounded(.down)))
            .formatted(.time(pattern: .minuteSecond))
    }
}
