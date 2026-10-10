import AppKit
import AVFoundation
import CoreGraphics
import SwiftUI

/// A permission macOS keeps for Valtz: what it is, where Valtz needs it,
/// its status now, and the System Settings pane that changes it.
struct PermissionItem: Identifiable, Sendable {
    enum Kind: String, CaseIterable, Sendable {
        case camera, microphone, screen, files, localNetwork, incoming
    }
    enum Status: Sendable {
        case granted, denied, notAsked, restricted
        /// macOS shows it only in System Settings: a folder's access is
        /// asked for, place by place, the first time Valtz reads there.
        case perPlace
        /// Not known until Valtz uses it (the local network: macOS tells
        /// an app only by letting it look, or not).
        case unknown
        /// Nothing to allow: the firewall is off.
        case firewallOff
    }
    let kind: Kind
    var status: Status
    var id: String { kind.rawValue }

    var title: String {
        switch kind {
        case .camera: String(localized: "Camera")
        case .microphone: String(localized: "Microphone")
        case .screen: String(localized: "Screen & System Audio Recording")
        case .files: String(localized: "Files and Folders")
        case .localNetwork: String(localized: "Local Network")
        case .incoming: String(localized: "Incoming Connections")
        }
    }

    var symbol: String {
        switch kind {
        case .camera: "camera.fill"
        case .microphone: "mic.fill"
        case .screen: "rectangle.dashed.badge.record"
        case .files: "folder.fill"
        case .localNetwork: "network"
        case .incoming: "shield.lefthalf.filled"
        }
    }

    var color: Color {
        switch kind {
        case .camera: .green
        case .microphone: .orange
        case .screen: .purple
        case .files: .blue
        case .localNetwork: .teal
        case .incoming: .gray
        }
    }

    /// When and where Valtz asks for it.
    var usedFor: String {
        switch kind {
        case .camera:
            String(localized: "A photo or a video taken with the camera: the prompt's paperclip › Take a Photo or Video….")
        case .microphone:
            String(localized: "Sound recorded from a microphone: the paperclip › Record Sound › From the Microphone, a blank audio asset's Record, and the sound of a video taken with the camera.")
        case .screen:
            String(localized: "What the Mac plays, recorded: the paperclip › Record Sound › From the System’s Audio, and a blank audio asset's Record with the system's audio as its source. Valtz records no picture of the screen.")
        case .files:
            String(localized: "Pictures, clips and models in Desktop, Documents, Downloads or on an external or network disk: imported, linked or saved there. macOS asks the first time for each place.")
        case .localNetwork:
            String(localized: "The fleet: finding the other Valtz Macs of this network, and the fleets to join, and connecting to them (Settings › Fleet). macOS asks the first time Valtz looks.")
        case .incoming:
            String(localized: "The fleet: a discoverable member is reached by the others, who send it jobs. With the macOS firewall on, macOS asks whether Valtz may accept their connections the first time it listens.")
        }
    }

    /// The pane of System Settings › Privacy & Security that holds it.
    var settingsURL: URL? {
        // The firewall is under Network, not Privacy & Security.
        if kind == .incoming {
            return URL(string:
                "x-apple.systempreferences:com.apple.Network-Settings.extension?Firewall")
        }
        let pane = switch kind {
        case .camera: "Privacy_Camera"
        case .microphone: "Privacy_Microphone"
        case .screen: "Privacy_ScreenCapture"
        case .files: "Privacy_FilesAndFolders"
        case .localNetwork: "Privacy_LocalNetwork"
        case .incoming: ""
        }
        return URL(string:
            "x-apple.systempreferences:com.apple.preference.security?\(pane)")
    }

    /// Whether macOS may still ask (it asks once; after that, System
    /// Settings changes it). Screen recording's request is offered while
    /// it is not allowed: macOS tells no "not asked" apart from "no".
    var canAsk: Bool {
        switch kind {
        case .files, .incoming: false
        case .screen: status != .granted
        // Looking is what asks (and what tells).
        case .localNetwork: status == .unknown
        default: status == .notAsked
        }
    }

    /// The firewall as `socketfilterfw` reads it (no administrator
    /// needed): off, blocking everything, or Valtz allowed or blocked --
    /// nil, not listed yet (macOS asks when Valtz first listens).
    nonisolated static func firewall() -> Status {
        func run(_ args: [String]) -> String {
            let p = Process()
            p.executableURL = URL(fileURLWithPath:
                "/usr/libexec/ApplicationFirewall/socketfilterfw")
            p.arguments = args
            let out = Pipe()
            p.standardOutput = out
            p.standardError = out
            guard (try? p.run()) != nil else { return "" }
            let data = out.fileHandleForReading.readDataToEndOfFile()
            p.waitUntilExit()
            return String(decoding: data, as: UTF8.self)
        }
        if run(["--getglobalstate"]).contains("disabled") {
            return .firewallOff
        }
        if run(["--getblockall"]).contains("enabled") { return .denied }
        let app = run(["--getappblocked", Bundle.main.bundlePath])
        if app.contains("permitted") { return .granted }
        if app.contains("blocked") { return .denied }
        return .notAsked
    }

    /// Every permission, as it stands now: the local network as browsing
    /// found it (the fleet's), the firewall as read.
    @MainActor
    static func current(localNetwork: String?,
                        firewall: Status) -> [PermissionItem] {
        func av(_ media: AVMediaType) -> Status {
            switch AVCaptureDevice.authorizationStatus(for: media) {
            case .authorized: .granted
            case .denied: .denied
            case .restricted: .restricted
            default: .notAsked
            }
        }
        // Screen recording has no "not asked": preflight says only
        // whether it is allowed.
        let screen: Status = CGPreflightScreenCaptureAccess() ? .granted
                                                              : .denied
        return [
            PermissionItem(kind: .camera, status: av(.video)),
            PermissionItem(kind: .microphone, status: av(.audio)),
            PermissionItem(kind: .screen, status: screen),
            PermissionItem(kind: .files, status: .perPlace),
            PermissionItem(kind: .localNetwork,
                           status: localNetwork == "allowed" ? .granted
                               : localNetwork == "denied" ? .denied
                               : .unknown),
            PermissionItem(kind: .incoming, status: firewall),
        ]
    }

    /// The system's prompt, where it can still be shown.
    @MainActor
    func ask() async {
        switch kind {
        case .camera: _ = await AVCaptureDevice.requestAccess(for: .video)
        case .microphone: _ = await AVCaptureDevice.requestAccess(for: .audio)
        case .screen: _ = CGRequestScreenCaptureAccess()
        case .files, .incoming: break
        // The network looked over a moment: macOS asks, the first time,
        // and the answer shows in what browsing finds.
        case .localNetwork: break
        }
    }
}

/// Settings › Permissions: each permission Valtz may ask macOS for --
/// whether it is allowed now, when and where Valtz needs it -- with the
/// system's prompt while it has not been shown, and the pane of System
/// Settings that changes it. Read again whenever Valtz comes back to the
/// front (from System Settings, say).
struct PermissionsView: View {
    @Bindable var model: AppModel
    @State private var items: [PermissionItem] = []
    @State private var firewall: PermissionItem.Status = .notAsked

    var body: some View {
        Form {
            Section { SettingsHeader(page: .permissions) }
            Section {
                ForEach(items) { p in
                    PermissionRow(item: p) {
                        Task {
                            if p.kind == .localNetwork {
                                await checkLocalNetwork()
                            } else {
                                await p.ask()
                            }
                            refresh()
                        }
                    }
                }
            } footer: {
                Text("A permission changed in System Settings takes effect when Valtz is opened again for screen and system audio recording; the others at once.")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
        .onAppear(perform: refresh)
        .onReceive(NotificationCenter.default.publisher(
            for: NSApplication.didBecomeActiveNotification)) { _ in
            refresh()
        }
    }

    private func refresh() {
        model.reloadFleet()
        items = PermissionItem.current(
            localNetwork: model.fleet?.localNetwork, firewall: firewall)
        Task {
            let fw = await Task.detached { PermissionItem.firewall() }.value
            firewall = fw
            items = PermissionItem.current(
                localNetwork: model.fleet?.localNetwork, firewall: fw)
        }
    }

    /// The network looked over for a few seconds -- macOS asks the first
    /// time -- and what that told of the permission.
    private func checkLocalNetwork() async {
        model.browseFleets(true)
        for _ in 0..<20 {
            try? await Task.sleep(for: .milliseconds(250))
            model.reloadFleet()
            if !(model.fleet?.localNetwork ?? "").isEmpty { break }
        }
        // The Fleet page looks while it is open; this page does not.
        model.browseFleets(false)
    }
}

private struct PermissionRow: View {
    let item: PermissionItem
    let ask: () -> Void

    var body: some View {
        HStack(alignment: .top, spacing: 12) {
            Image(systemName: item.symbol)
                .font(.system(size: 14, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 28, height: 28)
                .background(item.color.gradient,
                            in: RoundedRectangle(cornerRadius: 7))
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 8) {
                    Text(verbatim: item.title)
                        .font(.body.weight(.medium))
                    badge
                }
                Text(verbatim: item.usedFor)
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 8) {
                    if item.canAsk {
                        Button(item.kind == .localNetwork ? "Check Now"
                                                          : "Ask Now",
                               action: ask)
                    }
                    if let url = item.settingsURL {
                        Button("Open System Settings") {
                            NSWorkspace.shared.open(url)
                        }
                    }
                }
                .controlSize(.small)
                .padding(.top, 2)
            }
            Spacer(minLength: 0)
        }
        .padding(.vertical, 4)
    }

    private var badge: some View {
        let (text, color): (String, Color) = switch item.status {
        case .granted: (String(localized: "Allowed"), .green)
        case .denied: (String(localized: "Not allowed"), .red)
        case .restricted: (String(localized: "Restricted"), .orange)
        case .notAsked: (String(localized: "Not asked yet"), .secondary)
        case .perPlace: (String(localized: "Asked for each place"), .secondary)
        case .unknown: (String(localized: "Not known yet"), .secondary)
        case .firewallOff: (String(localized: "Firewall off: nothing to allow"), .secondary)
        }
        return Text(verbatim: text)
            .font(.caption.weight(.medium))
            .foregroundStyle(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 1)
            .overlay(Capsule().strokeBorder(color.opacity(0.5)))
    }
}
