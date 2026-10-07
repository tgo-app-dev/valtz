import Foundation

/// Localization, the app's side (DESIGN §10b).
///
/// Text in the app is localized the standard way: SwiftUI string
/// literals, and `String(localized:)` for text built in code, all looked
/// up in the String Catalog `Resources/Localizable.xcstrings`, which the
/// build compiles into the bundle's `.lproj` folders.
///
/// Text from the CORE arrives as a message key, named arguments and the
/// English text (core/include/valtz/base/message.h). `coreMessage` looks
/// the key up in the same catalog and fills in the arguments, so one
/// catalog holds every language; a key the catalog lacks falls back to
/// the core's English.
enum L10n {
    /// The language the UI is shown in (one of the bundle's
    /// localizations, as a BCP-47 tag), passed to the core so the
    /// engine's own messages follow it. Fixed for the process: macOS
    /// picks it at launch.
    static var uiLanguage: String {
        Bundle.main.preferredLocalizations.first ?? "en"
    }

    // MARK: Valtz's own language (Settings › General)
    //
    // Chosen the way macOS keeps a language for one app -- the app's own
    // AppleLanguages, which System Settings › Language & Region ›
    // Applications sets too -- and taken at the next launch.

    /// The languages Valtz is in, English first: "en", "zh-Hans" -- each
    /// once. `Bundle.localizations` lists a language twice when the
    /// Info.plist's CFBundleLocalizations names it and its .lproj is
    /// there too (["en", "zh-Hans", "zh-Hans", "en"]): the Language
    /// pop-up showed two of each, and choosing one checked both.
    static var available: [String] {
        let all = Set(Bundle.main.localizations).subtracting(["Base"])
        return all.sorted { a, b in
            a == "en" ? true : b == "en" ? false : a < b
        }
    }

    /// The language chosen for Valtz alone; nil follows the system's.
    static var chosen: String? {
        guard let id = Bundle.main.bundleIdentifier,
              let tags = UserDefaults.standard.persistentDomain(forName: id)?[
                  languagesKey] as? [String],
              let first = tags.first else { return nil }
        return Bundle.preferredLocalizations(from: available,
                                             forPreferences: [first]).first
    }

    /// Valtz in `tag` from its next launch -- or, nil, in the system's
    /// language. A scripted snapshot run changes nothing saved.
    static func choose(_ tag: String?) {
        guard ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT"] == nil
        else { return }
        if let tag {
            UserDefaults.standard.set([tag], forKey: languagesKey)
        } else {
            UserDefaults.standard.removeObject(forKey: languagesKey)
        }
    }

    /// What the system's languages make of Valtz: the language "System
    /// Default" means.
    static var systemDefault: String {
        let prefs = CFPreferencesCopyValue(
            languagesKey as CFString, kCFPreferencesAnyApplication,
            kCFPreferencesCurrentUser, kCFPreferencesAnyHost) as? [String]
        return Bundle.preferredLocalizations(
            from: available, forPreferences: prefs ?? ["en"]).first ?? "en"
    }

    /// A language's name in its own words: "English", "简体中文".
    static func name(_ tag: String) -> String {
        let l = Locale(identifier: tag)
        return l.localizedString(forIdentifier: tag)?.capitalized(with: l)
            ?? tag
    }

    private static let languagesKey = "AppleLanguages"

    /// A core message (a bridge reply or an event payload), localized.
    static func coreMessage(_ payload: [String: Any]) -> String {
        let english = payload["message"] as? String
            ?? String(localized: "Something went wrong.")
        guard let key = payload["key"] as? String else { return english }
        let missing = "\u{1}"
        var text = Bundle.main.localizedString(forKey: key, value: missing,
                                               table: nil)
        guard text != missing else { return english }
        for (name, value) in payload["args"] as? [String: Any] ?? [:] {
            text = text.replacingOccurrences(of: "{\(name)}", with: "\(value)")
        }
        return text
    }
}
