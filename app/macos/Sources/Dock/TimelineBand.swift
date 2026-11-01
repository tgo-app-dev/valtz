import SwiftUI

/// The timeline band, over the composer in the dock (DESIGN §6): the
/// TimelineCard in a dock card, while a timeline (or a still with pages)
/// is on the stage. Open, its rows are what its height leaves room for --
/// three while the dock has the height its content asks, else what the
/// composer leaves of the dock's, from one row to twelve; the splitter
/// over the dock is how it is made taller. Folded away, a disclosure
/// alone, where the band's left edge was: the band wanted or not is the
/// window's chrome (chrome.timelineBand), kept between launches.
struct TimelineBand: View {
    @Bindable var model: AppModel
    /// The dock's height is set: the band takes what the composer leaves.
    var fills = false

    var body: some View {
        if model.timelineOpen {
            DockCard {
                TimelineCard(model: model)
            }
            .frame(height: fills ? nil
                   : TimelineCard.height(rows: AppModel.timelineDefaultRows))
            .frame(minHeight: fills ? TimelineCard.height(rows: 1) : nil,
                   maxHeight: fills
                       ? TimelineCard.height(
                           rows: AppModel.timelineRowRange.upperBound)
                       : nil)
            // Past twelve rows, the room over it is left empty.
            .frame(maxHeight: fills ? .infinity : nil, alignment: .bottom)
            // The rows in view, for what reads them (the hooks' summary,
            // setTimelineRows); a row cut short is drawn, not counted.
            .onGeometryChange(for: CGFloat.self) { $0.size.height } action: {
                model.timelineRows = TimelineCard.rows(height: $0)
            }
            .transition(.move(edge: .bottom).combined(with: .opacity))
        } else {
            folded
                .transition(.opacity)
        }
    }

    /// The band folded: its disclosure and its name, the row a button.
    private var folded: some View {
        Button {
            model.showTimeline(true)
        } label: {
            HStack(spacing: VZ.Space.xs + 2) {
                Image(systemName: "chevron.right")
                    .font(.vzCaption.weight(.semibold))
                    .frame(width: 24)
                Text("Timeline")
                    .font(.vzCallout)
                Spacer(minLength: 0)
            }
            .foregroundStyle(.secondary)
            .frame(height: Self.foldedHeight)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("Show the timeline over the prompt (⇧⌘T)")
        .shortcutHint("⇧⌘T")
        .accessibilityLabel(Text("Timeline"))
        .accessibilityValue(Text("Collapsed"))
        .accessibilityHint(Text("Shows the timeline over the prompt"))
    }

    /// The folded band's row.
    static let foldedHeight: CGFloat = 22
}

/// The band's disclosure, open: the first of the timeline's corner
/// tools; it folds the band away.
struct TimelineDisclosure: View {
    let model: AppModel

    var body: some View {
        TimelineTool(symbol: "chevron.down", on: false,
                     help: "Hide the timeline (⇧⌘T)") {
            model.showTimeline(false)
        }
        .shortcutHint("⇧⌘T")
    }
}

extension AppModel {
    /// The band given `n` rows (a hook's `rows=`): the dock's height set
    /// to the composer's and that band's -- what dragging the splitter
    /// there does.
    func setTimelineRows(_ n: Int) {
        let r = Self.timelineRowRange
        let rows = min(r.upperBound, max(r.lowerBound, n))
        guard dockComposerHeight > 0 else { return }
        var h = dockComposerHeight + DockView.gap
            + TimelineCard.height(rows: rows) + DockView.inset
        if stageColumnHeight > 0 {
            h = min(h, DockView.bounds(column: stageColumnHeight).upperBound)
        }
        withAnimation(Self.motion) { chrome.dockHeight = h }
    }
}
