import CoreGraphics
import CoreImage
import CoreImage.CIFilterBuiltins
import Foundation
import SwiftUI

/// Adjustments, in image space: the look of the picture on the stage --
/// a result, changed after it is made, or the picture about to be
/// edited, changed before the model sees it.
///
/// Every value is 0 at rest and runs -1...1 in the UI, except exposure
/// (stops, -2...2) and the alpha (1 at rest, 0 clear: the layer's master
/// opacity, the Adjust panel's fader). WHAT EACH SLIDER DOES is defined
/// once, in the core
/// (media/adjust.h): it hands back the Core Image chain, which this
/// renders for the stage and the engine renders for the model -- on top
/// of a RAW's development, in extended linear sRGB -- so the stage shows
/// what the model gets. Non-destructive: the original stays untouched
/// and Reset brings it back.
struct ImageAdjustments: Equatable, Sendable {
    var exposure: Double = 0      // stops
    var contrast: Double = 0
    var highlights: Double = 0
    var shadows: Double = 0
    var vibrance: Double = 0
    var saturation: Double = 0
    var temperature: Double = 0   // + warmer
    var tint: Double = 0          // + magenta, - green
    var alpha: Double = 1         // 0 clear ... 1 as it is
    /// The Color panel's (ColorMixer.swift) and the Grade panel's
    /// (ColorGrading.swift): the same look, keyed with the rest.
    var mix = ColorMix()
    var grade = ColorGrade()

    var isIdentity: Bool { self == ImageAdjustments() }
    /// The Adjust panel's sliders all at rest (the mixer and the grade
    /// aside).
    var toneIsIdentity: Bool {
        var t = self
        t.mix = ColorMix()
        t.grade = ColorGrade()
        return t.isIdentity
    }

    enum Key: String, CaseIterable, Identifiable, Sendable {
        case exposure, contrast, highlights, shadows
        case vibrance, saturation, temperature, tint
        case alpha
        var id: String { rawValue }

        var label: String {
            switch self {
            case .exposure: String(localized: "Exposure")
            case .contrast: String(localized: "Contrast")
            case .highlights: String(localized: "Highlights")
            case .shadows: String(localized: "Shadows")
            case .vibrance: String(localized: "Vibrance")
            case .saturation: String(localized: "Saturation")
            case .temperature: String(localized: "Temperature")
            case .tint: String(localized: "Tint")
            case .alpha: String(localized: "Alpha")
            }
        }

        var range: ClosedRange<Double> {
            switch self {
            case .exposure: -2...2
            case .alpha: 0...1
            default: -1...1
            }
        }

        /// Where nothing changes.
        var rest: Double { self == .alpha ? 1 : 0 }

        /// A typed value, in what `display` shows -- stops for exposure,
        /// else -100...+100 ("30", "+30", "-12.5", "30%"; the alpha
        /// 0...100) -- clamped to the range; nil for text that is not a
        /// number.
        func parse(_ text: String) -> Double? {
            var t = text.trimmingCharacters(in: .whitespaces)
            if t.hasSuffix("%") { t.removeLast() }
            if t.hasPrefix("+") { t.removeFirst() }
            guard let v = Double(t.replacingOccurrences(of: ",", with: ".")),
                  v.isFinite else { return nil }
            let scaled = self == .exposure ? v : v / 100
            return min(range.upperBound, max(range.lowerBound, scaled))
        }

        /// What the slider shows: stops for exposure, the alpha a
        /// percentage, else -100...+100.
        func display(_ v: Double) -> String {
            if self == .alpha { return "\(Int((v * 100).rounded()))%" }
            let sign = v > 0.0005 ? "+" : ""
            if self == .exposure {
                return sign + String(format: "%.2f", v)
            }
            return sign + String(Int((v * 100).rounded()))
        }
    }

    subscript(key: Key) -> Double {
        get {
            switch key {
            case .exposure: exposure
            case .contrast: contrast
            case .highlights: highlights
            case .shadows: shadows
            case .vibrance: vibrance
            case .saturation: saturation
            case .temperature: temperature
            case .tint: tint
            case .alpha: alpha
            }
        }
        set {
            switch key {
            case .exposure: exposure = newValue
            case .contrast: contrast = newValue
            case .highlights: highlights = newValue
            case .shadows: shadows = newValue
            case .vibrance: vibrance = newValue
            case .saturation: saturation = newValue
            case .temperature: temperature = newValue
            case .tint: tint = newValue
            case .alpha: alpha = newValue
            }
        }
    }

    init() {}

    /// From a modifier's numbers (unknown names ignored).
    init(_ p: [String: Double]) {
        for key in Key.allCases {
            if let v = p[key.rawValue] { self[key] = v }
        }
        mix = ColorMix(p)
        grade = ColorGrade(p)
    }

    /// The sliders that moved, as the core reads them ("base_adjust"):
    /// the mixer's and the grade's beside the rest ("mix_hue_0",
    /// "grade_shadows_hue").
    var json: [String: Double] {
        var j: [String: Double] = mix.json.merging(grade.json) { a, _ in a }
        for key in Key.allCases where self[key] != key.rest {
            j[key.rawValue] = self[key]
        }
        return j
    }

    // MARK: - Rendering

    /// `image` with `chain`'s filters on it (a clip's frame, as the player
    /// draws it, or a still).
    nonisolated static func applyChain(_ chain: [Step],
                                       to image: CIImage) -> CIImage {
        var ci = image
        for step in chain {
            // The color mixer: Valtz's own kernel, not a stock filter.
            if step.filter == ColorMixKernel.filter {
                ci = ColorMixKernel.apply(ci, step.params)
                continue
            }
            // The color grade: the same.
            if step.filter == ColorGradeKernel.filter {
                ci = ColorGradeKernel.apply(ci, step.params)
                continue
            }
            guard let f = CIFilter(name: step.filter) else { continue }
            f.setValue(ci, forKey: kCIInputImageKey)
            for (key, v) in step.params {
                if v.count == 1 {
                    f.setValue(v[0], forKey: key)
                } else {
                    f.setValue(CIVector(values: v.map { CGFloat($0) },
                                        count: v.count), forKey: key)
                }
            }
            ci = f.outputImage ?? ci
        }
        return ci
    }

    /// One Core Image filter of the chain (CoreService.adjustmentChain):
    /// a parameter is a number (one value) or a CIVector.
    struct Step: Sendable {
        let filter: String
        let params: [String: [Double]]
    }

    /// `image` a power of two smaller -- 1 / 2^level of its size, by
    /// Lanczos, in half floats in its own colour space -- what the live
    /// draws read while a slider moves (a 48 MP picture's look at a
    /// quarter ~3 ms, against ~22 at its size); nil if Core Image cannot.
    nonisolated static func level(_ level: Int,
                                  of image: CGImage) -> CGImage? {
        let f = 1 / Double(1 << max(0, level))
        let w = max(1, (Double(image.width) * f).rounded())
        let h = max(1, (Double(image.height) * f).rounded())
        let space = image.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!
        let small = CIImage(cgImage: image).clampedToExtent()
            .applyingFilter("CILanczosScaleTransform", parameters: [
                kCIInputScaleKey: h / Double(image.height),
                kCIInputAspectRatioKey: (w / Double(image.width))
                    / (h / Double(image.height)),
            ])
        let context = CIContext(options: [
            .workingColorSpace: CGColorSpace(
                name: CGColorSpace.extendedLinearSRGB)!,
            .workingFormat: CIFormat.RGBAh,
            .cacheIntermediates: false,
        ])
        return context.createCGImage(
            small, from: CGRect(x: 0, y: 0, width: w, height: h),
            format: .RGBAh, colorSpace: space)
    }

    /// `image` with `chain` applied, in its own color space, drawn on the
    /// GPU into a surface the stage shows as it is (GPUPicture); nil if
    /// Core Image cannot render it.
    nonisolated static func render(_ chain: [Step],
                                   to image: CGImage) -> GPUPicture? {
        let extent = CGRect(x: 0, y: 0, width: image.width,
                            height: image.height)
        let ci = applyChain(chain, to: CIImage(cgImage: image))
        let space = image.colorSpace ?? CGColorSpace(name: CGColorSpace.sRGB)!
        return GPUPicture.render(ci, extent: extent, colorSpace: space)
    }
}

/// The adjustments panel: eight sliders over the picture on the stage,
/// applied live -- a result, or the picture to edit, which the model then
/// receives adjusted (on top of a RAW's development). The values are
/// recorded on the image, applied when a file is made from it; the
/// original is kept. Each value is a field too: type one, or clear it
/// with its x; Reset clears them all. Laid out for the inspector's
/// Adjust section, 280 to 480 points wide: the alpha a row over the
/// eight, in one column, the note under them; the keyframes are the
/// section header's.
struct AdjustPanel: View {
    @Bindable var model: AppModel

    private let keys: [ImageAdjustments.Key] = [
        .exposure, .contrast, .highlights, .shadows,
        .temperature, .tint, .vibrance, .saturation,
    ]

    private var note: LocalizedStringKey {
        !model.canAdjust
            ? "Adjustments wait for the image being made."
            : model.adjustsBase
            ? "Applied as the picture is edited and saved; the original is kept."
            : "Applied when the image is saved; the original is kept."
    }

    /// Bypass and Reset.
    @ViewBuilder
    private var buttons: some View {
        BypassButton(model: model, part: .adjust,
                     help: "Hold to see the picture without its adjustments")
        // Its own sliders: the mixer is the Color panel's.
        Button("Reset", action: model.resetAdjustments)
            .disabled((model.keyedStage
                      ? !model.clipAdjustKeys.keys.contains {
                          !$0.value.toneIsIdentity }
                          || model.layerOffStage != nil
                      : model.adjustments.toneIsIdentity)
                      || model.selectedLayerLocked)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Spacer()
                buttons
            }
            Grid(alignment: .leading, horizontalSpacing: 8,
                 verticalSpacing: 8) {
                alphaRow
                GridRow {
                    Divider().gridCellColumns(3)
                }
                ForEach(keys) { row($0) }
            }
            .focusSection()
            // Greyed while the playhead is off the selected layer -- its
            // values would change unseen -- or it is locked.
            .disabled(!model.canEditLayerHere)
            if !model.keyedStage {
                Text(note)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .controlSize(.small)
    }

    /// The layer's master alpha as a row: 100% as it is.
    private var alphaRow: some View {
        let key = ImageAdjustments.Key.alpha
        return GridRow {
            Text(key.label)
                .foregroundStyle(.secondary)
                .gridColumnAlignment(.trailing)
                .onTapGesture(count: 2) { model.setAdjustment(.alpha, 1) }
                .help("Double-click for 100%")
            CenteredSlider(value: Binding(get: { model.adjustments.alpha },
                                          set: { model.setAdjustment(.alpha, $0) }),
                           range: key.range, zero: 1)
                .frame(minWidth: 60)
                .accessibilityLabel(Text(key.label))
                .help("The layer's alpha: 100% as it is, lower to let what is under it show through, 0% clear.")
            Text(verbatim: key.display(model.adjustments.alpha))
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
        }
    }

    private func row(_ key: ImageAdjustments.Key) -> some View {
        GridRow {
            Text(key.label)
                .foregroundStyle(.secondary)
                .gridColumnAlignment(.trailing)
                .onTapGesture(count: 2) { model.setAdjustment(key, 0) }
                .help("Double-click to reset")
            CenteredSlider(value: Binding(get: { model.adjustments[key] },
                                          set: { model.setAdjustment(key, $0) }),
                           range: key.range)
                .frame(minWidth: 60)
                .accessibilityLabel(Text(key.label))
            AdjustValueField(model: model, key: key)
        }
    }
}

/// One adjustment's value as a text box: typed values are taken on
/// Return or when the field is left (exposure in stops, the others
/// -100...+100), clamped to the slider's range; the x on its right clears
/// that value alone.
private struct AdjustValueField: View {
    @Bindable var model: AppModel
    let key: ImageAdjustments.Key
    @State private var text = ""
    /// What it last showed: text unlike it is being typed.
    @State private var shown = ""
    @FocusState private var focused: Bool

    var body: some View {
        let value = model.adjustments[key]
        HStack(spacing: 2) {
            // Named for VoiceOver by its row; the row shows the name.
            TextField(key.label, text: $text)
                .labelsHidden()
                .textFieldStyle(.plain)
                .multilineTextAlignment(.trailing)
                .font(.callout.monospacedDigit())
                .focused($focused)
                .onSubmit(commit)
                .frame(width: 42)
            Button {
                model.setAdjustment(key, 0)
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.caption)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(.tertiary)
            .help("Reset \(key.label)")
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
        .onAppear { show(key.display(value)) }
        // It follows the slider and the frame a clip shows, focused or
        // not, unless a value is being typed in it.
        .onChange(of: value) { _, v in
            if text == shown { show(key.display(v)) }
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
        if let v = key.parse(text) { model.setAdjustment(key, v) }
        show(key.display(model.adjustments[key]))
    }
}
