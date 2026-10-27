import AppKit
import CoreImage
import SwiftUI

/// The Grade panel's COLOR GRADE (the core's media/adjust.h ColorGrade):
/// a TINT for each tone range -- shadows, midtones, highlights -- a hue
/// and a saturation each, set on a colour wheel; and where the ranges
/// lie: the midtones' BALANCE, their WIDTH, how the ranges BLEND. It is
/// on the layer's look beside Adjust's sliders and the mixer's curves,
/// keyed with them. What it does to a pixel is the core's: its kernel's
/// source comes over the bridge, and the chain's step carries numbers the
/// core works out. The ranges are worked out here too (`weights`), as the
/// core works them out, so the panel can draw them.

enum GradeRange: String, CaseIterable, Identifiable, Sendable {
    case shadows, midtones, highlights
    var id: String { rawValue }

    var label: String {
        switch self {
        case .shadows: String(localized: "Shadows")
        case .midtones: String(localized: "Midtones")
        case .highlights: String(localized: "Highlights")
        }
    }
}

/// A range's tint: a hue (degrees, 0..<360) and a saturation (0...1); no
/// hue while untinted, as the core reads one.
struct GradeTint: Equatable, Sendable {
    private(set) var hue = 0.0
    private(set) var saturation = 0.0

    init(hue: Double = 0, saturation: Double = 0) {
        let s = min(1, max(0, saturation.isFinite ? saturation : 0))
        self.saturation = s
        self.hue = s > 0 && hue.isFinite ? MixCurve.wrap(hue) : 0
    }

    /// From its point on the wheel's disk (held to it).
    init(x: Double, y: Double) {
        let s = min(1, hypot(x, y))
        guard s > 1e-12 else {
            self.init()
            return
        }
        self.init(hue: atan2(y, x) * 180 / .pi, saturation: s)
    }

    /// Its point on the wheel's disk: the hue its angle, from red's at
    /// the right counter-clockwise, the saturation its distance out.
    var x: Double { saturation * cos(hue * .pi / 180) }
    var y: Double { saturation * sin(hue * .pi / 180) }
}

/// A tone's weights in the three ranges (they sum to 1).
struct GradeWeights: Equatable {
    var shadows = 0.0
    var midtones = 0.0
    var highlights = 0.0

    subscript(r: GradeRange) -> Double {
        switch r {
        case .shadows: shadows
        case .midtones: midtones
        case .highlights: highlights
        }
    }
}

struct ColorGrade: Equatable, Sendable {
    var shadows = GradeTint()
    var midtones = GradeTint()
    var highlights = GradeTint()
    var balance = 0.0
    var width = 0.5
    var blend = 0.5

    /// Nothing tinted (the ranges may be set).
    var isIdentity: Bool {
        shadows.saturation == 0 && midtones.saturation == 0
            && highlights.saturation == 0
    }
    /// All at rest: nothing for Reset.
    var isEmpty: Bool { self == ColorGrade() }

    subscript(r: GradeRange) -> GradeTint {
        get {
            switch r {
            case .shadows: shadows
            case .midtones: midtones
            case .highlights: highlights
            }
        }
        set {
            switch r {
            case .shadows: shadows = newValue
            case .midtones: midtones = newValue
            case .highlights: highlights = newValue
            }
        }
    }

    /// The ranges' numbers, as the core keys them ("grade_balance").
    enum Number: String, CaseIterable, Identifiable, Sendable {
        case balance, width, blend
        var id: String { rawValue }
        var key: String { "grade_\(rawValue)" }
        var range: ClosedRange<Double> { self == .balance ? -1...1 : 0...1 }
        var rest: Double { self == .balance ? 0 : 0.5 }

        var label: String {
            switch self {
            case .balance: String(localized: "Balance")
            case .width: String(localized: "Midtone Width")
            case .blend: String(localized: "Blend")
            }
        }

        var help: String {
            switch self {
            case .balance: String(localized: "Where the midtones lie: toward the shadows, or toward the highlights. Double-click to reset.")
            case .width: String(localized: "How wide the midtones are: where they are at half strength with no blend, and the shadows and highlights take over. Double-click to reset.")
            case .blend: String(localized: "How gradually one range gives way to the next: a clean cut at 0, the most gradual at 100. Double-click to reset.")
            }
        }

        /// What its slider shows: -100...+100 for the balance, else
        /// 0...100.
        func display(_ v: Double) -> String {
            let n = Int((v * 100).rounded())
            return self == .balance && n > 0 ? "+\(n)" : "\(n)"
        }
    }

    subscript(n: Number) -> Double {
        get {
            switch n {
            case .balance: balance
            case .width: width
            case .blend: blend
            }
        }
        set {
            let v = min(n.range.upperBound, max(n.range.lowerBound, newValue))
            switch n {
            case .balance: balance = v
            case .width: width = v
            case .blend: blend = v
            }
        }
    }

    init() {}

    /// From a modifier's numbers, as the core reads them:
    /// "grade_shadows_hue", "grade_shadows_sat", ..., "grade_balance",
    /// "grade_width", "grade_blend".
    init(_ p: [String: Double]) {
        for r in GradeRange.allCases {
            let k = "grade_\(r.rawValue)"
            self[r] = GradeTint(hue: p[k + "_hue"] ?? 0,
                                saturation: p[k + "_sat"] ?? 0)
        }
        for n in Number.allCases {
            if let v = p[n.key], v.isFinite { self[n] = v }
        }
    }

    /// What is not at rest, as the core reads it.
    var json: [String: Double] {
        var j: [String: Double] = [:]
        for r in GradeRange.allCases where self[r].saturation > 0 {
            j["grade_\(r.rawValue)_hue"] = self[r].hue
            j["grade_\(r.rawValue)_sat"] = self[r].saturation
        }
        for n in Number.allCases where self[n] != n.rest {
            j[n.key] = self[n]
        }
        return j
    }

    /// Four made one, number by number (a key's lerp or spline): each
    /// tint as its point on the disk, the ranges as numbers -- the core's
    /// zip_grade.
    static func zip(_ p: Self, _ a: Self, _ b: Self, _ n: Self,
                    _ f: (Double, Double, Double, Double) -> Double) -> Self {
        var o = ColorGrade()
        for r in GradeRange.allCases {
            if p[r] == a[r] && b[r] == a[r] && n[r] == a[r] {
                o[r] = a[r]
                continue
            }
            o[r] = GradeTint(x: f(p[r].x, a[r].x, b[r].x, n[r].x),
                             y: f(p[r].y, a[r].y, b[r].y, n[r].y))
        }
        for k in Number.allCases { o[k] = f(p[k], a[k], b[k], n[k]) }
        return o
    }

    /// The ranges' tones: where the shadows meet the midtones and half
    /// the width of that change, where the midtones meet the highlights
    /// and half its width -- the core's grade_tones.
    var tones: (s: Double, sReach: Double, u: Double, uReach: Double) {
        let c = 0.5 + ColorGradeKernel.balanceReach * min(1, max(-1, balance))
        let w = min(1, max(0, width)), b = min(1, max(0, blend))
        var s = c * (1 - w), u = c + (1 - c) * w
        var hs = b * min(s, 1 - s), hu = b * min(u, 1 - u)
        if s <= 1e-9 { (s, hs) = (-1, 0) }
        if u >= 1 - 1e-9 { (u, hu) = (2, 0) }
        return (s, hs, u, hu)
    }

    /// A tone's (0...1) weights, as the kernel weighs a pixel.
    func weights(_ tone: Double) -> GradeWeights {
        let k = tones, t = min(1, max(0, tone))
        func ramp(_ at: Double, _ reach: Double) -> Double {
            if reach <= 1e-5 { return t < at ? 0 : 1 }
            let x = min(1, max(0, (t - (at - reach)) / (2 * reach)))
            return x * x * (3 - 2 * x)
        }
        var s = 1 - ramp(k.s, k.sReach), h = ramp(k.u, k.uReach)
        if s + h > 1 {
            let sum = s + h
            s /= sum
            h /= sum
        }
        return GradeWeights(shadows: s, midtones: max(0, 1 - s - h),
                            highlights: h)
    }
}

// MARK: - The kernel

/// The grade's Core Image colour kernel: the core's Metal source compiled
/// once, handed the step's gains and tones. Read from any thread: a
/// clip's frames are drawn on AVFoundation's.
enum ColorGradeKernel {
    private final class Box: @unchecked Sendable {
        let lock = NSLock()
        var kernel: CIKernel?
        var filter = "ValtzColorGrade"
        var balanceReach = 0.4
    }
    private static let box = Box()

    /// The core's description (CoreService.colorGrade), taken once.
    static func install(_ info: CoreReply) {
        guard info.ok, let source = info["kernel"] as? String,
              let name = info["kernel_name"] as? String else { return }
        let kernel = (try? CIKernel.kernels(withMetalString: source))?
            .first { $0.name == name }
        box.lock.withLock {
            box.kernel = kernel
            if let f = info["filter"] as? String { box.filter = f }
            if let b = info["balance_reach"] as? Double { box.balanceReach = b }
        }
    }

    /// Its step's name in a chain (media::kColorGradeFilter).
    static var filter: String { box.lock.withLock { box.filter } }
    static var balanceReach: Double { box.lock.withLock { box.balanceReach } }

    /// `image` graded with the step's numbers; as it was where the kernel
    /// is not here.
    nonisolated static func apply(_ image: CIImage,
                                  _ params: [String: [Double]]) -> CIImage {
        guard let kernel = box.lock.withLock({ box.kernel }) else {
            return image
        }
        func vector(_ name: String, _ n: Int) -> CIVector {
            let v = params[name] ?? []
            let c = (0..<n).map { i -> CGFloat in
                i < v.count ? CGFloat(v[i]) : (n == 3 ? 1 : 0)
            }
            return CIVector(values: c, count: n)
        }
        let args: [Any] = [image, vector("inputShadows", 3),
                           vector("inputMidtones", 3),
                           vector("inputHighlights", 3),
                           vector("inputTones", 4)]
        if let k = kernel as? CIColorKernel {
            return k.apply(extent: image.extent, arguments: args) ?? image
        }
        return kernel.apply(extent: image.extent,
                            roiCallback: { _, r in r },
                            arguments: args) ?? image
    }
}

// MARK: - The model

extension AppModel {
    /// The grade, whole: on a still, as it is; on a clip, at the frame on
    /// screen (Keyframes.edit) -- as Adjust's.
    func setGrade(_ g: ColorGrade) {
        guard adjustments.grade != g else { return }
        guard keyedStage else {
            adjustments.grade = g
            return
        }
        guard layerOffStage == nil else { return }
        clipAdjustKeys.edit(at: keyFrame) { $0.grade = g }
        clipTracksChanged()
    }

    /// One range's tint, from its wheel.
    func setGradeTint(_ r: GradeRange, _ t: GradeTint) {
        guard adjustments.grade[r] != t else { return }
        guard keyedStage else {
            adjustments.grade[r] = t
            return
        }
        guard layerOffStage == nil else { return }
        clipAdjustKeys.edit(at: keyFrame) { $0.grade[r] = t }
        clipTracksChanged()
    }

    /// One of the ranges' numbers, from its slider.
    func setGradeNumber(_ n: ColorGrade.Number, _ v: Double) {
        guard adjustments.grade[n] != v else { return }
        guard keyedStage else {
            adjustments.grade[n] = v
            return
        }
        guard layerOffStage == nil else { return }
        clipAdjustKeys.edit(at: keyFrame) { $0.grade[n] = v }
        clipTracksChanged()
    }

    /// Reset: the grade at rest -- `range`'s tint, or all of it; on a
    /// clip, in every key -- the Adjust and Color panels' values kept.
    func resetColorGrade(_ range: GradeRange? = nil) {
        let rest = { (a: ImageAdjustments) -> ImageAdjustments in
            var o = a
            if let range {
                o.grade[range] = GradeTint()
            } else {
                o.grade = ColorGrade()
            }
            return o
        }
        if keyedStage {
            clipAdjustKeys = clipAdjustKeys.collapsingValues(rest)
            clipTracksChanged()
            return
        }
        adjustments = rest(adjustments)
    }

    /// The grade is not at rest (Reset has work).
    var gradeChanged: Bool {
        keyedStage
            ? clipAdjustKeys.keys.contains { !$0.value.grade.isEmpty }
            : !adjustments.grade.isEmpty
    }

    /// The grade tints something to leave out.
    var canBypassGrade: Bool {
        guard layerOffStage == nil else { return false }
        return keyedStage
            ? clipAdjustKeys.keys.contains { !$0.value.grade.isIdentity }
            : !adjustments.grade.isIdentity
    }
}

// MARK: - The panel

struct GradePanel: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                if model.keyedStage {
                    // The look's keys: the Adjust panel's too.
                    KeyframeBar(model: model, track: .adjust)
                } else {
                    Text("Tint the shadows, midtones and highlights: click a wheel for the hue and how strong it is.")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                GradeBypassButton(model: model)
                Button("Reset") { model.resetColorGrade() }
                    .disabled(!model.gradeChanged
                              || (model.keyedStage
                                  && model.layerOffStage != nil)
                              || model.selectedLayerLocked)
            }
            VStack(alignment: .leading, spacing: 12) {
                HStack(alignment: .top, spacing: 12) {
                    ForEach(GradeRange.allCases) { r in
                        GradeWheelColumn(model: model, range: r)
                    }
                }
                HStack(alignment: .center, spacing: 18) {
                    Grid(alignment: .leading, horizontalSpacing: 10,
                         verticalSpacing: 8) {
                        ForEach(ColorGrade.Number.allCases) { row($0) }
                    }
                    GradeRangesGraph(grade: model.adjustments.grade)
                        .frame(minWidth: 110, maxWidth: .infinity)
                        .frame(height: 64)
                }
            }
            // Greyed while the playhead is off the selected layer -- its
            // values would change unseen -- or it is locked.
            .disabled(!model.canEditLayerHere)
        }
        .controlSize(.small)
    }

    private func row(_ n: ColorGrade.Number) -> some View {
        GridRow {
            Text(n.label)
                .foregroundStyle(.secondary)
                .gridColumnAlignment(.trailing)
                .onTapGesture(count: 2) { model.setGradeNumber(n, n.rest) }
                .help(n.help)
            CenteredSlider(value: Binding(
                               get: { model.adjustments.grade[n] },
                               set: { model.setGradeNumber(n, $0) }),
                           range: n.range, zero: n.rest)
                .frame(minWidth: 110)
                .help(n.help)
                .accessibilityLabel(Text(n.label))
            Text(verbatim: n.display(model.adjustments.grade[n]))
                .font(.callout.monospacedDigit())
                .foregroundStyle(.secondary)
                .frame(width: 34, alignment: .trailing)
        }
    }

    /// A range's colour as the panel draws it: its tint's hue, its own
    /// grey while untinted.
    static func colour(_ r: GradeRange, _ t: GradeTint) -> Color {
        guard t.saturation > 0 else {
            switch r {
            case .shadows: return Color(white: 0.32)
            case .midtones: return Color(white: 0.55)
            case .highlights: return Color(white: 0.8)
            }
        }
        return Color(hue: t.hue / 360, saturation: 0.35 + 0.6 * t.saturation,
                     brightness: 0.9)
    }
}

/// A wheel with its range's name over it and its tint in words under
/// it (beside the name, a narrow tray cut them short).
private struct GradeWheelColumn: View {
    @Bindable var model: AppModel
    let range: GradeRange
    @Environment(\.isEnabled) private var enabled

    var body: some View {
        let t = model.adjustments.grade[range]
        VStack(spacing: 4) {
            Text(verbatim: range.label)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            GradeWheel(range: range, tint: t, enabled: enabled,
                       onChange: { model.setGradeTint(range, $0) },
                       onReset: { model.resetColorGrade(range) })
                .frame(width: 132, height: 132)
                .accessibilityLabel(Text(verbatim: range.label))
            Text(verbatim: t.saturation > 0
                 ? "\(MixBand.nearest(t.hue).label) \(Int(t.hue.rounded()) % 360)° · \(Int((t.saturation * 100).rounded()))"
                 : "—")
                .font(.caption.monospacedDigit())
                .foregroundStyle(.secondary)
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity)
        .opacity(enabled ? 1 : 0.55)
    }
}

/// Where the ranges lie across the tones, dark to light: each range's
/// weight, in its tint's colour.
private struct GradeRangesGraph: View {
    let grade: ColorGrade

    var body: some View {
        Canvas { ctx, size in
            let bar: CGFloat = 6
            let h = size.height - bar - 3
            ctx.fill(Path(roundedRect: CGRect(x: 0, y: size.height - bar,
                                              width: size.width,
                                              height: bar),
                          cornerRadius: 2),
                     with: .linearGradient(
                        Gradient(colors: [.black, .white]),
                        startPoint: CGPoint(x: 0, y: 0),
                        endPoint: CGPoint(x: size.width, y: 0)))
            let n = 120
            let samples = (0...n).map { grade.weights(Double($0) / Double(n)) }
            for r in GradeRange.allCases {
                var line = Path()
                for (i, w) in samples.enumerated() {
                    let p = CGPoint(x: size.width * CGFloat(i) / CGFloat(n),
                                    y: h - h * CGFloat(w[r]))
                    if i == 0 { line.move(to: p) } else { line.addLine(to: p) }
                }
                var area = line
                area.addLine(to: CGPoint(x: size.width, y: h))
                area.addLine(to: CGPoint(x: 0, y: h))
                area.closeSubpath()
                let c = GradePanel.colour(r, grade[r])
                ctx.fill(area, with: .color(c.opacity(0.3)))
                ctx.stroke(line, with: .color(c), lineWidth: 1.5)
            }
        }
        .help("Where the shadows, midtones and highlights lie across the tones, dark to light")
        .accessibilityHidden(true)
    }
}

/// Bypass for the grade: held, the stage without it.
private struct GradeBypassButton: View {
    @Bindable var model: AppModel
    @State private var held = false

    var body: some View {
        Text("Bypass")
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .foregroundStyle(held ? Color.white : Color.primary)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous)
                .fill(held ? AnyShapeStyle(Color.accentColor)
                           : AnyShapeStyle(.quaternary)))
            .opacity(model.canBypassGrade ? 1 : 0.45)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    guard !held, model.canBypassGrade else { return }
                    held = true
                    model.setGradeBypass(true)
                }
                .onEnded { _ in
                    held = false
                    model.setGradeBypass(false)
                })
            .onDisappear {
                if held { model.setGradeBypass(false) }
            }
            .help("Hold to see the picture without its colour grade")
            .accessibilityAddTraits(.isButton)
    }
}

// MARK: - A wheel

/// One of the Grade panel's wheels, an AppKit view (its own mouse
/// handling: a click, a drag, a double-click, a right-click's menu).
struct GradeWheel: NSViewRepresentable {
    let range: GradeRange
    let tint: GradeTint
    let enabled: Bool
    let onChange: (GradeTint) -> Void
    let onReset: () -> Void

    func makeNSView(context: Context) -> GradeWheelNSView {
        let v = GradeWheelNSView(range: range)
        update(v)
        return v
    }

    func updateNSView(_ v: GradeWheelNSView, context: Context) { update(v) }

    private func update(_ v: GradeWheelNSView) {
        v.onChange = onChange
        v.onReset = onReset
        v.show(tint, enabled: enabled)
    }
}

/// A wheel: the hues round its edge -- red at the right, then yellow,
/// green, aqua, blue, magenta counter-clockwise -- the saturation out
/// from its centre, none there. A click or a drag puts the tint under the
/// pointer (held to the disk; ⇧ keeps its hue, the drag its strength
/// alone), a double-click takes it off.
final class GradeWheelNSView: NSView {
    /// Points: the disk this far in from the view's edges, and how far
    /// past its edge a press still sets the tint.
    private static let inset: CGFloat = 3
    private static let reach: CGFloat = 8

    let range: GradeRange
    var onChange: ((GradeTint) -> Void)?
    var onReset: (() -> Void)?
    private(set) var tint = GradeTint()
    private var enabled = true
    private var dragging = false
    /// ⇧ held through a drag: the hue kept.
    private var keptHue: Double?

    /// The wheels shown, by range: the snapshot hooks post their events
    /// to them.
    private static let shown = NSMapTable<NSString, GradeWheelNSView>
        .strongToWeakObjects()
    static func view(_ r: GradeRange) -> GradeWheelNSView? {
        shown.object(forKey: r.rawValue as NSString)
    }

    init(range: GradeRange) {
        self.range = range
        super.init(frame: .zero)
        toolTip = String(localized: "Click or drag for the tint: its hue round the wheel, its strength out from the centre. ⇧ keeps the hue; double-click for none.")
        setAccessibilityElement(true)
        setAccessibilityRole(.group)
    }

    required init?(coder: NSCoder) { fatalError("init(coder:) is unused") }

    override var isFlipped: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        if window != nil {
            Self.shown.setObject(self, forKey: range.rawValue as NSString)
        }
    }

    /// The model's tint -- not while dragged: the drag's own is shown
    /// until it is let go.
    func show(_ t: GradeTint, enabled e: Bool) {
        let changed = (!dragging && t != tint) || e != enabled
        if !dragging { tint = t }
        enabled = e
        setAccessibilityValue(tint.saturation > 0
            ? "\(Int(tint.hue.rounded()) % 360)° · \(Int((tint.saturation * 100).rounded()))"
            : String(localized: "None"))
        if changed { needsDisplay = true }
    }

    // MARK: Where things are

    private var centre: CGPoint { CGPoint(x: bounds.midX, y: bounds.midY) }
    private var radius: CGFloat {
        max(1, min(bounds.width, bounds.height) / 2 - Self.inset)
    }

    private func place(_ t: GradeTint) -> CGPoint {
        CGPoint(x: centre.x + radius * CGFloat(t.x),
                y: centre.y - radius * CGFloat(t.y))
    }

    private func tint(at p: CGPoint) -> GradeTint {
        let x = Double((p.x - centre.x) / radius)
        let y = Double((centre.y - p.y) / radius)
        guard let h = keptHue else { return GradeTint(x: x, y: y) }
        // Along the hue's own line out from the centre.
        let a = h * .pi / 180
        return GradeTint(hue: h,
                         saturation: max(0, x * cos(a) + y * sin(a)))
    }

    // MARK: Drawing

    override func draw(_ dirtyRect: NSRect) {
        guard let g = NSGraphicsContext.current?.cgContext else { return }
        let c = centre, r = radius
        let disk = CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r)
        g.saveGState()
        g.addEllipse(in: disk)
        g.clip()
        if let art = GradeWheelArt.image {
            // Its first row at the top: the view is flipped.
            g.saveGState()
            g.translateBy(x: 0, y: disk.maxY + disk.minY)
            g.scaleBy(x: 1, y: -1)
            g.interpolationQuality = .high
            g.draw(art, in: disk)
            g.restoreGState()
        }
        // A shade over it, so the marks read on every hue.
        g.setFillColor(NSColor.black.withAlphaComponent(0.12).cgColor)
        g.fill(disk)
        g.setStrokeColor(NSColor.white.withAlphaComponent(0.4).cgColor)
        g.setLineWidth(1)
        // Half strength, and the centre's cross.
        g.setLineDash(phase: 0, lengths: [2, 3])
        g.strokeEllipse(in: disk.insetBy(dx: r / 2, dy: r / 2))
        g.setLineDash(phase: 0, lengths: [])
        g.strokeLineSegments(between: [
            CGPoint(x: c.x - 5, y: c.y), CGPoint(x: c.x + 5, y: c.y),
            CGPoint(x: c.x, y: c.y - 5), CGPoint(x: c.x, y: c.y + 5),
        ])
        g.restoreGState()

        // The tint: a line out to it, its puck.
        let at = place(tint)
        if tint.saturation > 0 {
            g.setStrokeColor(NSColor.white.withAlphaComponent(0.85).cgColor)
            g.setLineWidth(1.2)
            g.strokeLineSegments(between: [c, at])
        }
        let puck = CGRect(x: at.x - 5.5, y: at.y - 5.5, width: 11, height: 11)
        g.setFillColor((dragging ? NSColor.controlAccentColor : .white)
            .cgColor)
        g.fillEllipse(in: puck)
        g.setStrokeColor(NSColor.black.withAlphaComponent(0.6).cgColor)
        g.setLineWidth(1)
        g.strokeEllipse(in: puck)

        g.setStrokeColor(NSColor.separatorColor.cgColor)
        g.strokeEllipse(in: disk.insetBy(dx: 0.5, dy: 0.5))
    }

    // MARK: The pointer

    override func resetCursorRects() {
        addCursorRect(bounds, cursor: enabled ? .crosshair : .arrow)
    }

    override func mouseDown(with e: NSEvent) {
        guard enabled else { return }
        let p = convert(e.locationInWindow, from: nil)
        guard hypot(p.x - centre.x, p.y - centre.y) <= radius + Self.reach
        else { return }
        if MarkupPointer.clicks(e) >= 2 {
            // A double-click: no tint.
            set(GradeTint())
            return
        }
        dragging = true
        keptHue = e.modifierFlags.contains(.shift) && tint.saturation > 0
            ? tint.hue : nil
        set(tint(at: p))
        needsDisplay = true
    }

    override func mouseDragged(with e: NSEvent) {
        guard dragging else { return }
        // ⇧ taken at each step.
        if e.modifierFlags.contains(.shift) {
            if keptHue == nil, tint.saturation > 0 { keptHue = tint.hue }
        } else {
            keptHue = nil
        }
        set(tint(at: convert(e.locationInWindow, from: nil)))
    }

    override func mouseUp(with e: NSEvent) {
        guard dragging else { return }
        dragging = false
        keptHue = nil
        onChange?(tint)
        needsDisplay = true
    }

    override func menu(for event: NSEvent) -> NSMenu? {
        guard enabled else { return nil }
        let m = NSMenu()
        let reset = NSMenuItem(
            title: String(localized: "Reset \(range.label)"),
            action: tint.saturation > 0 ? #selector(resetChosen) : nil,
            keyEquivalent: "")
        reset.target = self
        m.addItem(reset)
        return m
    }

    @objc private func resetChosen() { onReset?() }

    private func set(_ t: GradeTint) {
        guard t != tint else { return }
        tint = t
        onChange?(t)
        needsDisplay = true
    }
}

/// The wheels' ground: each hue round the edge, at full saturation there,
/// none at the centre -- made once, drawn scaled.
enum GradeWheelArt {
    @MainActor private static var made: CGImage?

    @MainActor
    static var image: CGImage? {
        if let made { return made }
        let n = 192
        var px = [UInt8](repeating: 0, count: n * n * 4)
        let v = 0.9
        for j in 0..<n {
            for i in 0..<n {
                let x = (Double(i) + 0.5) / Double(n) * 2 - 1
                // Its first row at the top: y up from the centre.
                let y = 1 - (Double(j) + 0.5) / Double(n) * 2
                let r = hypot(x, y)
                // Softened over its last pixel.
                let alpha = min(1, max(0, (1 - r) * Double(n) / 2 + 0.5))
                guard alpha > 0 else { continue }
                let s = min(1, r)
                let h = MixCurve.wrap(atan2(y, x) * 180 / .pi) / 60
                let f = { (k: Double) -> Double in
                    let m = (k + h).truncatingRemainder(dividingBy: 6)
                    return v - v * s * max(0, min(m, 4 - m, 1))
                }
                let o = (j * n + i) * 4
                px[o] = UInt8((f(5) * alpha * 255).rounded())
                px[o + 1] = UInt8((f(3) * alpha * 255).rounded())
                px[o + 2] = UInt8((f(1) * alpha * 255).rounded())
                px[o + 3] = UInt8((alpha * 255).rounded())
            }
        }
        guard let space = CGColorSpace(name: CGColorSpace.sRGB),
              let provider = CGDataProvider(data: Data(px) as CFData),
              let img = CGImage(
                width: n, height: n, bitsPerComponent: 8, bitsPerPixel: 32,
                bytesPerRow: n * 4, space: space,
                bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo
                    .premultipliedLast.rawValue),
                provider: provider, decode: nil, shouldInterpolate: true,
                intent: .defaultIntent) else { return nil }
        made = img
        return img
    }
}
