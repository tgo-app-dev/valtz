import SwiftUI

/// Information › Canvas: the canvas the picture or clip is shown on, and
/// -- a clip -- how long its timeline runs.
///
///   Canvas    its size; Change… opens the Canvas Size popover: a width
///             and a height, and where the picture stays as the canvas
///             grows or shrinks around it (the anchor) -- as in an image
///             editor. What it adds is clear (black in a video).
///   Length    a clip's timeline, in frames and seconds: longer, and the
///             clips in it show nothing past their last frame; shorter,
///             and they are cut. Its own length again: Reset.
struct CanvasSection: View {
    @Bindable var model: AppModel
    let asset: AssetDTO
    @State private var lengthText = ""
    @FocusState private var lengthFocused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("Canvas").font(.system(size: 11, weight: .semibold))
            // The inspector's rows: labels right-aligned in one column.
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 8,
                 verticalSpacing: 4) {
                GridRow {
                    Text("Size")
                        .foregroundStyle(.secondary)
                        .gridColumnAlignment(.trailing)
                        .frame(minWidth: 72, alignment: .trailing)
                    HStack(spacing: 6) {
                        Text(verbatim: "\(size.width) × \(size.height)")
                            .monospacedDigit()
                        Button("Change…") { model.showsCanvasSize = true }
                            .controlSize(.small)
                            .popover(isPresented: $model.showsCanvasSize,
                                     arrowEdge: .leading) {
                                CanvasSizePopover(
                                    size: size,
                                    own: model.ownFrame(of: asset)
                                ) { w, h, ax, ay in
                                    model.showsCanvasSize = false
                                    model.setCanvas(asset, width: w,
                                                    height: h, anchorX: ax,
                                                    anchorY: ay)
                                } reset: {
                                    model.showsCanvasSize = false
                                    model.resetCanvas(asset)
                                }
                            }
                    }
                }
                if asset.kind == "video" {
                    GridRow {
                        Text("Length")
                            .foregroundStyle(.secondary)
                            .frame(minWidth: 72, alignment: .trailing)
                        HStack(spacing: 6) {
                            TextField("frames", text: $lengthText)
                                .textFieldStyle(.roundedBorder)
                                .controlSize(.small)
                                .frame(width: 60)
                                .monospacedDigit()
                                .focused($lengthFocused)
                                .onSubmit(commitLength)
                            Text(verbatim: seconds)
                                .foregroundStyle(.secondary)
                                .monospacedDigit()
                            if asset.timeline != nil {
                                Button("Reset") {
                                    model.setTimeline(asset, frames: 0)
                                }
                                .controlSize(.small)
                                .help("Its own length again")
                            }
                        }
                    }
                }
            }
        }
        .onAppear { lengthText = "\(frames)" }
        .onChange(of: frames) { _, f in
            if !lengthFocused { lengthText = "\(f)" }
        }
    }

    /// The canvas as it is: its own frame, or the size it was given.
    private var size: (width: Int, height: Int) {
        if let c = asset.canvas, c.resized { return (c.w, c.h) }
        return model.ownFrame(of: asset)
    }

    /// The timeline's frames: as set, or its own -- a composition's
    /// length, a clip's frames.
    private var frames: Int {
        asset.timeline ?? (asset.isComposition ? asset.length : nil)
            ?? asset.info?.frames ?? 0
    }

    /// What its frames are counted at: a composition's own rate (its
    /// clip's may be another: 150 frames at 30 fps read as 6.25 s at a
    /// 24 fps clip's), else the clip's.
    private var rate: Double {
        (asset.compositionRate ?? asset.info?.frameRate)?.fps ?? 24
    }

    private var seconds: String {
        let fps = rate
        let s = Double(frames) / max(1, fps)
        return String(localized: "\(s.formatted(.number.precision(.fractionLength(2)))) s")
    }

    /// A number of frames, or seconds ("5 s", "2.5s").
    private func commitLength() {
        let t = lengthText.trimmingCharacters(in: .whitespaces)
            .lowercased()
        let fps = rate
        var n: Int?
        if t.hasSuffix("s"), let s = Double(t.dropLast()
            .trimmingCharacters(in: .whitespaces)) {
            n = Int((s * fps).rounded())
        } else if let f = Int(t) {
            n = f
        }
        guard let n, n > 0 else {
            lengthText = "\(frames)"
            return
        }
        model.setTimeline(asset, frames: n == (asset.info?.frames ?? -1)
                                            ? 0 : n)
    }
}

/// Canvas Size: a width and a height, and the ANCHOR -- which of nine
/// points of the picture stays where it is while the canvas changes
/// around it.
struct CanvasSizePopover: View {
    let size: (width: Int, height: Int)
    let own: (width: Int, height: Int)
    let apply: (Int, Int, Double, Double) -> Void
    let reset: () -> Void
    @State private var width = ""
    @State private var height = ""
    @State private var anchor = (x: 1, y: 1)  // 0, 1, 2 across and down

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Canvas Size").font(.headline)
            Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 8,
                 verticalSpacing: 8) {
                GridRow {
                    Text("Width").gridColumnAlignment(.trailing)
                    dimension($width)
                }
                GridRow {
                    Text("Height")
                    dimension($height)
                }
                GridRow(alignment: .top) {
                    Text("Anchor")
                    anchorGrid
                }
            }
            HStack {
                Button("Own Size") { reset() }
                    .help("Back to its own frame: \(own.width) × \(own.height)")
                Spacer()
                Button("Apply") {
                    guard let w = Int(width), let h = Int(height),
                          w > 0, h > 0 else { return }
                    apply(w, h, Double(anchor.x) / 2, Double(anchor.y) / 2)
                }
                .keyboardShortcut(.defaultAction)
                .disabled(Int(width) == nil || Int(height) == nil)
            }
        }
        .padding(16)
        .frame(width: 260)
        .onAppear {
            width = "\(size.width)"
            height = "\(size.height)"
        }
    }

    private func dimension(_ text: Binding<String>) -> some View {
        HStack(spacing: 4) {
            TextField("", text: text)
                .textFieldStyle(.roundedBorder)
                .frame(width: 70)
                .monospacedDigit()
            Text("px").foregroundStyle(.secondary)
        }
    }

    /// Nine points of the picture: the chosen one filled, arrows from it
    /// to the ones around, which way the canvas grows.
    private var anchorGrid: some View {
        Grid(horizontalSpacing: 3, verticalSpacing: 3) {
            ForEach(0..<3, id: \.self) { y in
                GridRow {
                    ForEach(0..<3, id: \.self) { x in
                        Button {
                            anchor = (x, y)
                        } label: {
                            anchorCell(x, y)
                                .frame(width: 22, height: 22)
                                .background(RoundedRectangle(cornerRadius: 4)
                                    .fill(Color.primary.opacity(0.06)))
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                        .help(Self.name(x, y))
                        .accessibilityLabel(Text(Self.name(x, y)))
                        .accessibilityAddTraits(anchor == (x, y)
                                                ? .isSelected : [])
                    }
                }
            }
        }
    }

    @ViewBuilder
    private func anchorCell(_ x: Int, _ y: Int) -> some View {
        let dx = x - anchor.x, dy = y - anchor.y
        if dx == 0 && dy == 0 {
            Image(systemName: "circle.fill").font(.system(size: 9))
        } else if abs(dx) <= 1 && abs(dy) <= 1 {
            Image(systemName: Self.arrow(dx, dy))
                .font(.system(size: 9, weight: .semibold))
                .foregroundStyle(.secondary)
        }
    }

    private static func arrow(_ dx: Int, _ dy: Int) -> String {
        switch (dx, dy) {
        case (-1, -1): "arrow.up.left"
        case (0, -1): "arrow.up"
        case (1, -1): "arrow.up.right"
        case (-1, 0): "arrow.left"
        case (1, 0): "arrow.right"
        case (-1, 1): "arrow.down.left"
        case (0, 1): "arrow.down"
        default: "arrow.down.right"
        }
    }

    private static func name(_ x: Int, _ y: Int) -> String {
        switch (x, y) {
        case (0, 0): String(localized: "Top left")
        case (1, 0): String(localized: "Top")
        case (2, 0): String(localized: "Top right")
        case (0, 1): String(localized: "Left")
        case (1, 1): String(localized: "Centre")
        case (2, 1): String(localized: "Right")
        case (0, 2): String(localized: "Bottom left")
        case (1, 2): String(localized: "Bottom")
        default: String(localized: "Bottom right")
        }
    }
}
