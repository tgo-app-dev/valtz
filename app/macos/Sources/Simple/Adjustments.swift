import CoreGraphics
import CoreImage
import CoreImage.CIFilterBuiltins
import Foundation

/// Adjustments, in image space: the look of the picture on the stage --
/// a result, changed after it is made, or the picture about to be
/// edited, changed before the model sees it.
///
/// Every value is 0 at rest and runs -1...1 in the UI, except exposure
/// (stops, -2...2). WHAT EACH SLIDER DOES is defined once, in the core
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

    var isIdentity: Bool { self == ImageAdjustments() }

    enum Key: String, CaseIterable, Identifiable, Sendable {
        case exposure, contrast, highlights, shadows
        case vibrance, saturation, temperature, tint
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
            }
        }

        var range: ClosedRange<Double> { self == .exposure ? -2...2 : -1...1 }

        /// A typed value, in what `display` shows -- stops for exposure,
        /// else -100...+100 ("30", "+30", "-12.5", "30%") -- clamped to
        /// the range; nil for text that is not a number.
        func parse(_ text: String) -> Double? {
            var t = text.trimmingCharacters(in: .whitespaces)
            if t.hasSuffix("%") { t.removeLast() }
            if t.hasPrefix("+") { t.removeFirst() }
            guard let v = Double(t.replacingOccurrences(of: ",", with: ".")),
                  v.isFinite else { return nil }
            let scaled = self == .exposure ? v : v / 100
            return min(range.upperBound, max(range.lowerBound, scaled))
        }

        /// What the slider shows: stops for exposure, else -100...+100.
        func display(_ v: Double) -> String {
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
            }
        }
    }

    init() {}

    /// From a modifier's numbers (unknown names ignored).
    init(_ p: [String: Double]) {
        for key in Key.allCases {
            if let v = p[key.rawValue] { self[key] = v }
        }
    }

    /// The sliders that moved, as the core reads them ("base_adjust").
    var json: [String: Double] {
        var j: [String: Double] = [:]
        for key in Key.allCases where self[key] != 0 {
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
