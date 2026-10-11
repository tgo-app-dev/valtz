import SwiftUI

/// Where Settings is: a page of its sidebar, or a page UNDER one -- System
/// Settings' second level, opened from a row of its page (Fleet's Offload
/// Policies and Shared Capabilities). A page under another keeps that
/// page selected in the sidebar.
enum SettingsPlace: Hashable, Sendable {
    case page(SettingsPage)
    case fleetOffload
    case fleetSharing

    /// The pages under pages, in their sidebar page's order.
    static let below: [SettingsPlace] = [.fleetOffload, .fleetSharing]

    /// The sidebar's page it is, or is under.
    var page: SettingsPage {
        switch self {
        case .page(let p): p
        case .fleetOffload, .fleetSharing: .fleet
        }
    }

    var title: String {
        switch self {
        case .page(let p): p.title
        case .fleetOffload: String(localized: "Offload Policies")
        case .fleetSharing: String(localized: "Shared Capabilities")
        }
    }

    /// Its name for the snapshot hooks ("fleet", "fleet-offload").
    var raw: String {
        switch self {
        case .page(let p): p.rawValue
        case .fleetOffload: "fleet-offload"
        case .fleetSharing: "fleet-sharing"
        }
    }

    init?(raw: String) {
        switch raw {
        case "fleet-offload": self = .fleetOffload
        case "fleet-sharing": self = .fleetSharing
        default:
            guard let p = SettingsPage(rawValue: raw) else { return nil }
            self = .page(p)
        }
    }

    /// Where Settings opens: General -- or, in a snapshot run, where
    /// VALTZ_SNAPSHOT_SETTINGS says.
    static var firstShown: SettingsPlace {
        ProcessInfo.processInfo.environment["VALTZ_SNAPSHOT_SETTINGS"]
            .flatMap(SettingsPlace.init(raw:)) ?? .page(.general)
    }

    /// What its page holds, as a search finds it: the names of its
    /// sections and settings, in the UI's language.
    @MainActor
    func keywords(_ model: AppModel) -> [String] {
        switch self {
        case .page(.general):
            [String(localized: "Language"), String(localized: "Updates"),
             String(localized: "Version"), String(localized: "This Mac"),
             String(localized: "Chip"), String(localized: "Memory"),
             String(localized: "GPU"), String(localized: "Neural Engine"),
             String(localized: "Engine"), String(localized: "Projects"),
             String(localized: "Models"), String(localized: "Cache"),
             String(localized: "Project")]
        case .page(.capabilities):
            [String(localized: "Models"), String(localized: "Download"),
             String(localized: "Link"), String(localized: "Quantize")]
                + (model.capabilityTree?.families.map(\.name) ?? [])
        case .page(.helper):
            [String(localized: "Helper"), String(localized: "Keep Loaded"),
             String(localized: "Sampling"),
             String(localized: "Multi-token prediction"),
             String(localized: "Thinking"),
             String(localized: "Video Summary"),
             String(localized: "Enhance"), String(localized: "Debugging"),
             String(localized: "Show Speed in the Agent's Output")]
        case .page(.experimental):
            [String(localized: "Agent"), String(localized: "Thinks On"),
             String(localized: "Cloud"), String(localized: "API Key"),
             String(localized: "Ask before each step")]
        case .page(.fleet):
            [String(localized: "Name"), String(localized: "Discoverable"),
             String(localized: "Secret"), String(localized: "Members"),
             String(localized: "Join a Fleet"),
             String(localized: "Create a Fleet"),
             String(localized: "Leave Fleet"),
             String(localized: "Take Fleet Jobs"),
             String(localized: "Only at Set Times")]
        case .page(.storage):
            [String(localized: "Internal SSD"), String(localized: "Models"),
             String(localized: "Projects"), String(localized: "Cache"),
             String(localized: "Size")]
        case .page(.permissions):
            [String(localized: "Camera"), String(localized: "Microphone"),
             String(localized: "Screen & System Audio Recording"),
             String(localized: "Files and Folders"),
             String(localized: "Local Network"),
             String(localized: "Incoming Connections")]
        case .fleetOffload:
            [String(localized: "Offload"), FleetOffload.never.title,
             FleetOffload.localFirst.title, FleetOffload.fleetFirst.title,
             FleetOffload.always.title]
                + FleetTaskLabel.all.map(\.title)
        case .fleetSharing:
            [String(localized: "Share"), String(localized: "Withhold"),
             String(localized: "Offered Now")]
                + FleetTaskLabel.all.map(\.title)
        }
    }

    /// `query` is in its name or what its page holds.
    @MainActor
    func matches(_ query: String, _ model: AppModel) -> Bool {
        let q = query.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return true }
        return ([title] + keywords(model)).contains {
            $0.localizedStandardContains(q)
        }
    }

    /// What the sidebar lists for a search: each page that matches, or
    /// holds a page that does, and under it the pages under it that
    /// match -- in the sidebar's order.
    @MainActor
    static func found(_ query: String, _ model: AppModel) -> [SettingsPlace] {
        var out: [SettingsPlace] = []
        for p in SettingsPage.allCases {
            let under = below.filter {
                $0.page == p && $0.matches(query, model)
            }
            if SettingsPlace.page(p).matches(query, model) || !under.isEmpty {
                out.append(.page(p))
                out += under
            }
        }
        return out
    }
}

/// Where Settings has been: ‹ goes back, › forward again, as System
/// Settings' arrows do; going somewhere new drops what lay ahead.
struct SettingsHistory: Equatable, Sendable {
    private(set) var places: [SettingsPlace]
    private(set) var at = 0

    init(_ first: SettingsPlace) { places = [first] }

    var current: SettingsPlace { places[at] }
    var canGoBack: Bool { at > 0 }
    var canGoForward: Bool { at < places.count - 1 }

    mutating func go(_ p: SettingsPlace) {
        guard p != current else { return }
        places.removeSubrange((at + 1)...)
        places.append(p)
        at = places.count - 1
    }

    mutating func back() {
        if canGoBack { at -= 1 }
    }

    mutating func forward() {
        if canGoForward { at += 1 }
    }
}

/// An offload policy: where this Mac's jobs of a task (or a model) go
/// while it is in a fleet (the core's fleet::Config::offload).
enum FleetOffload: String, CaseIterable, Identifiable {
    case never
    case localFirst = "local-first"
    case fleetFirst = "fleet-first"
    case always
    var id: String { rawValue }

    var title: String {
        switch self {
        case .never: String(localized: "Never")
        case .localFirst: String(localized: "Local First")
        case .fleetFirst: String(localized: "Fleet First")
        case .always: String(localized: "Always")
        }
    }

    /// What it does, in a sentence.
    var meaning: String {
        switch self {
        case .never:
            String(localized: "Runs here, never sent to a member.")
        case .localFirst:
            String(localized: "Runs here, unless this Mac cannot run it or is busy: then a free member that can.")
        case .fleetFirst:
            String(localized: "Runs on a free member that can, else here.")
        case .always:
            String(localized: "Runs on a member only, never here: refused while none can take it.")
        }
    }

    static func named(_ raw: String) -> FleetOffload {
        FleetOffload(rawValue: raw) ?? .localFirst
    }
}

/// A fleet task as the person reads it: an engine op's name and icon.
struct FleetTaskLabel {
    let op: String
    let title: String
    let symbol: String

    static let all: [FleetTaskLabel] = [
        .init(op: "generate-image", title: String(localized: "Pictures"),
              symbol: "photo"),
        .init(op: "edit-image", title: String(localized: "Picture Edits"),
              symbol: "wand.and.rays"),
        .init(op: "generate-video", title: String(localized: "Clips"),
              symbol: "film"),
        .init(op: "generate-audio", title: String(localized: "Songs"),
              symbol: "music.note"),
        .init(op: "generate-speech", title: String(localized: "Speech"),
              symbol: "waveform"),
        .init(op: "transcribe-audio",
              title: String(localized: "Transcription"),
              symbol: "captions.bubble"),
        .init(op: "summarize-video",
              title: String(localized: "Video Summaries"),
              symbol: "text.below.photo"),
        .init(op: "upscale-video", title: String(localized: "Clip Upscaling"),
              symbol: "arrow.up.left.and.arrow.down.right"),
        .init(op: "upscale-image",
              title: String(localized: "Picture Upscaling"),
              symbol: "plus.magnifyingglass"),
        .init(op: "chat", title: String(localized: "Agentic Helper"),
              symbol: "brain"),
        .init(op: "agent-turn", title: String(localized: "Agent Turns"),
              symbol: "wand.and.stars"),
    ]

    static func of(_ op: String) -> FleetTaskLabel {
        all.first { $0.op == op }
            ?? FleetTaskLabel(op: op, title: op, symbol: "gearshape")
    }
}

/// A row of a Settings page that opens a page under it, as System
/// Settings draws one: its icon, its name, what it is set to, a chevron.
struct SettingsLinkRow: View {
    let title: String
    let detail: String
    let symbol: String
    let color: Color
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            HStack(spacing: 10) {
                SettingsIcon(symbol: symbol, color: color, size: 22)
                Text(verbatim: title)
                Spacer(minLength: 8)
                Text(verbatim: detail)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                Image(systemName: "chevron.right")
                    .font(.system(size: 11, weight: .semibold))
                    .foregroundStyle(.tertiary)
            }
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .accessibilityAddTraits(.isLink)
    }
}
