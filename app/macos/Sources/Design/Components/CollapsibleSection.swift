import SwiftUI

/// A column of sections, each under a header that folds it: the
/// inspector's Layers, Information and a layer's Adjust, Color, Grade,
/// Crop, Trim and Audio. Open sections share the column's height by
/// weight; a divider between two open ones is dragged to move height
/// from one to the other, and double-clicked to even them out. Folded,
/// a section is its header alone.
///
/// The caller owns the state -- which are folded, each one's weight --
/// keyed by its own `ID`, so it can remember them.
struct CollapsibleStack<ID: Hashable, Content: View>: View {
    @Binding var folded: Set<ID>
    @Binding var weights: [ID: Double]
    let sections: [CollapsibleSection<ID>]
    @ViewBuilder var content: (ID) -> Content
    /// Each open section's height as laid out: what a drag starts from.
    @State private var heights: [ID: CGFloat] = [:]

    var body: some View {
        StackedPanels {
            ForEach(sections) { s in
                let open = !folded.contains(s.id)
                VStack(spacing: 0) {
                    SectionHeader(section: s, open: open) {
                        withAnimation(VZ.Motion.standard) {
                            if open { folded.insert(s.id) }
                            else { folded.remove(s.id) }
                        }
                    }
                    .overlay(alignment: .top) {
                        if let above = openAbove(s.id) {
                            SectionDivider(
                                above: above.id, below: s.id,
                                names: (above.label, s.label),
                                heights: heights, weights: $weights)
                        }
                    }
                    if open {
                        content(s.id)
                            .frame(maxWidth: .infinity, maxHeight: .infinity,
                                   alignment: .top)
                            .onGeometryChange(for: CGFloat.self) {
                                $0.size.height
                            } action: { heights[s.id] = $0 }
                    }
                }
                .layoutValue(key: PanelWeight.self,
                             value: open ? weights[s.id] ?? 1 : nil)
            }
        }
    }

    /// The open section right above `id`, if the one above is open.
    private func openAbove(_ id: ID) -> CollapsibleSection<ID>? {
        guard let i = sections.firstIndex(where: { $0.id == id }), i > 0
        else { return nil }
        let a = sections[i - 1]
        return folded.contains(a.id) ? nil : a
    }
}

/// What a section is: its id, its name (localized by the caller: a
/// divider names the sections it parts), its symbol, and an accessory
/// drawn at the header's trailing end (a layer's keyframe bar).
struct CollapsibleSection<ID: Hashable>: Identifiable {
    let id: ID
    let label: String
    let symbol: String
    var accessory: AnyView?

    init(_ id: ID, label: String, symbol: String,
         accessory: AnyView? = nil) {
        self.id = id
        self.label = label
        self.symbol = symbol
        self.accessory = accessory
    }
}

/// Open sections share the height by weight; a folded one's header is at
/// its own.
private struct StackedPanels: Layout {
    func sizeThatFits(proposal: ProposedViewSize, subviews: Subviews,
                      cache: inout ()) -> CGSize {
        let w = proposal.width ?? 260
        let fixed = subviews.reduce(CGFloat(0)) { sum, v in
            v[PanelWeight.self] == nil
                ? sum + v.sizeThatFits(.init(width: w, height: nil)).height
                : sum
        }
        return CGSize(width: w, height: max(proposal.height ?? fixed, fixed))
    }

    func placeSubviews(in bounds: CGRect, proposal: ProposedViewSize,
                       subviews: Subviews, cache: inout ()) {
        let w = bounds.width
        var fixed: CGFloat = 0
        var total = 0.0
        var own: [CGFloat?] = []
        for v in subviews {
            if let weight = v[PanelWeight.self] {
                total += weight
                own.append(nil)
            } else {
                let h = v.sizeThatFits(.init(width: w, height: nil)).height
                fixed += h
                own.append(h)
            }
        }
        let rest = max(0, bounds.height - fixed)
        var y = bounds.minY
        for (i, v) in subviews.enumerated() {
            let h = own[i] ?? (total > 0
                ? rest * CGFloat((v[PanelWeight.self] ?? 0) / total) : 0)
            v.place(at: CGPoint(x: bounds.minX, y: y), anchor: .topLeading,
                    proposal: .init(width: w, height: h))
            y += h
        }
    }
}

private struct PanelWeight: LayoutValueKey {
    static let defaultValue: Double? = nil
}

/// A section's header: its fold, its symbol, its name, its accessory.
private struct SectionHeader<ID: Hashable>: View {
    let section: CollapsibleSection<ID>
    let open: Bool
    let toggle: () -> Void
    @State private var hovering = false

    var body: some View {
        HStack(spacing: VZ.Space.s) {
            Button(action: toggle) {
                HStack(spacing: 6) {
                    Image(systemName: "chevron.right")
                        .font(.vzCaption.weight(.semibold))
                        .imageScale(.small)
                        .rotationEffect(.degrees(open ? 90 : 0))
                        .foregroundStyle(.secondary)
                        .frame(width: 10)
                    Image(systemName: section.symbol)
                        .foregroundStyle(.secondary)
                    Text(verbatim: section.label)
                        .font(.vzLabel.weight(.semibold))
                    Spacer(minLength: 0)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel(Text(verbatim: section.label))
            .accessibilityValue(Text(open ? "Open" : "Folded"))
            .accessibilityAddTraits(.isHeader)
            .help(open ? "Fold this section to its header"
                       : "Open this section")
            if let a = section.accessory, open {
                a
            }
        }
        .padding(.horizontal, VZ.Space.m)
        .frame(height: VZ.Size.sectionHeader)
        .frame(maxWidth: .infinity)
        .background(Color.primary.opacity(hovering ? 0.07 : 0.04))
        .onHover { hovering = $0 }
        .overlay(alignment: .bottom) { Divider() }
    }
}

/// The divider between two open sections, the top edge of the lower
/// one's header: dragged, the one above takes height from the one below
/// (or gives it), neither under `least`; double-clicked, every section's
/// share is even again.
private struct SectionDivider<ID: Hashable>: View {
    let above: ID
    let below: ID
    let names: (String, String)
    let heights: [ID: CGFloat]
    @Binding var weights: [ID: Double]
    @State private var start: (ha: CGFloat, hb: CGFloat, wa: Double,
                               wb: Double)?
    private let least: CGFloat = 60

    var body: some View {
        DragHandle(label: String(localized: "Divider between \(names.0) and \(names.1)"),
                   value: heights[above].map {
                       String(localized: "\(Int($0)) points")
                   },
                   changed: drag, ended: { start = nil },
                   reset: { weights = [:] })
            .frame(height: 6)
            .frame(maxWidth: .infinity)
            .help("Drag to share the height between the sections; double-click to share it evenly")
    }

    private func drag(_ dy: CGFloat) {
        if start == nil {
            guard let ha = heights[above], let hb = heights[below],
                  ha + hb > 0 else { return }
            start = (ha, hb, weights[above] ?? 1, weights[below] ?? 1)
        }
        guard let s = start else { return }
        let sum = s.ha + s.hb
        let ha = min(max(s.ha + dy, min(least, sum / 2)),
                     sum - min(least, sum / 2))
        let wa = (s.wa + s.wb) * Double(ha / sum)
        weights[above] = wa
        weights[below] = s.wa + s.wb - wa
    }
}
