import SwiftUI

/// A chip under the prompt: a setting's name and its value in a capsule
/// -- "Model · Auto (Krea 2 Turbo)", "16:9 · Large", "5 s" -- a chevron
/// after them. A click opens its popover, where the setting is changed;
/// the chip reads the new value as it closes. VoiceOver hears the name,
/// then the value.
struct ChipMenu<Popover: View>: View {
    let label: LocalizedStringKey
    let value: String
    var systemImage: String?
    @Binding var isOpen: Bool
    @ViewBuilder var popover: () -> Popover

    var body: some View {
        Button {
            isOpen.toggle()
        } label: {
            HStack(spacing: VZ.Space.xs) {
                if let systemImage {
                    Image(systemName: systemImage)
                        .foregroundStyle(.secondary)
                }
                Text(label).foregroundStyle(.secondary)
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
        .accessibilityLabel(Text(label))
        .accessibilityValue(Text(verbatim: value))
        .accessibilityHint(Text("Shows its settings"))
        .accessibilityAddTraits(isOpen ? .isSelected : [])
        .popover(isPresented: $isOpen, arrowEdge: .bottom) {
            popover()
        }
    }
}
