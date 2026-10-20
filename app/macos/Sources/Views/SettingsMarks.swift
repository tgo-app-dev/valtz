import AppKit
import SwiftUI

/// SETTINGS SEARCH SHOWN ON THE PAGE, as System Settings shows it
/// (DESIGN §10a Settings). While words are typed in the sidebar's search,
/// every setting on the page whose name holds them is HIGHLIGHTED -- the
/// system's find highlight behind its row -- and the sidebar lists, under
/// each page it finds, the settings found there (SettingsResult); one
/// picked from them is scrolled to and pulses. A row says what it is
/// called with `.settingsMark(name)`, in the words the page shows it by:
/// the same words its page's keywords hold (SettingsPlace.keywords), which
/// the search reads without the page being drawn.

/// A setting picked from the sidebar's results: its page shown, the row
/// scrolled to and made to pulse. `serial` tells one pick from the same
/// picked again.
struct SettingsReveal: Equatable, Sendable {
    var name: String
    var serial: Int
}

extension EnvironmentValues {
    /// What the sidebar's search field holds ("": no search).
    @Entry var settingsQuery = ""
    /// The setting picked from the sidebar's results, if one was.
    @Entry var settingsReveal: SettingsReveal? = nil
}

extension View {
    /// A setting of a Settings page, by the name it shows: highlighted
    /// while the search finds it, scrolled to and pulsing when picked from
    /// the sidebar's results. More names than one: any of them found
    /// lights it. Its scroll target is told by where it is marked as
    /// well as its names: names alone collide -- two words of English that
    /// are one in Chinese ("Project", "Projects"), a label every family
    /// has ("TAE") -- and a Form or a List drops rows whose ids do. A
    /// mark made for each item of a list names its item: `scope`.
    func settingsMark(_ names: String..., scope: String = "",
                      file: StaticString = #fileID,
                      line: UInt = #line) -> some View {
        modifier(SettingsMark(
            names: names,
            anchor: "settings-mark:\(file):\(line):\(scope):"
                + names.joined(separator: "|")))
    }
}

enum SettingsSearchText {
    /// `name` holds `query` -- case, accents and width aside, as System
    /// Settings' search matches.
    static func matches(_ name: String, _ query: String) -> Bool {
        let q = query.trimmingCharacters(in: .whitespaces)
        return !q.isEmpty && name.localizedStandardContains(q)
    }

    /// Where `query` lies in `name`, as `matches` finds it.
    static func range(_ name: String, _ query: String) -> Range<String.Index>? {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return nil }
        return name.range(of: q, options: [.caseInsensitive,
                                           .diacriticInsensitive,
                                           .widthInsensitive],
                          locale: .current)
    }

    /// `name` with what `query` found in it set bold -- a sidebar result,
    /// as Spotlight shows one.
    static func emphasized(_ name: String, _ query: String) -> AttributedString {
        var s = AttributedString(name)
        if let r = range(name, query),
           let lo = AttributedString.Index(r.lowerBound, within: s),
           let hi = AttributedString.Index(r.upperBound, within: s) {
            s[lo..<hi].font = .system(size: 12, weight: .bold)
        }
        return s
    }
}

/// The settings marked on the page shown: each name's marks, by their
/// scroll targets, as they came on -- where a pick of a name scrolls, and
/// what a snapshot run says is lit.
@MainActor
final class SettingsMarksShown {
    static let shared = SettingsMarksShown()
    private var marks: [String: [String]] = [:]

    func add(_ names: [String], _ anchor: String) {
        for k in names { marks[k, default: []].append(anchor) }
    }

    func remove(_ names: [String], _ anchor: String) {
        for k in names {
            if let i = marks[k]?.firstIndex(of: anchor) {
                marks[k]?.remove(at: i)
            }
            if marks[k]?.isEmpty ?? false { marks[k] = nil }
        }
    }

    /// Where a pick of `name` scrolls: the first of its marks to come on
    /// (a Form lays its rows out ahead of the scroll), none if none has.
    func anchor(_ name: String) -> String? { marks[name]?.first }

    /// The names shown that `query` lights, sorted.
    func lit(_ query: String) -> [String] {
        marks.keys.filter { SettingsSearchText.matches($0, query) }.sorted()
    }
}

private struct SettingsMark: ViewModifier {
    let names: [String]
    let anchor: String
    @Environment(\.settingsQuery) private var query
    @Environment(\.settingsReveal) private var reveal
    @Environment(\.colorScheme) private var scheme
    /// Pulsing, after a pick.
    @State private var pulse = false

    private var lit: Bool {
        names.contains { SettingsSearchText.matches($0, query) }
    }

    private var picked: Bool {
        guard let r = reveal else { return false }
        return names.contains(r.name)
    }

    func body(content: Content) -> some View {
        content
            .background {
                RoundedRectangle(cornerRadius: 6, style: .continuous)
                    .fill(Color(nsColor: .findHighlightColor)
                        .opacity(pulse ? 0.95
                                       : scheme == .dark ? 0.32 : 0.42))
                    .padding(.horizontal, -6)
                    .padding(.vertical, -3)
                    .opacity(lit || picked ? 1 : 0)
                    .animation(.easeOut(duration: 0.15), value: lit)
                    .allowsHitTesting(false)
            }
            .id(anchor)
            .onAppear {
                SettingsMarksShown.shared.add(names, anchor)
                if picked { flash() }
            }
            .onDisappear { SettingsMarksShown.shared.remove(names, anchor) }
            .onChange(of: reveal) { _, r in
                if let r, names.contains(r.name) { flash() }
            }
    }

    /// Three pulses, as a found setting draws the eye.
    private func flash() {
        pulse = false
        withAnimation(.easeInOut(duration: 0.28).repeatCount(5,
                                                              autoreverses: true)) {
            pulse = true
        }
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(1400))
            withAnimation(.easeOut(duration: 0.25)) { pulse = false }
        }
    }
}

/// A row the sidebar lists: a place -- or, while a search is typed, a
/// setting found on one, set in under its page.
struct SettingsResult: Hashable, Sendable {
    let place: SettingsPlace
    let setting: String?

    init(_ place: SettingsPlace, _ setting: String? = nil) {
        self.place = place
        self.setting = setting
    }

    /// Its name for the snapshot hooks: "fleet", "general>Language".
    var raw: String {
        setting.map { "\(place.raw)>\($0)" } ?? place.raw
    }
}

extension SettingsPlace {
    /// What the sidebar lists for a search: each place it finds (found),
    /// and under each the settings of its page the words are in -- in the
    /// order the page has them, each once, its own name left out.
    @MainActor
    static func results(_ query: String, _ model: AppModel)
        -> [SettingsResult] {
        var out: [SettingsResult] = []
        for p in found(query, model) {
            out.append(SettingsResult(p))
            var seen: Set<String> = [p.title]
            for k in p.keywords(model)
            where SettingsSearchText.matches(k, query) && !seen.contains(k) {
                seen.insert(k)
                out.append(SettingsResult(p, k))
            }
        }
        return out
    }
}
