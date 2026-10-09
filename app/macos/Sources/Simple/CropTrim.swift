import CoreGraphics
import CoreImage
import Foundation
import SwiftUI

/// Crop and rotate, on the picture on the stage -- the Crop panel. The
/// picture (its CONTENT) lies on a CANVAS, by default its own size:
/// scaled (each axis on its own, as a share of the frame), turned about
/// its centre, its centre offset from the canvas's; where it does not
/// reach, the background colour. Zooming in crops,
/// zooming out pads.
///
/// Like the adjustments it is an IMAGE MODIFIER (core project::Modifier,
/// kind "crop"): recorded on the asset, never baked into its file, made
/// real only where pixels leave Valtz -- a model reading it as a base,
/// an export. WHERE THE CONTENT LANDS is defined once, in the core
/// (media/crop.h): the stage, B and a shared copy are drawn from the
/// placement it hands back (CoreService.cropPlacement), as the engine
/// draws the model's input.
struct CropSpec: Equatable, Sendable {
    /// The picture's size when it was cropped; 0 until the first change.
    var contentWidth = 0
    var contentHeight = 0
    /// The canvas, in the same pixels; 0 is the content's own size.
    var canvasWidth = 0
    var canvasHeight = 0
    /// Canvas pixels per content pixel, across and down: the share of the
    /// frame the picture covers (1 is the frame's own size).
    var scaleX = 1.0
    var scaleY = 1.0
    /// The content's centre from the canvas's, in canvas widths and
    /// heights (+ right, + down).
    var offsetX = 0.0
    var offsetY = 0.0
    /// Degrees, + clockwise.
    var rotate = 0.0
    /// Where the content does not reach: sRGB, straight alpha.
    var pad: [Double] = [0, 0, 0, 1]

    /// A whole turn either way (core kMaxTurn): a clip keyed 0 and 360
    /// turns all the way round.
    static let rotateRange = -360.0...360.0
    static let scaleRange = 0.05...20.0

    var isIdentity: Bool {
        scaleX == 1 && scaleY == 1 && offsetX == 0 && offsetY == 0
            && rotate == 0
            && ((canvasWidth == 0 && canvasHeight == 0)
                || (canvasWidth == contentWidth
                    && canvasHeight == contentHeight))
    }

    /// The canvas in pixels: its own, or the content's.
    var canvasSize: CGSize {
        CGSize(width: canvasWidth > 0 ? canvasWidth : contentWidth,
               height: canvasHeight > 0 ? canvasHeight : contentHeight)
    }

    /// As the core reads it (media/crop.h): flat numbers; none for the
    /// identity.
    var json: [String: Double] {
        guard !isIdentity else { return [:] }
        return [
            "content_w": Double(contentWidth),
            "content_h": Double(contentHeight),
            "canvas_w": Double(canvasWidth),
            "canvas_h": Double(canvasHeight),
            "scale_x": scaleX, "scale_y": scaleY,
            "offset_x": offsetX, "offset_y": offsetY,
            "rotate": rotate,
            "pad_r": pad[0], "pad_g": pad[1], "pad_b": pad[2],
            "pad_a": pad[3],
        ]
    }

    init() {}

    /// From an asset's "crop" modifier.
    init(_ p: [String: Double]) {
        contentWidth = Int(p["content_w"] ?? 0)
        contentHeight = Int(p["content_h"] ?? 0)
        canvasWidth = Int(p["canvas_w"] ?? 0)
        canvasHeight = Int(p["canvas_h"] ?? 0)
        scaleX = p["scale_x"] ?? p["scale"] ?? 1
        scaleY = p["scale_y"] ?? p["scale"] ?? 1
        offsetX = p["offset_x"] ?? 0
        offsetY = p["offset_y"] ?? 0
        rotate = p["rotate"] ?? 0
        pad = [p["pad_r"] ?? 0, p["pad_g"] ?? 0, p["pad_b"] ?? 0,
               p["pad_a"] ?? 1]
    }

    /// The padding as a colour well shows it.
    var padColor: CGColor {
        CGColor(srgbRed: pad[0], green: pad[1], blue: pad[2], alpha: pad[3])
    }

    /// A typed rotation ("12.5", "-3°", "+90"), held to the slider's
    /// range; nil for text that is not a number.
    static func parseRotate(_ text: String) -> Double? {
        var t = text.trimmingCharacters(in: .whitespaces)
        if t.hasSuffix("°") { t.removeLast() }
        if t.hasPrefix("+") { t.removeFirst() }
        guard let v = Double(t.replacingOccurrences(of: ",", with: ".")),
              v.isFinite else { return nil }
        return min(rotateRange.upperBound, max(rotateRange.lowerBound, v))
    }

    /// "+12.5°", one decimal.
    static func displayRotate(_ v: Double) -> String {
        (v > 0.05 ? "+" : "") + String(format: "%.1f", v) + "°"
    }
}

/// Where a crop puts the content, as the core says (media/crop.h): the
/// canvas, and content pixels -> canvas pixels in Core Image's frame
/// (origin bottom left, y up). What the stage's canvas lays out and the
/// renders below draw.
struct CropPlacement: Equatable {
    var canvas: CGSize
    var transform: CGAffineTransform
    var pad: CGColor
}

extension CropPlacement {
    /// `image` on its canvas, over the padding, as a still -- for B and a
    /// shared copy. nil if Core Image cannot render it.
    nonisolated func render(_ image: CGImage) -> CGImage? {
        let canvasRect = CGRect(origin: .zero, size: canvas)
        guard canvasRect.width >= 1, canvasRect.height >= 1 else {
            return nil
        }
        let content = CIImage(cgImage: image).transformed(by: transform)
        let padImage = CIImage(color: CIColor(cgColor: pad))
            .cropped(to: canvasRect)
        let out = content.composited(over: padImage).cropped(to: canvasRect)
        let space = image.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!
        // Drawn on the GPU into a surface, and read in place.
        return GPUPicture.render(out, extent: canvasRect,
                                 colorSpace: space)?.image
    }
}

/// A clip's marks -- the Trim panel -- as frames of the clip (0 is the
/// first); nil is unset. Both are kept. Recorded on the asset (modifier
/// "trim") and made real when it is exported, frame for frame, its sound
/// with it.
struct TrimSpec: Equatable, Sendable {
    var markIn: Int?
    var markOut: Int?

    var isIdentity: Bool { markIn == nil && markOut == nil }

    init() {}

    init(_ p: [String: Double]) {
        if let i = p["in"], i >= 0 { markIn = Int(i) }
        if let o = p["out"], o >= 0 { markOut = Int(o) }
    }

    /// As the core reads it, with the rate the marks count in.
    func json(rate: FrameRate) -> [String: Double] {
        guard !isIdentity else { return [:] }
        return ["in": Double(markIn ?? -1), "out": Double(markOut ?? -1),
                "rate_num": Double(rate.num), "rate_den": Double(rate.den)]
    }
}

/// A clip's frame rate, exactly (24000/1001 is not 23.976).
struct FrameRate: Equatable, Sendable {
    var num: Int
    var den: Int

    static let fallback = FrameRate(num: 24, den: 1)
    /// A sound's: its marks and its player's place, in milliseconds.
    static let milliseconds = FrameRate(num: 1000, den: 1)
    var fps: Double { den > 0 ? Double(num) / Double(den) : 24 }

    /// Counted in milliseconds: a sound's.
    var isMilliseconds: Bool { self == .milliseconds }

    /// The frame shown at `seconds`: the last that has begun (a hair of
    /// slack for times that land on a frame's start).
    func frame(at seconds: Double) -> Int {
        max(0, Int((seconds * fps + 1e-3).rounded(.down)))
    }

    /// When frame `n` starts, exactly: n × den / num seconds.
    func time(of n: Int) -> (value: Int64, timescale: Int32) {
        (Int64(n) * Int64(den), Int32(num))
    }

    /// "00:00:03:12": hours, minutes and whole seconds of when frame `n`
    /// shows, then which frame of that second it is (0 is the first).
    /// For a rate that is not whole, a second's first frame is the first
    /// to start in it.
    func timecode(_ n: Int) -> String {
        // A sound's: "00:01:23.456", hours to milliseconds.
        if isMilliseconds {
            let ms = max(0, n)
            let secs = ms / 1000
            return String(format: "%02d:%02d:%02d.%03d", secs / 3600,
                          secs / 60 % 60, secs % 60, ms % 1000)
        }
        let secs = Int((Double(n) * Double(den) / Double(num)).rounded(.down))
        let first = Int((Double(secs) * Double(num) / Double(den))
            .rounded(.up))
        let ff = n - first
        let h = secs / 3600, m = secs / 60 % 60, s = secs % 60
        return String(format: "%02d:%02d:%02d:%02d", h, m, s, ff)
    }

    /// A place typed in this rate's frames: a timecode as `timecode`
    /// writes it ("00:00:01:12" -- its last field the frame within the
    /// second), a frame's number ("#36", "36f"), or a time ("1:23.456",
    /// "83.456"); nil for text it cannot read.
    func parseFrame(_ text: String) -> Int? {
        let t = text.trimmingCharacters(in: .whitespaces)
        if t.hasPrefix("#") || t.lowercased().hasSuffix("f") {
            let digits = t.trimmingCharacters(
                in: CharacterSet(charactersIn: "#fF "))
            guard let n = Int(digits), n >= 0 else { return nil }
            return n
        }
        let parts = t.split(separator: ":", omittingEmptySubsequences: false)
        if parts.count == 4 && !isMilliseconds {
            let n = parts.compactMap { Int($0) }
            guard n.count == 4, n.allSatisfy({ $0 >= 0 }) else { return nil }
            let secs = n[0] * 3600 + n[1] * 60 + n[2]
            let first = Int((Double(secs) * Double(num) / Double(den))
                .rounded(.up))
            return first + n[3]
        }
        guard let seconds = FrameRate.parseTime(t) else { return nil }
        return frame(at: seconds)
    }

    /// A time typed as seconds: "83.456", "1:23.456", "00:01:23.456" (a
    /// comma for the point too); nil for one it cannot read.
    nonisolated static func parseTime(_ text: String) -> Double? {
        let t = text.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ",", with: ".")
            .replacingOccurrences(of: "s", with: "")
            .trimmingCharacters(in: .whitespaces)
        let parts = t.split(separator: ":", omittingEmptySubsequences: false)
        guard (1...3).contains(parts.count) else { return nil }
        var seconds = 0.0
        for (i, p) in parts.enumerated() {
            let last = i == parts.count - 1
            guard let v = last ? Double(p) : Double(Int(p) ?? -1),
                  v >= 0, v.isFinite, last || v == v.rounded() else {
                return nil
            }
            seconds = seconds * 60 + v
        }
        return seconds
    }
}

/// The crop's placement, as the Crop panel's text boxes show it: offsets
/// and scales relative to the frame (0, 0, 1, 1 as the picture is).
enum CropField: CaseIterable, Identifiable, Sendable {
    case offsetX, offsetY, scaleX, scaleY
    var id: Self { self }

    /// Its name, for VoiceOver (the card shows Offset / Zoom and the
    /// axis).
    var label: LocalizedStringKey {
        switch self {
        case .offsetX: "Offset X"
        case .offsetY: "Offset Y"
        case .scaleX: "Zoom X"
        case .scaleY: "Zoom Y"
        }
    }

    /// The axis it is, beside its box.
    var axis: String {
        self == .offsetX || self == .scaleX ? "X" : "Y"
    }

    /// Its wheel: an offset moves a frame's width (height) in 500 points
    /// of drag; a zoom doubles in about 170 (a factor of e^0.004 a
    /// point).
    var jogPerPoint: Double {
        self == .offsetX || self == .offsetY ? 0.002 : 0.004
    }

    var jogHelp: LocalizedStringKey {
        switch self {
        case .offsetX: "Drag the dot left or right to move the picture across (⌥ finer, ⇧ faster)"
        case .offsetY: "Drag the dot left or right to move the picture up or down (⌥ finer, ⇧ faster)"
        case .scaleX: "Drag the dot left or right to zoom out or in (⌥ finer, ⇧ faster)"
        case .scaleY: "Drag the dot left or right to zoom its height out or in (⌥ finer, ⇧ faster)"
        }
    }

    var help: LocalizedStringKey {
        switch self {
        case .offsetX: "Where the picture's centre is, across, in frame widths (+ right)"
        case .offsetY: "Where the picture's centre is, down, in frame heights (+ down)"
        case .scaleX: "The picture's width, as a share of the frame's"
        case .scaleY: "The picture's height, as a share of the frame's"
        }
    }

    func value(_ c: CropSpec) -> Double {
        switch self {
        case .offsetX: c.offsetX
        case .offsetY: c.offsetY
        case .scaleX: c.scaleX
        case .scaleY: c.scaleY
        }
    }

    /// A typed value, held to its range; nil for text that is not a
    /// number.
    func parse(_ text: String) -> Double? {
        let t = text.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: ",", with: ".")
        guard let v = Double(t.hasPrefix("+") ? String(t.dropFirst()) : t),
              v.isFinite else { return nil }
        switch self {
        case .offsetX, .offsetY: return min(10, max(-10, v))
        case .scaleX, .scaleY:
            return min(CropSpec.scaleRange.upperBound,
                       max(CropSpec.scaleRange.lowerBound, v))
        }
    }

    /// Up to three places, no trailing zeros ("0.25", "1", "-0.1").
    static func display(_ v: Double) -> String {
        var t = String(format: "%.3f", v)
        while t.contains("."), t.hasSuffix("0") { t.removeLast() }
        if t.hasSuffix(".") { t.removeLast() }
        return t == "-0" ? "0" : t
    }
}

/// The Crop panel -- a picture's, or a clip's, keyed (the guides are View
/// › Show Guides):
///
///   the background colour, where the picture does not reach
///   the placement's keyframes                         (a clip)
///   Offset  X [  ] ◉  Y [  ] ◉     Zoom  X [  ] ◉  🔒  Y [  ] ◉     Reset
///   the rotation's keyframes                          (a clip)
///   the rotation: slider and degrees                           Reset
///
/// Each value is typed in its box, or JOGGED by the wheel after it: its
/// dot dragged left or right. The lock between the zooms keeps them in
/// ratio, and the second wheel goes while it is shut.
struct CropPanel: View {
    @Bindable var model: AppModel
    /// The rows under a part's header (its keyframes, or its name) are
    /// set in by about four characters, to read as under it.
    private static let indent: CGFloat = 28

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            // The background is the canvas's: a picture's own bottom
            // layer's. A layer placed on a frame -- an upper one, any of
            // the project's -- shows what is below where it does not reach.
            if !model.isPlacedLayer(model.activeLayer) {
                HStack(spacing: 8) {
                    Text("Background")
                        .foregroundStyle(.secondary)
                        .searchMark(model.searchHits.contains(SettingsRow.cropPad))
                    ColorPicker("Background", selection: Binding(
                        get: { model.crop.padColor },
                        set: { model.setCropPad($0) }),
                        supportsOpacity: true)
                        .labelsHidden()
                    Spacer()
                }
            }
            header(.place, "Crop and zoom",
                   bypass: "Hold to see the picture without its crop and zoom",
                   reset: model.resetCropPlacement,
                   isReset: model.cropPlacementIsReset)
            // Offset and zoom on one row; short of room, one under the
            // other.
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 18) {
                    offsetGroup
                    zoomGroup
                    Spacer(minLength: 0)
                }
                VStack(alignment: .leading, spacing: 8) {
                    offsetGroup
                    zoomGroup
                }
            }
            .searchMark(model.searchHits.contains(SettingsRow.cropZoom))
            .padding(.leading, Self.indent)
            if model.canUpscaleLayer || model.upscaleJob != nil {
                upscaleRow
                    .padding(.leading, Self.indent)
            } else if model.upscaleNeedsFlatten {
                flattenFirstRow
                    .padding(.leading, Self.indent)
            }
            header(.turn, "Rotation",
                   bypass: "Hold to see the picture without its rotation",
                   reset: model.resetCropRotate,
                   isReset: model.cropRotateIsReset)
            HStack(spacing: 8) {
                CenteredSlider(value: Binding(get: { model.crop.rotate },
                                              set: { model.setCropRotate($0) }),
                               range: CropSpec.rotateRange)
                    .accessibilityLabel(Text("Rotate"))
                RotateField(model: model)
            }
            .searchMark(model.searchHits.contains(SettingsRow.cropRotate))
            .padding(.leading, Self.indent)
        }
        .controlSize(.small)
        .disabled(!model.canAdjust)
    }

    /// "Offset  X [ ] ◉  Y [ ] ◉": where the picture's centre is, in frame
    /// widths and heights.
    private var offsetGroup: some View {
        HStack(spacing: 8) {
            Text("Offset")
                .foregroundStyle(.secondary)
                .fixedSize()
            value(.offsetX, jog: true)
            value(.offsetY, jog: true)
        }
    }

    /// "Zoom  X [ ] ◉ 🔒 Y [ ] ◉": its size as a share of the frame's;
    /// locked, X and Y keep their ratio and one wheel turns both.
    private var zoomGroup: some View {
        HStack(spacing: 8) {
            Text("Zoom")
                .foregroundStyle(.secondary)
                .fixedSize()
            value(.scaleX, jog: true)
            Button {
                model.cropZoomLocked.toggle()
            } label: {
                Image(systemName: model.cropZoomLocked ? "lock.fill"
                                                       : "lock.open")
                    .frame(width: 18, height: 20)
                    .contentShape(Rectangle())
                    .contentTransition(.symbolEffect(.replace))
            }
            .buttonStyle(.borderless)
            .foregroundStyle(model.cropZoomLocked ? Color.primary
                                                  : Color.secondary)
            .help(model.cropZoomLocked
                  ? "X and Y zoom together, keeping their ratio: click to zoom them apart"
                  : "X and Y zoom apart: click to zoom them together")
            .accessibilityLabel(Text("Zoom X and Y together"))
            value(.scaleY, jog: !model.cropZoomLocked)
        }
    }

    /// An axis's box and, after it, its wheel (or the wheel's room).
    private func value(_ f: CropField, jog: Bool) -> some View {
        HStack(spacing: 4) {
            CropValueField(model: model, field: f)
            ZStack {
                if jog {
                    JogWheel(perPoint: f.jogPerPoint, help: f.jogHelp) {
                        model.nudgeCropPlacement(f, by: $0)
                    }
                    .transition(.opacity)
                }
            }
            .frame(width: JogWheel.size)
            .animation(.smooth(duration: 0.2), value: jog)
        }
    }

    /// Scaled up at one size: the clip or picture rendered at that size by
    /// an upscaler (FlashVSR, VOSR), the layer then showing it at its own
    /// pixels.
    /// While it runs, how far it is, and Stop.
    @ViewBuilder
    private var upscaleRow: some View {
        HStack(spacing: 8) {
            if let job = model.upscaleJob {
                ProgressView(value: min(1, max(0, model.jobs[job]?.progress
                                                      ?? 0)))
                    .frame(width: 120)
                Text(model.upscaleCaption)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Spacer()
                Button("Stop", action: model.stopUpscale)
            } else {
                Button {
                    model.upscaleLayer()
                } label: {
                    Label("Render Upscaled", systemImage: "sparkles.rectangle.stack")
                }
                .help("Render it at the size it is shown, with the upscaler: sharper than scaling it up")
                if let t = model.upscaleTarget {
                    Text(verbatim: "\(t.width) × \(t.height)")
                        .font(.callout.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }
        }
    }

    /// Scaled up, but a composition: an upscaler restores one picture or
    /// clip, so the way there is a flat copy first.
    private var flattenFirstRow: some View {
        HStack(spacing: 8) {
            Image(systemName: "info.circle")
                .foregroundStyle(.secondary)
            Text("\(model.activeLayerSourceName) is a composition: flatten it first to upscale it")
                .font(.callout)
                .foregroundStyle(.secondary)
                .lineLimit(2)
            Spacer()
            if model.flatteningLayer {
                ProgressView().controlSize(.small)
            }
            Button("Flatten First", action: model.flattenLayerForUpscale)
                .disabled(model.flatteningLayer)
                .help("Make a flat copy of what the layer shows -- one picture or clip, as it is drawn -- and show it in its place; then Render Upscaled")
        }
    }

    /// A part's row: its keyframes on a clip, its name on a still; Bypass
    /// and Reset at the right.
    private func header(_ part: KeyTrack, _ name: LocalizedStringKey,
                        bypass: LocalizedStringKey,
                        reset: @escaping () -> Void,
                        isReset: Bool) -> some View {
        HStack(spacing: 8) {
            if model.keyedStage {
                KeyframeBar(model: model, track: part)
            } else {
                Text(name).foregroundStyle(.secondary)
            }
            Spacer()
            BypassButton(model: model, part: part, help: bypass)
            Button("Reset", action: reset)
                .disabled(isReset)
        }
    }

}

/// One of the placement's values as a text box, as an adjustment's: taken
/// on Return or when it is left, held to its range; it follows the zoom
/// tools and a drag on the stage.
private struct CropValueField: View {
    @Bindable var model: AppModel
    let field: CropField
    @State private var text = ""
    /// What it last showed: text unlike it is being typed.
    @State private var shown = ""
    @FocusState private var focused: Bool

    var body: some View {
        let value = field.value(model.crop)
        HStack(spacing: 4) {
            Text(verbatim: field.axis)
                .foregroundStyle(.secondary)
                .fixedSize()
            TextField(field.label, text: $text)
                .labelsHidden()
                .textFieldStyle(.roundedBorder)
                .multilineTextAlignment(.trailing)
                .font(.callout.monospacedDigit())
                .focused($focused)
                .onSubmit(commit)
                .frame(width: 52)
        }
        .help(field.help)
        .onAppear { show(CropField.display(value)) }
        // It follows the value, focused or not -- the zoom tools, a drag,
        // the frame a clip shows -- unless a value is being typed in it.
        .onChange(of: value) { _, v in
            if text == shown { show(CropField.display(v)) }
        }
        .onChange(of: focused) { _, f in
            if !f { commit() }
        }
    }

    private func show(_ s: String) {
        text = s
        shown = s
    }

    private func commit() {
        if let v = field.parse(text) { model.setCropPlacement(field, v) }
        show(CropField.display(field.value(model.crop)))
    }
}

/// The rotation as a text box, as an adjustment's: taken on Return or
/// when it is left, held to the slider's range; its x clears it.
private struct RotateField: View {
    @Bindable var model: AppModel
    @State private var text = ""
    @State private var shown = ""
    @FocusState private var focused: Bool

    var body: some View {
        let value = model.crop.rotate
        HStack(spacing: 2) {
            TextField("Rotate", text: $text)
                .labelsHidden()
                .textFieldStyle(.plain)
                .multilineTextAlignment(.trailing)
                .font(.callout.monospacedDigit())
                .focused($focused)
                .onSubmit(commit)
                .frame(width: 52)
            Button {
                model.setCropRotate(0)
            } label: {
                Image(systemName: "xmark.circle.fill").font(.caption)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.tertiary)
            .help("Reset Rotate")
            .opacity(value == 0 ? 0 : 1)
            .disabled(value == 0)
        }
        .padding(.leading, 6)
        .padding(.trailing, 3)
        .padding(.vertical, 2)
        .background(RoundedRectangle(cornerRadius: 5)
            .fill(Color(nsColor: .textBackgroundColor)))
        .overlay(RoundedRectangle(cornerRadius: 5)
            .strokeBorder(Color(nsColor: .separatorColor)))
        .onAppear { show(CropSpec.displayRotate(value)) }
        // It follows the slider and the frame, focused or not, unless an
        // angle is being typed in it.
        .onChange(of: value) { _, v in
            if text == shown { show(CropSpec.displayRotate(v)) }
        }
        .onChange(of: focused) { _, f in
            if !f { commit() }
        }
    }

    private func show(_ s: String) {
        text = s
        shown = s
    }

    private func commit() {
        if let v = CropSpec.parseRotate(text) { model.setCropRotate(v) }
        show(CropSpec.displayRotate(model.crop.rotate))
    }
}

/// The Trim panel, for a clip -- the selected layer's -- everything
/// centred:
///
///   kept length          00:00:01:23 #47                     Reset
///   in ⇤ [I]   { |<, rewind, back, a frame back, pause, on, play, ffwd, >| }   [O] ⇥ out
///   Speed [1×]                 (its frames resampled: nearest)
///   Pitch [−] [+0] [+] semitones  [Hold | Follow Speed]     (a sound)
///   Volume 🔈 ────●── [0.0 dB]                               (a sound)
///   Starts on the timeline at [00:00:02:00] ⇥ ↩
///
/// Every time around the transport is the CLIP's own -- where the player
/// is in it (hours, minutes and seconds, the frame within that second,
/// the frame's number), its marks, what they keep -- and every row down
/// to the volume changes the clip as instantiated (its layer). The last
/// row alone is the composition's timeline: where the clip lies on it.
/// Past the clip's end (or before its start) the time says so, and the
/// stage shows a film's torn edge.
struct TrimPanel: View {
    @Bindable var model: AppModel
    /// Each side of the transport, for its mark -- a time and two
    /// buttons; a sound's time a field -- so the transport stays centred.
    private var side: CGFloat { model.trimRate.isMilliseconds ? 172 : 150 }

    var body: some View {
        let rate = model.stageFrameRate
        VStack(spacing: 12) {
            ZStack {
                clipTime
                    .searchMark(model.searchHits.contains(SettingsRow.trimNow))
                HStack {
                    if let n = model.trimmedFrames {
                        Text(verbatim: model.trimLengthText(n))
                            .font(.callout.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Reset", action: model.resetTrim)
                        .disabled(model.trim.isIdentity)
                }
            }
            // The marks at the row's ends; short of room, under it.
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 6) {
                    markIn
                        .frame(width: side, alignment: .leading)
                    Spacer(minLength: 0)
                    transport
                    Spacer(minLength: 0)
                    markOut
                        .frame(width: side, alignment: .trailing)
                }
                VStack(spacing: 10) {
                    transport
                    HStack(spacing: 36) {
                        markIn
                        markOut
                    }
                }
            }
            if model.layerIsTimed {
                SpeedRow(model: model)
                if model.layerHasSound {
                    PitchRow(model: model)
                    VolumeRow(model: model)
                }
            }
            start(rate: rate)
        }
        .controlSize(.small)
        .disabled(model.isGenerating)
    }

    /// Where the player is in the clip itself -- or that it is not in it.
    @ViewBuilder
    private var clipTime: some View {
        let r = model.trimRate
        HStack(spacing: 6) {
            if let edge = model.clipEdge {
                ZigzagStrip(teethOnLeft: edge == .after, tooth: 6)
                    .fill(Color.secondary)
                    .frame(width: 7, height: 16)
                Text(edge == .after ? "After the clip's end"
                                    : "Before the clip's start")
                    .font(.title3)
                    .foregroundStyle(.secondary)
            } else {
                let n = model.sourceFrameAtPlayhead
                Text(verbatim: r.timecode(n))
                    .font(.title3.monospacedDigit())
                // A sound's marks are times, not frames.
                if !r.isMilliseconds {
                    Text(verbatim: "#\(n)")
                        .font(.title3.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }
            if abs(model.videoRate) > 1 {
                Text(verbatim: speed(model.videoRate))
                    .font(.callout.monospacedDigit())
                    .foregroundStyle(.tertiary)
            }
        }
        .help("Where the player is in the clip itself: its own time, not the timeline's")
    }

    /// Where the selected layer starts on the composition's timeline
    /// (DESIGN §6a): a time to type, or where the player is.
    private func start(rate: FrameRate) -> some View {
        HStack(spacing: 6) {
            Text("Starts on the timeline at")
                .foregroundStyle(.secondary)
            StartTimeField(model: model,
                           shown: rate.timecode(model.trimOffset))
            TransportButton(symbol: "arrow.right.to.line.compact",
                            help: "Start here: where the player is on the timeline",
                            on: model.trimOffset == model.videoFrame) {
                model.setStartAtPlayhead()
            }
            TransportButton(symbol: "arrow.uturn.backward",
                            help: "Go to its start", on: false) {
                model.seekVideo(to: model.trimOffset)
            }
        }
        .searchMark(model.searchHits.contains(SettingsRow.trimStart))
    }

    /// { the clip's start, fast rewind, 1× back, a frame back, pause, a
    /// frame on, 1×, fast forward, the clip's end }; the one playing is
    /// pressed in. A frame's step is heard (← and → step too).
    private var transport: some View {
        let r = model.videoRate
        return HStack(spacing: 4) {
            TransportButton(symbol: "backward.end.fill",
                            help: "Go to the clip's start", on: false) {
                model.seekClipEdge(end: false)
            }
            TransportButton(symbol: "backward.fill", help: "Fast rewind",
                            on: r < -1, action: model.fastRewind)
            TransportButton(symbol: "play.fill", mirrored: true,
                            help: "Play backward", on: r == -1) {
                model.playVideo(rate: -1)
            }
            TransportButton(symbol: "backward.frame.fill",
                            help: model.stageIsAudio
                                ? "Back 10 ms, heard (←; ⌥: 1 ms)"
                                : "Back one frame, heard (←)",
                            on: false) {
                model.stepVideo(-1)
            }
            TransportButton(symbol: "pause.fill", help: "Pause",
                            on: r == 0) {
                model.playVideo(rate: 0)
            }
            TransportButton(symbol: "forward.frame.fill",
                            help: model.stageIsAudio
                                ? "On 10 ms, heard (→; ⌥: 1 ms)"
                                : "On one frame, heard (→)",
                            on: false) {
                model.stepVideo(1)
            }
            TransportButton(symbol: "play.fill", help: "Play", on: r == 1) {
                model.playVideo(rate: 1)
            }
            TransportButton(symbol: "forward.fill", help: "Fast forward",
                            on: r > 1, action: model.fastForward)
            TransportButton(symbol: "forward.end.fill",
                            help: "Go to the clip's end", on: false) {
                model.seekClipEdge(end: true)
            }
        }
    }

    /// The mark-in at the row's left: its time, ⇤, [I].
    private var markIn: some View {
        let i = model.trim.markIn
        return HStack(spacing: 4) {
            markTime(i, rate: model.trimRate, isIn: true)
            TransportButton(symbol: "arrow.left.to.line",
                            help: "Go to the mark-in", on: false) {
                if let i {
                    model.seekVideo(to: model.timelineFrame(ofSource: i))
                }
            }
            .disabled(i == nil)
            TransportButton(symbol: "i.square", help: "Mark In",
                            on: i != nil && i == model.sourceFrameAtPlayhead
                                && model.clipEdge == nil) {
                model.setMark(in: true)
            }
            .searchMark(model.searchHits.contains(SettingsRow.markIn))
        }
    }

    /// The mark-out at the row's right: [O], ⇥, its time.
    private var markOut: some View {
        let o = model.trim.markOut
        return HStack(spacing: 4) {
            TransportButton(symbol: "o.square", help: "Mark Out",
                            on: o != nil && o == model.sourceFrameAtPlayhead
                                && model.clipEdge == nil) {
                model.setMark(in: false)
            }
            .searchMark(model.searchHits.contains(SettingsRow.markOut))
            TransportButton(symbol: "arrow.right.to.line",
                            help: "Go to the mark-out", on: false) {
                if let o {
                    model.seekVideo(to: model.timelineFrame(ofSource: o))
                }
            }
            .disabled(o == nil)
            markTime(o, rate: model.trimRate, isIn: false)
        }
    }

    /// "00:00:00:12" over "#12", on its outer side, or a dash for a mark
    /// not set. A sound's is a time to type, to the millisecond.
    @ViewBuilder
    private func markTime(_ mark: Int?, rate: FrameRate,
                          isIn: Bool) -> some View {
        if rate.isMilliseconds {
            MarkTimeField(model: model, isIn: isIn,
                          shown: mark.map { rate.timecode($0) } ?? "")
        } else if let mark {
            VStack(alignment: isIn ? .leading : .trailing, spacing: 0) {
                Text(verbatim: rate.timecode(mark))
                    .font(.callout.monospacedDigit())
                Text(verbatim: "#\(mark)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.tertiary)
            }
        } else {
            Text("Not set")
                .foregroundStyle(.tertiary)
        }
    }

    /// "4×", "−2×".
    private func speed(_ r: Float) -> String {
        (r < 0 ? "−" : "") + String(Int(abs(r).rounded())) + "×"
    }
}

/// A sound's mark as a time to type -- "00:01:23.456", "1:23.456" or
/// "83.456" -- set on Return or when the field is left; it shows the mark
/// again as it is, and follows it unless a time is being typed.
private struct MarkTimeField: View {
    @Bindable var model: AppModel
    let isIn: Bool
    let shown: String
    @State private var text = ""
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField(isIn ? "Mark in" : "Mark out", text: $text)
            .textFieldStyle(.roundedBorder)
            .font(.callout.monospacedDigit())
            .multilineTextAlignment(.center)
            .frame(width: 102)
            .focused($focused)
            .onSubmit(commit)
            .onChange(of: focused) { _, now in
                if !now { commit() }
            }
            .onAppear { show(shown) }
            .onChange(of: shown) { _, s in
                if text == lastShown { show(s) }
            }
            .help("Type a time: 1:23.456, or seconds")
    }

    private func show(_ s: String) {
        text = s
        lastShown = s
    }

    private func commit() {
        if text != lastShown && !text.isEmpty {
            if !model.setMark(in: isIn, typed: text) { NSSound.beep() }
        }
        show(shown)
    }
}

/// Where the selected layer starts on the composition's timeline, as a
/// time to type -- a timecode as it shows ("00:00:01:12"), a frame
/// ("#36"), or seconds ("1:23.456", "83.456") -- set on Return or when
/// the field is left.
private struct StartTimeField: View {
    @Bindable var model: AppModel
    let shown: String
    @State private var text = ""
    @State private var lastShown = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("Start", text: $text)
            .textFieldStyle(.roundedBorder)
            .font(.body.monospacedDigit())
            .multilineTextAlignment(.center)
            .frame(width: 118)
            .focused($focused)
            .onSubmit(commit)
            .onChange(of: focused) { _, now in
                if !now { commit() }
            }
            .onAppear { show(shown) }
            .onChange(of: shown) { _, s in
                if text == lastShown { show(s) }
            }
            .help("Where this clip starts in the composition's timeline -- not a time in the clip. Type a timecode (00:00:01:12), a frame (#36) or seconds")
    }

    private func show(_ s: String) {
        text = s
        lastShown = s
    }

    private func commit() {
        if text != lastShown && !text.isEmpty {
            if !model.setStart(typed: text) { NSSound.beep() }
        }
        show(shown)
    }
}

/// One of the Trim panel's buttons (and the timeline's): its symbol
/// (mirrored for playing backward) in a circle that shows it pressed in.
struct TransportButton: View {
    let symbol: String
    var mirrored = false
    let help: LocalizedStringKey
    let on: Bool
    let action: () -> Void
    @State private var hovering = false
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 14))
                .scaleEffect(x: mirrored ? -1 : 1, y: 1)
                .frame(width: 30, height: 30)
                .background {
                    Circle().fill(Color.primary.opacity(
                        on ? 0.14 : hovering && enabled ? 0.06 : 0))
                }
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .foregroundStyle(enabled ? .primary : .tertiary)
        .accessibilityLabel(Text(help))
        .help(help)
        .onHover { hovering = $0 }
    }
}

/// Held down, the stage shows the picture without one part of its look
/// (`part`: the adjustments, the crop and zoom, the rotation); let go, it
/// shows it again. Only the showing changes -- nothing is recorded.
struct BypassButton: View {
    @Bindable var model: AppModel
    let part: KeyTrack
    let help: LocalizedStringKey

    var body: some View {
        Button("Bypass") {}
            .buttonStyle(HoldButtonStyle { model.setBypass(part, $0) })
            .disabled(!model.canBypass(part))
            .help(help)
    }
}

/// A push button that says when it is held, and lets go if it leaves the
/// screen while held (a panel closed under the pointer).
private struct HoldButtonStyle: ButtonStyle {
    let onHold: (Bool) -> Void

    func makeBody(configuration: Configuration) -> some View {
        HoldButton(pressed: configuration.isPressed, onHold: onHold) {
            configuration.label
        }
    }
}

private struct HoldButton<Label: View>: View {
    let pressed: Bool
    let onHold: (Bool) -> Void
    @ViewBuilder let label: Label
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        // Drawn as the small push buttons beside it (Reset), and filled
        // with the accent while held.
        label
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .foregroundStyle(pressed ? Color.white : Color.primary)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(pressed ? AnyShapeStyle(Color.accentColor)
                              : AnyShapeStyle(.quaternary)))
            .opacity(enabled ? 1 : 0.45)
            .contentShape(Rectangle())
            .onChange(of: pressed) { _, p in onHold(p) }
            .onDisappear { if pressed { onHold(false) } }
    }
}
