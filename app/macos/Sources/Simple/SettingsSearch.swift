import Foundation

/// A row of simple mode's composer cards, as the title bar's search sees
/// it: the words on the row -- its label and every option it offers, in
/// the UI's language -- so "16:9", "quality" or "tint" finds the setting
/// even while its card is closed.
struct SettingsRow: Identifiable, Sendable {
    /// Stable ids the cards mark their rows with.
    static let modality = "modality", model = "model"
    static let orientation = "orientation", aspect = "aspect"
    static let size = "size", favor = "favor", seed = "seed"
    static let length = "length"
    static let songPlan = "song-plan", songLength = "song-length"
    static let lyrics = "lyrics", soundtrack = "soundtrack"
    static let voice = "voice", speechLength = "speech-length"
    static let cropZoom = "crop-zoom", cropPad = "crop-pad"
    static let cropRotate = "crop-rotate"
    static let trimNow = "trim-now", markIn = "mark-in", markOut = "mark-out"
    static let trimStart = "trim-start", speed = "speed"
    static let pitch = "pitch", volume = "volume"

    let id: String
    /// The card it is on; none for what is always in view (the modality
    /// tab above the prompt).
    let panel: ComposerPanel?
    let terms: [String]

    /// The rows whose words contain `query` (case- and
    /// diacritic-insensitive, like Finder), generation card first.
    @MainActor
    static func matching(_ query: String, in model: AppModel) -> [SettingsRow] {
        all(model).filter { row in
            row.terms.contains { $0.localizedStandardContains(query) }
        }
    }

    @MainActor
    private static func all(_ m: AppModel) -> [SettingsRow] {
        let d = m.dimensions
        let ratios = AspectRatio.allCases.flatMap {
            [$0.rawValue, $0.label(.portrait)]
        }
        // Speech: its voice and its length.
        if m.speaks {
            return [
                SettingsRow(id: modality, panel: nil,
                            terms: [String(localized: "Create")]
                                + Modality.allCases.map(\.label)),
                SettingsRow(id: model, panel: .generate,
                            terms: [String(localized: "Model"),
                                    String(localized: "Auto")]
                                + m.modelOptions(for: .audio).map(\.name)),
                SettingsRow(id: voice, panel: .generate,
                            terms: [String(localized: "Voice"),
                                    String(localized: "Clone"),
                                    m.speechVoiceText]),
                SettingsRow(id: speechLength, panel: .generate,
                            terms: [String(localized: "Length"),
                                    String(localized: "Duration"),
                                    String(localized: "Auto")]),
                SettingsRow(id: favor, panel: .generate,
                            terms: [String(localized: "Favor")]
                                + Preference.allCases.flatMap {
                                    [$0.label, $0.searchName, $0.hint]
                                }
                                + [m.preferenceHint]),
                SettingsRow(id: seed, panel: .generate,
                            terms: [String(localized: "Seed"),
                                    String(localized: "Random")]),
            ]
        }
        // A song has no shape or size: its own rows instead.
        if m.activeModality == .audio {
            return [
                SettingsRow(id: modality, panel: nil,
                            terms: [String(localized: "Create")]
                                + Modality.allCases.map(\.label)),
                SettingsRow(id: model, panel: .generate,
                            terms: [String(localized: "Model"),
                                    String(localized: "Auto")]
                                + m.modelOptions(for: .audio).map(\.name)),
                SettingsRow(id: songPlan, panel: .generate,
                            terms: [String(localized: "Score"),
                                    String(localized: "Chords")]
                                + SongPlan.allCases.flatMap {
                                    [$0.label, $0.hint]
                                }),
                SettingsRow(id: songLength, panel: .generate,
                            terms: [String(localized: "Length"),
                                    String(localized: "Duration"),
                                    String(localized: "Auto"),
                                    m.songLengthText]
                                + AppModel.songLengths.map {
                                    AppModel.songTime(Double($0))
                                }),
                SettingsRow(id: lyrics, panel: .generate,
                            terms: [String(localized: "Lyrics"),
                                    String(localized: "Words"),
                                    m.songLyricsText]),
                SettingsRow(id: favor, panel: .generate,
                            terms: [String(localized: "Favor")]
                                + Preference.allCases.flatMap {
                                    [$0.label, $0.searchName, $0.hint]
                                }
                                + [m.preferenceHint]),
                SettingsRow(id: seed, panel: .generate,
                            terms: [String(localized: "Seed"),
                                    String(localized: "Random")]),
            ] + (m.showsTrim ? [
                SettingsRow(id: trimNow, panel: .trim,
                            terms: [String(localized: "Trim"),
                                    String(localized: "Now")]),
                SettingsRow(id: markIn, panel: .trim,
                            terms: [String(localized: "Mark In")]),
                SettingsRow(id: markOut, panel: .trim,
                            terms: [String(localized: "Mark Out")]),
                SettingsRow(id: speed, panel: .trim,
                            terms: [String(localized: "Speed"),
                                    String(localized: "Playback speed")]),
                SettingsRow(id: pitch, panel: .trim,
                            terms: [String(localized: "Pitch"),
                                    String(localized: "Follow Speed"),
                                    String(localized: "Hold Pitch")]),
                SettingsRow(id: volume, panel: .trim,
                            terms: [String(localized: "Volume"),
                                    String(localized: "Mute")]),
                SettingsRow(id: trimStart, panel: .trim,
                            terms: [String(localized: "Starts on the timeline at"),
                                    String(localized: "Offset")]),
            ] : [])
        }
        var rows = [
            SettingsRow(id: modality, panel: nil,
                        terms: [String(localized: "Create")]
                            + Modality.allCases.map(\.label)),
            SettingsRow(id: model, panel: .generate,
                        terms: [String(localized: "Model"),
                                String(localized: "Auto")]
                            + Modality.allCases.flatMap {
                                m.modelOptions(for: $0).map(\.name)
                            }),
            // While the ratio row is hidden, its ratios find the row
            // that brings it back.
            SettingsRow(id: orientation, panel: .generate,
                        terms: [String(localized: "Orientation")]
                            + Orientation.allCases.map(\.label)
                            + (m.showsAspectRatio ? [] : ratios)),
            SettingsRow(id: size, panel: .generate,
                        terms: [String(localized: "Size"),
                                String(localized: "Custom"),
                                "\(d.width) × \(d.height)",
                                "\(d.width)x\(d.height)"]
                            + SizeClass.allCases.map(\.label)),
            SettingsRow(id: favor, panel: .generate,
                        terms: [String(localized: "Favor")]
                            + Preference.allCases.flatMap {
                                [$0.label, $0.searchName, $0.hint]
                            }
                            + [m.preferenceHint]),
            SettingsRow(id: seed, panel: .generate,
                        terms: [String(localized: "Seed"),
                                String(localized: "Random")]),
        ]
        // Shown only when there is a choice to make.
        if m.showsAspectRatio {
            rows.append(SettingsRow(
                id: aspect, panel: .generate,
                terms: [String(localized: "Aspect ratio")] + ratios))
        }
        // A clip's length, while the card makes clips.
        if m.activeModality == .video {
            rows.append(SettingsRow(
                id: length, panel: .generate,
                terms: [String(localized: "Length"),
                        String(localized: "Duration"),
                        String(localized: "Frames"), m.clipLengthText]
                    + AppModel.clipLengths.map {
                        String(localized: "\($0) s")
                    }))
        }
        // The Crop card's rows: a picture's crop, or a clip's trim.
        if m.showsTrim {
            rows += [
                SettingsRow(id: trimNow, panel: .trim,
                            terms: [String(localized: "Trim"),
                                    String(localized: "Now")]),
                SettingsRow(id: markIn, panel: .trim,
                            terms: [String(localized: "Mark In")]),
                SettingsRow(id: markOut, panel: .trim,
                            terms: [String(localized: "Mark Out")]),
                SettingsRow(id: speed, panel: .trim,
                            terms: [String(localized: "Speed"),
                                    String(localized: "Playback speed")]),
                SettingsRow(id: pitch, panel: .trim,
                            terms: [String(localized: "Pitch"),
                                    String(localized: "Follow Speed"),
                                    String(localized: "Hold Pitch")]),
                SettingsRow(id: volume, panel: .trim,
                            terms: [String(localized: "Volume"),
                                    String(localized: "Mute")]),
                SettingsRow(id: trimStart, panel: .trim,
                            terms: [String(localized: "Starts on the timeline at"),
                                    String(localized: "Offset")]),
            ]
        }
        if m.showsCrop {
            rows += [
                SettingsRow(id: cropZoom, panel: .crop,
                            terms: [String(localized: "Crop"),
                                    String(localized: "Zoom"),
                                    String(localized: "Offset")]),
                SettingsRow(id: cropPad, panel: .crop,
                            terms: [String(localized: "Padding")]),
                SettingsRow(id: cropRotate, panel: .crop,
                            terms: [String(localized: "Rotate")]),
            ]
        }
        // The Adjust card's rows, while there is a picture to adjust.
        if m.showsAdjust {
            rows += ImageAdjustments.Key.allCases.map { key in
                SettingsRow(id: key.rawValue, panel: .adjust,
                            terms: [key.label])
            }
        }
        return rows
    }
}
