import Foundation

#if VALTZ_SPARKLE
import Sparkle
#endif

/// Auto-update, through Sparkle when it was built in (VALTZ_SPARKLE_DIR,
/// app/macos/fetch-sparkle.sh). Both sides of the #if offer the same
/// small surface -- whether it can check, a check, the automatic checks
/// -- so nothing else needs a conditional: a build without Sparkle hides
/// Check for Updates… and the Updates section.
///
/// The feed and the key that updates must be signed with are in
/// Info.plist (SUFeedURL, SUPublicEDKey). Sparkle asks the person, on
/// the second launch, before it checks by itself; until then, and when
/// they say no, it sends nothing.
@MainActor @Observable
final class Updater {
#if VALTZ_SPARKLE
    @ObservationIgnored private let controller: SPUStandardUpdaterController
    /// Mirrors Sparkle's setting, so the toggle redraws when it changes.
    private(set) var checksAutomatically: Bool

    init() {
        // A scripted snapshot run is a test of the window, not a launch:
        // no scheduled check, no permission prompt over the window.
        let scripted = ProcessInfo.processInfo
            .environment["VALTZ_SNAPSHOT"] != nil
        controller = SPUStandardUpdaterController(
            startingUpdater: !scripted, updaterDelegate: nil,
            userDriverDelegate: nil)
        checksAutomatically =
            controller.updater.automaticallyChecksForUpdates
    }

    var canCheck: Bool { true }

    func checkForUpdates() { controller.checkForUpdates(nil) }

    func setChecksAutomatically(_ on: Bool) {
        controller.updater.automaticallyChecksForUpdates = on
        checksAutomatically = on
    }
#else
    init() {}
    var canCheck: Bool { false }
    var checksAutomatically: Bool { false }
    func checkForUpdates() {}
    func setChecksAutomatically(_ on: Bool) { _ = on }
#endif

    /// "0.1.0 (412)": the version, and the build the updates count by.
    var version: String {
        let info = Bundle.main.infoDictionary ?? [:]
        let short = info["CFBundleShortVersionString"] as? String ?? "?"
        let build = info["CFBundleVersion"] as? String ?? "?"
        return "\(short) (\(build))"
    }
}
