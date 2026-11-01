import SwiftUI

/// The window's right column, the system's inspector: Inspector or Agent,
/// picked at its top.
///
/// The inspector is a stack of sections, each under a header that folds
/// it, the open ones sharing the height -- a divider between them dragged
/// to move it, double-clicked to even it out: Layers and Information,
/// then the selected layer's edits, Adjust, Color, Grade, Crop, Trim and
/// Audio, each listed only while it applies (AppModel.sectionApplies) and
/// never greyed. An edit's header carries its keyframes; Color's and
/// Grade's a button that opens them at their full width in a popover.
/// What is folded, the shares and the panel picked are the window's
/// chrome, kept between launches.
///
/// The Agent is its session at work -- its state and controls, the hold
/// note, its words and steps, the step waiting for a word -- or, with
/// none, what it is for.
struct RightColumn: View {
    @Bindable var model: AppModel

    var body: some View {
        VStack(spacing: 0) {
            Picker("Right Column", selection: $model.chrome.rightPanel) {
                Label("Inspector", systemImage: "info.circle")
                    .tag(RightPanel.inspector)
                Label("Agent", systemImage: Modality.agent.symbol)
                    .tag(RightPanel.agent)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .labelStyle(.titleAndIcon)
            .controlSize(.regular)
            .accessibilityLabel(Text("Right Column"))
            .help("Show the inspector or the Agent")
            .padding(.horizontal, VZ.Space.m)
            .padding(.vertical, VZ.Space.s)
            Divider()
            switch model.chrome.rightPanel {
            case .inspector: inspector
            case .agent: AgentPanel(model: model)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }

    private var sections: [CollapsibleSection<InspectorSection>] {
        model.inspectorSections.map {
            CollapsibleSection(
                $0, label: $0.label, symbol: $0.symbol,
                accessory: $0.isEdit
                    ? AnyView(SectionAccessory(model: model, section: $0))
                    : nil)
        }
    }

    private var inspector: some View {
        CollapsibleStack(
            folded: $model.chrome.collapsedSections,
            weights: $model.chrome.sectionWeights,
            sections: sections) { s in
            switch s {
            case .layers:
                // Its notes while it has the column to itself.
                LayersSection(
                    model: model,
                    notes: model.chrome.collapsedSections.contains(.info))
                    .padding(.top, VZ.Space.xs)
                    .font(.system(size: 11))
                    .controlSize(.small)
            case .info:
                InformationSection(model: model)
            default:
                EditSection(model: model, section: s)
            }
        }
        // The Color section out of sight: its target tool lets go of the
        // stage.
        .onChange(of: model.sectionShown(.color)) { _, shown in
            if !shown { model.colorTargeting = false }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
    }
}

/// A layer's edit, laid out for the column's width (280-480 points): its
/// panel's compact layout, scrolling when its share is shorter than it.
private struct EditSection: View {
    @Bindable var model: AppModel
    let section: InspectorSection

    var body: some View {
        ScrollView(.vertical) {
            panel
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, VZ.Space.m)
                .padding(.vertical, VZ.Space.m)
        }
        .scrollBounceBehavior(.basedOnSize)
    }

    @ViewBuilder
    private var panel: some View {
        switch section {
        case .adjust: AdjustPanel(model: model)
        case .color: ColorPanel(model: model, compact: true)
        case .grade: GradePanel(model: model, compact: true)
        case .crop: CropPanel(model: model)
        case .trim: TrimPanel(model: model)
        case .audio: AudioPanel(model: model)
        case .layers, .info: EmptyView()
        }
    }
}

/// An edit section's header accessory: the keyframes of what it keys --
/// the look's for Adjust, Color and Grade, the placement's for Crop (its
/// rotation, a track of its own, keeps its row in the body), the sound's
/// for Audio -- while the stage has keys (a clip, a still with pages; a
/// sound's always); and Color's and Grade's wide view.
private struct SectionAccessory: View {
    @Bindable var model: AppModel
    let section: InspectorSection
    @State private var wide = false

    private var track: KeyTrack? {
        switch section {
        case .adjust, .color, .grade: model.keyedStage ? .adjust : nil
        case .crop: model.keyedStage ? .place : nil
        case .audio: .sound
        case .layers, .info, .trim: nil
        }
    }

    var body: some View {
        HStack(spacing: VZ.Space.xs) {
            if let track {
                KeyframeBar(model: model, track: track, compact: true)
                    .accessibilityElement(children: .contain)
                    .accessibilityLabel(Text("Keyframes"))
            }
            if section == .color || section == .grade {
                Button {
                    wide = true
                } label: {
                    Label("Show Wide", systemImage:
                            "arrow.up.left.and.arrow.down.right")
                        .labelStyle(.iconOnly)
                        .frame(width: 22)
                }
                .buttonStyle(.borderless)
                .help(section == .color
                      ? "Open the colour mixer at its full width"
                      : "Open the colour grade at its full width")
                .popover(isPresented: $wide, arrowEdge: .leading) {
                    Group {
                        if section == .color {
                            ColorPanel(model: model)
                        } else {
                            GradePanel(model: model)
                        }
                    }
                    .padding(20)
                    .frame(width: 600)
                }
            }
        }
        .controlSize(.small)
        .fixedSize()
    }
}

/// The right column's Agent: the session at work, or what it is for.
private struct AgentPanel: View {
    @Bindable var model: AppModel

    var body: some View {
        if let a = model.agent {
            AgentPane(model: model, shown: a, inset: VZ.Space.m)
                .frame(maxWidth: .infinity, maxHeight: .infinity,
                       alignment: .top)
        } else {
            EmptyState.AgentIdle()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }
}
