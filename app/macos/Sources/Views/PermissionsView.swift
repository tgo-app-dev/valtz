import AppKit
import AVFoundation
import CoreGraphics
import SwiftUI

/// A permission macOS keeps for Valtz: what it is, where Valtz needs it,
/// its status now, and the System Settings pane that changes it.
struct PermissionItem: Identifiable, Sendable {
    enum Kind: String, CaseIterable, Sendable {
        case camera, microphone, screen, files
    }
    enum Status: Sendable {
        case granted, denied, notAsked, restricted
        /// macOS shows it only in System Settings: a folder's access is
        /// asked for, place by place, the first time Valtz reads there.
        case perPlace
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
        }
    }

    var symbol: String {
        switch kind {
        case .camera: "camera.fill"
        case .microphone: "mic.fill"
        case .screen: "rectangle.dashed.badge.record"
        case .files: "folder.fill"
        }
    }

    var color: Color {
        switch kind {
        case .camera: .green
        case .microphone: .orange
        case .screen: .purple
        case .files: .blue
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
        }
    }

    /// The pane of System Settings › Privacy & Security that holds it.
    var settingsURL: URL? {
        let pane = switch kind {
        case .camera: "Privacy_Camera"
        case .microphone: "Privacy_Microphone"
        case .screen: "Privacy_ScreenCapture"
        case .files: "Privacy_FilesAndFolders"
        }
        return URL(string:
            "x-apple.systempreferences:com.apple.preference.security?\(pane)")
    }

    /// Whether macOS may still ask (it asks once; after that, System
    /// Settings changes it). Screen recording's request is offered while
    /// it is not allowed: macOS tells no "not asked" apart from "no".
    var canAsk: Bool {
        switch kind {
        case .files: false
        case .screen: status != .granted
        default: status == .notAsked
        }
    }

    /// Every permission, as it stands now.
    @MainActor
    static func current() -> [PermissionItem] {
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
        ]
    }

    /// The system's prompt, where it can still be shown.
    @MainActor
    func ask() async {
        switch kind {
        case .camera: _ = await AVCaptureDevice.requestAccess(for: .video)
        case .microphone: _ = await AVCaptureDevice.requestAccess(for: .audio)
        case .screen: _ = CGRequestScreenCaptureAccess()
        case .files: break
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

    var body: some View {
        Form {
            Section { SettingsHeader(page: .permissions) }
            Section {
                ForEach(items) { p in
                    PermissionRow(item: p) {
                        Task {
                            await p.ask()
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
        items = PermissionItem.current()
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
                        Button("Ask Now", action: ask)
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
        }
        return Text(verbatim: text)
            .font(.caption.weight(.medium))
            .foregroundStyle(color)
            .padding(.horizontal, 6)
            .padding(.vertical, 1)
            .overlay(Capsule().strokeBorder(color.opacity(0.5)))
    }
}
