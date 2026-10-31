import Foundation

/// The generation card's rows, by a stable id each: the label a row is
/// set under, and what the memory card's suggestion rings
/// (AppModel.memorySuggests). The title bar's search that found them by
/// their words is gone; the sidebar's search filters Assets.
enum SettingsRow {
    static let model = "model"
    static let orientation = "orientation", aspect = "aspect"
    static let size = "size", favor = "favor", seed = "seed"
    static let length = "length"
    static let songPlan = "song-plan", songLength = "song-length"
    static let lyrics = "lyrics", soundtrack = "soundtrack"
    static let voice = "voice", speechLength = "speech-length"
}
