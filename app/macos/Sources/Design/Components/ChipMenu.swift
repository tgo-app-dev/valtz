import SwiftUI

/// A chip under the prompt: a setting's name and its value in a capsule
/// -- "Model · Auto (Krea 2 Turbo)", "16:9 · Large", "5 s" -- a chevron
/// after them. A click opens its popover, where the setting is changed;
/// the chip reads the new value as it closes. VoiceOver hears the name,
/// then the value, and that it shows its settings.
///
/// `appKit`: AppKit's popover, semi-transient, in place of SwiftUI's --
/// it stays open while Valtz is left (Favor's, for weights dragged into
/// Tune from the Finder). `press`: what a click does, in place of
/// toggling `isOpen` (the dock's chips know whether the popover was open
/// as the press began; it shuts itself on that press).
struct ChipMenu<Popover: View>: View {
    /// The setting's name, as the UI's language says it.
    let label: String
    let value: String
    var systemImage: String?
    @Binding var isOpen: Bool
    var appKit = false
    var press: (() -> Void)?
    @ViewBuilder var popover: () -> Popover

    var body: some View {
        let chip = Button {
            if let press { press() } else { isOpen.toggle() }
        } label: {
            HStack(spacing: VZ.Space.xs) {
                if let systemImage {
                    Image(systemName: systemImage)
                        .foregroundStyle(.secondary)
                }
                Text(verbatim: label).foregroundStyle(.secondary)
                Text(verbatim: value)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Image(systemName: "chevron.down")
                    .font(.vzCaption.weight(.semibold))
                    .imageScale(.small)
                    .foregroundStyle(.secondary)
            }
            .font(.vzCallout)
        }
        .buttonStyle(.bordered)
        .buttonBorderShape(.capsule)
        .controlSize(.small)
        .accessibilityLabel(Text(verbatim: label))
        .accessibilityValue(Text(verbatim: value))
        .accessibilityHint(Text("Shows its settings"))
        .accessibilityAddTraits(isOpen ? .isSelected : [])
        // Over the chip: the dock stands at the window's foot.
        if appKit {
            chip.appKitPopover(isPresented: $isOpen, anchor: .top,
                               arrowEdge: .top) {
                popover()
            }
        } else {
            chip.popover(isPresented: $isOpen, arrowEdge: .top) {
                popover()
            }
        }
    }
}
