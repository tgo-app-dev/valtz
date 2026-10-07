import AppKit

/// How a movie export is ENCODED (core engine::VideoEncoding): what the
/// save panel's video rows set, kept between saves.
struct VideoEncodingSettings: Codable, Equatable, Sendable {
    enum Rate: String, Codable, Sendable { case auto, quality, bitrate }
    var rate = Rate.auto
    /// 1...100, for a rate of the encoder's own at that quality.
    var quality = 70.0
    /// Megabits per second: the average, and a peak (0 none).
    var bitrate = 20.0
    var peak = 0.0
    /// The most seconds from one keyframe to the next; 0 the encoder's.
    var keyframes = 0.0
    var bFrames = true
    /// H.264.
    var profile = "high"
    var level = ""
    var entropy = "cabac"
    /// HEVC: "main10" (keeps HDR) or "main" (8-bit).
    var hevcProfile = "main10"
    /// ProRes: 4444's and 422's flavour.
    var prores4444 = "4444"
    var prores422 = "422hq"

    /// The request's "video" for `format`, nil when nothing differs from
    /// the encoder's own.
    func request(for format: ExportChoice) -> [String: Any]? {
        var j: [String: Any] = [:]
        switch format {
        case .prores4444:
            if prores4444 != "4444" { j["prores"] = prores4444 }
        case .prores422hq:
            if prores422 != "422hq" { j["prores"] = prores422 }
        case .h264, .hevc10:
            switch rate {
            case .auto: break
            case .quality: j["quality"] = quality / 100
            case .bitrate:
                j["bitrate"] = Int64((bitrate * 1e6).rounded())
                if peak >= bitrate {
                    j["max_bitrate"] = Int64((peak * 1e6).rounded())
                }
            }
            if keyframes > 0 { j["keyframe_seconds"] = keyframes }
            let baseline = format == .h264 && profile == "baseline"
            if baseline || !bFrames { j["b_frames"] = false }
            if format == .h264 {
                j["profile"] = profile
                if !level.isEmpty { j["level"] = level }
                if !baseline { j["entropy"] = entropy }
            } else if hevcProfile == "main" {
                j["profile"] = "main"
            }
        default:
            return nil
        }
        return j.isEmpty ? nil : j
    }

    /// About how large the picture comes out at an average bitrate over
    /// `seconds`; nil at a rate of the encoder's own.
    func estimate(for format: ExportChoice, seconds: Double?) -> Int64? {
        guard format == .h264 || format == .hevc10, rate == .bitrate,
              let seconds, seconds > 0 else { return nil }
        return Int64(bitrate * 1e6 * seconds / 8)
    }
}

/// The save panel's rows for a movie's encoding, under its format: AppKit
/// controls (a SwiftUI picker in a save panel's accessory never opened):
///
///   Type:       ProRes 422 HQ ▾                      (ProRes)
///   Profile:    High ▾   Level: Automatic ▾           (H.264; HEVC: Main 10 ▾)
///   Entropy:    CABAC ▾                               (H.264 Main / High)
///   Rate:       Average bitrate ▾  [ 20 ] Mbps  Peak [  ] Mbps
///               about 1.2 GB
///   Keyframes:  Every 2 seconds ▾
///               [x] B-frames
@MainActor
final class VideoEncodingRows: NSObject, NSTextFieldDelegate {
    private(set) var settings: VideoEncodingSettings
    private var format: ExportChoice = .h264
    private let seconds: Double?
    /// Their rows in the panel's grid, shown as the format has them.
    private(set) var rows: [[NSView]] = []
    private var rowViews: [NSView] = []

    private let proresMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let profileMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let levelLabel = NSTextField(labelWithString:
        String(localized: "Level:"))
    private let levelMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let entropyMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let rateMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let bitrate = NSTextField()
    private let peak = NSTextField()
    private let mbps = NSTextField(labelWithString: String(localized: "Mbps"))
    private let peakLabel = NSTextField(labelWithString:
        String(localized: "Peak"))
    private let peakMbps = NSTextField(labelWithString:
        String(localized: "Mbps"))
    private let quality = NSSlider(value: 70, minValue: 1, maxValue: 100,
                                   target: nil, action: nil)
    private let qualityValue = NSTextField(labelWithString: "")
    private let keyMenu = NSPopUpButton(frame: .zero, pullsDown: false)
    private let bFrames = NSButton(checkboxWithTitle:
        String(localized: "B-frames"), target: nil, action: nil)
    private let sizeLabel = NSTextField(labelWithString: "")
    private let proresRow: NSView
    private let profileRow: NSView
    private let entropyRow: NSView
    private let rateRow: NSView
    private let keyRow: NSView
    private let bRow: NSView
    private let sizeRow: NSView

    private static let h264Profiles: [(String, String)] = [
        ("baseline", String(localized: "Baseline")),
        ("main", String(localized: "Main")),
        ("high", String(localized: "High")),
    ]
    private static let hevcProfiles: [(String, String)] = [
        ("main10", String(localized: "Main 10 (10-bit, keeps HDR)")),
        ("main", String(localized: "Main (8-bit)")),
    ]
    private static let levels = ["", "3.0", "3.1", "3.2", "4.0", "4.1",
                                 "4.2", "5.0", "5.1", "5.2"]
    private static let keyframeSeconds: [Double] = [0, 0.5, 1, 2, 4, 10]
    private static let prores4444: [(String, String)] = [
        ("4444", "ProRes 4444"), ("4444xq", "ProRes 4444 XQ"),
    ]
    private static let prores422: [(String, String)] = [
        ("422hq", "ProRes 422 HQ"), ("422", "ProRes 422"),
        ("422lt", "ProRes 422 LT"), ("422proxy", "ProRes 422 Proxy"),
    ]

    init(seconds: Double?) {
        self.seconds = seconds
        settings = Self.remembered ?? VideoEncodingSettings()
        func label(_ s: String) -> NSTextField {
            NSTextField(labelWithString: s)
        }
        func line(_ views: [NSView]) -> NSStackView {
            let s = NSStackView(views: views)
            s.orientation = .horizontal
            s.alignment = .centerY
            s.spacing = 6
            return s
        }
        for f in [bitrate, peak] {
            let n = NumberFormatter()
            n.numberStyle = .decimal
            n.minimum = 0
            n.maximum = 2000
            n.maximumFractionDigits = 2
            f.formatter = n
            f.alignment = .right
            f.widthAnchor.constraint(equalToConstant: 56).isActive = true
        }
        peak.placeholderString = String(localized: "none")
        for l in [mbps, peakLabel, peakMbps, sizeLabel, qualityValue] {
            l.textColor = .secondaryLabelColor
        }
        sizeLabel.font = .monospacedDigitSystemFont(
            ofSize: NSFont.smallSystemFontSize, weight: .regular)
        qualityValue.font = .monospacedDigitSystemFont(
            ofSize: NSFont.systemFontSize, weight: .regular)
        quality.widthAnchor.constraint(equalToConstant: 130).isActive = true
        proresRow = proresMenu
        profileRow = line([profileMenu, levelLabel, levelMenu])
        entropyRow = entropyMenu
        rateRow = line([rateMenu, bitrate, mbps, peakLabel, peak, peakMbps,
                        quality, qualityValue])
        sizeRow = sizeLabel
        keyRow = keyMenu
        bRow = bFrames
        super.init()
        rows = [
            [label(String(localized: "Type:")), proresRow],
            [label(String(localized: "Profile:")), profileRow],
            [label(String(localized: "Entropy:")), entropyRow],
            [label(String(localized: "Rate:")), rateRow],
            [NSView(), sizeRow],
            [label(String(localized: "Keyframes:")), keyRow],
            [NSView(), bRow],
        ]
        rowViews = [proresRow, profileRow, entropyRow, rateRow, sizeRow,
                    keyRow, bRow]

        levelMenu.addItem(withTitle: String(localized: "Automatic"))
        for l in Self.levels.dropFirst() { levelMenu.addItem(withTitle: l) }
        entropyMenu.addItems(withTitles: ["CABAC", "CAVLC"])
        rateMenu.addItems(withTitles: [
            String(localized: "Automatic"),
            String(localized: "Quality"),
            String(localized: "Average bitrate"),
        ])
        for s in Self.keyframeSeconds {
            keyMenu.addItem(withTitle: s == 0
                ? String(localized: "Automatic")
                : s < 1 ? String(localized: "Every half second")
                : s == 1 ? String(localized: "Every second")
                : String(localized: "Every \(String(Int(s))) seconds"))
        }
        entropyMenu.toolTip = String(localized: "CABAC makes smaller files; CAVLC decodes on the simplest players")
        bFrames.toolTip = String(localized: "Frames predicted from both sides: smaller files; some old players and editors need them off")
        peak.toolTip = String(localized: "The most it may spend in any one second -- empty for no cap")
        keyMenu.toolTip = String(localized: "The longest a player waits for a frame it can start from, seeking")
        for c in [proresMenu, profileMenu, levelMenu, entropyMenu, rateMenu,
                  keyMenu] as [NSPopUpButton] {
            c.target = self
            c.action = #selector(changed)
        }
        quality.target = self
        quality.action = #selector(changed)
        quality.isContinuous = true
        bFrames.target = self
        bFrames.action = #selector(changed)
        bitrate.delegate = self
        peak.delegate = self
    }

    /// The rows for `format`: shown, hidden, filled.
    func show(_ format: ExportChoice) {
        self.format = format
        let prores = format == .prores4444 || format == .prores422hq
        let coded = format == .h264 || format == .hevc10
        proresMenu.removeAllItems()
        let flavours = format == .prores4444 ? Self.prores4444
                                             : Self.prores422
        proresMenu.addItems(withTitles: flavours.map(\.1))
        proresMenu.selectItem(at: flavours.firstIndex {
            $0.0 == (format == .prores4444 ? settings.prores4444
                                           : settings.prores422)
        } ?? 0)
        profileMenu.removeAllItems()
        let profiles = format == .h264 ? Self.h264Profiles
                                       : Self.hevcProfiles
        profileMenu.addItems(withTitles: profiles.map(\.1))
        profileMenu.selectItem(at: profiles.firstIndex {
            $0.0 == (format == .h264 ? settings.profile
                                     : settings.hevcProfile)
        } ?? 0)
        levelMenu.selectItem(at: Self.levels.firstIndex(of: settings.level)
                                 ?? 0)
        entropyMenu.selectItem(at: settings.entropy == "cavlc" ? 1 : 0)
        rateMenu.selectItem(at: settings.rate == .quality ? 1
                                : settings.rate == .bitrate ? 2 : 0)
        bitrate.doubleValue = settings.bitrate
        peak.stringValue = settings.peak > 0
            ? String(format: "%g", settings.peak) : ""
        quality.doubleValue = settings.quality
        keyMenu.selectItem(at: Self.keyframeSeconds.firstIndex(
            of: settings.keyframes) ?? 0)
        bFrames.state = settings.bFrames ? .on : .off
        proresRow.isHidden = !prores
        profileRow.isHidden = !coded
        rateRow.isHidden = !coded
        keyRow.isHidden = !coded
        bRow.isHidden = !coded
        refresh()
    }

    /// Their grid rows hidden with them (a hidden view leaves its row).
    func hideRows(in grid: NSGridView) {
        for r in 0..<grid.numberOfRows {
            let row = grid.row(at: r)
            guard row.numberOfCells > 1,
                  let v = row.cell(at: 1).contentView,
                  rowViews.contains(v) else { continue }
            row.isHidden = v.isHidden
        }
    }

    var onChange: (() -> Void)?

    @objc private func changed() {
        read()
        refresh()
        onChange?()
    }

    func controlTextDidChange(_ obj: Notification) {
        read()
        refresh()
    }

    private func read() {
        let prores = format == .prores4444 ? Self.prores4444
                                           : Self.prores422
        let p = proresMenu.indexOfSelectedItem
        if format == .prores4444, prores.indices.contains(p) {
            settings.prores4444 = prores[p].0
        } else if format == .prores422hq, prores.indices.contains(p) {
            settings.prores422 = prores[p].0
        }
        let i = profileMenu.indexOfSelectedItem
        if format == .h264, Self.h264Profiles.indices.contains(i) {
            settings.profile = Self.h264Profiles[i].0
        } else if format == .hevc10, Self.hevcProfiles.indices.contains(i) {
            settings.hevcProfile = Self.hevcProfiles[i].0
        }
        let l = levelMenu.indexOfSelectedItem
        if Self.levels.indices.contains(l) { settings.level = Self.levels[l] }
        settings.entropy = entropyMenu.indexOfSelectedItem == 1
            ? "cavlc" : "cabac"
        settings.rate = rateMenu.indexOfSelectedItem == 1 ? .quality
            : rateMenu.indexOfSelectedItem == 2 ? .bitrate : .auto
        if bitrate.doubleValue > 0 { settings.bitrate = bitrate.doubleValue }
        settings.peak = peak.stringValue.isEmpty ? 0 : peak.doubleValue
        settings.quality = quality.doubleValue.rounded()
        let k = keyMenu.indexOfSelectedItem
        if Self.keyframeSeconds.indices.contains(k) {
            settings.keyframes = Self.keyframeSeconds[k]
        }
        settings.bFrames = bFrames.state == .on
    }

    /// What shows with what: the level with H.264, entropy and B-frames
    /// away from Baseline, the bitrate's fields or the quality's slider
    /// with their rate -- and about the file's size.
    private func refresh() {
        let h264 = format == .h264
        let baseline = h264 && settings.profile == "baseline"
        levelLabel.isHidden = !h264
        levelMenu.isHidden = !h264
        entropyRow.isHidden = !h264
        entropyMenu.isEnabled = !baseline
        if baseline { entropyMenu.selectItem(at: 1) }
        bFrames.isEnabled = !baseline
        if baseline { bFrames.state = .off }
        let byRate = settings.rate == .bitrate
        for v in [bitrate, mbps, peakLabel, peak, peakMbps] as [NSView] {
            v.isHidden = !byRate
        }
        quality.isHidden = settings.rate != .quality
        qualityValue.isHidden = settings.rate != .quality
        qualityValue.stringValue = "\(Int(settings.quality))%"
        if byRate && settings.peak > 0 && settings.peak < settings.bitrate {
            sizeLabel.stringValue = String(localized: "The peak is below the average: no peak")
        } else if let bytes = settings.estimate(for: format,
                                                seconds: seconds) {
            sizeLabel.stringValue = String(localized: "about \(bytes.formatted(ByteCountFormatStyle(style: .file)))")
        } else {
            sizeLabel.stringValue = ""
        }
        sizeLabel.isHidden = sizeLabel.stringValue.isEmpty
            || !(format == .h264 || format == .hevc10)
    }

    /// The request's "video".
    var request: [String: Any]? { settings.request(for: format) }

    // As the controls set them -- for a scripted snapshot run.
    func set(_ s: VideoEncodingSettings) {
        settings = s
        show(format)
    }

    func remember() {
        guard Self.keeps,
              let data = try? JSONEncoder().encode(settings) else { return }
        UserDefaults.standard.set(data, forKey: Self.key)
    }

    private static let key = "save.videoEncoding"
    private static var keeps: Bool {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
    }
    private static var remembered: VideoEncodingSettings? {
        guard keeps,
              let data = UserDefaults.standard.data(forKey: key) else {
            return nil
        }
        return try? JSONDecoder().decode(VideoEncodingSettings.self,
                                         from: data)
    }
}
