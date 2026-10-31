import SwiftUI

/// Valtz's design tokens: the few measures its own chrome -- the stage,
/// the timeline, the composer's card -- is built from, so every surface
/// shares them. Where SwiftUI takes an optional length (a stack's
/// spacing, a padding), leave it out and the system supplies the Mac's
/// own; these are for what the system does not size.
enum VZ {
    /// Spacing, on a 4-point grid.
    enum Space {
        static let xxs: CGFloat = 2
        static let xs: CGFloat = 4
        static let s: CGFloat = 8
        static let m: CGFloat = 12
        static let l: CGFloat = 16
        static let xl: CGFloat = 20
        static let xxl: CGFloat = 32
    }

    /// Corner radii. A control is a capsule or `control`; a card inside a
    /// container follows the container's corner (concentric), never a
    /// radius of its own.
    enum Radius {
        static let control: CGFloat = 6
        static let card: CGFloat = 12
        static let dock: CGFloat = 16

        /// A shape whose corners follow the container's -- a card in the
        /// dock, a section in the inspector -- rounded at least `minimum`
        /// where the container's corner is far away.
        static func concentric(minimum: CGFloat = card) -> ConcentricRectangle {
            ConcentricRectangle(corners: .concentric(minimum: .fixed(minimum)))
        }
    }

    /// Motion: the one curve every layout change shares, and a quicker
    /// one for a control's own state.
    @MainActor enum Motion {
        static var standard: Animation { AppModel.motion }
        static let quick = Animation.smooth(duration: 0.2 * AppModel.animationScale)
    }

    /// Elevation: two shadows, and no others. `dock` lifts the composer's
    /// card and the timeline off the window; `float` a notice or a glass
    /// overlay off what it covers.
    enum Elevation {
        static let dock = Shadow(color: .black.opacity(0.13), radius: 18, y: 8)
        static let float = Shadow(color: .black.opacity(0.18), radius: 10, y: 4)

        struct Shadow {
            var color: Color
            var radius: CGFloat
            var y: CGFloat
        }
    }

    /// Sizes the system does not set.
    enum Size {
        /// The contextual row above the stage (markup's tools, compare's).
        static let contextBar: CGFloat = 34
        /// The dock at its smallest: a prompt line and the chip row.
        static let dockMin: CGFloat = 132
        /// The timeline's rows, ruler and transport (TimelineNSView).
        static let timelineRow: CGFloat = 22
        static let timelineRuler: CGFloat = 26
        static let transport: CGFloat = 48
        /// A meter's bar: its length, its thickness.
        static let meter: CGFloat = 36
        static let meterBar: CGFloat = 5
        /// A section's header row (the inspector's, the gallery's).
        static let sectionHeader: CGFloat = 26
        /// A stage button's box, and the symbol in it (StageButton).
        static let toolbarButton: CGFloat = 28
        static let toolbarSymbol: CGFloat = 15
        /// The badge column before a task row's title.
        static let badgeColumn: CGFloat = 24
    }
}

extension View {
    /// One of the two shadows.
    func vzShadow(_ s: VZ.Elevation.Shadow) -> some View {
        shadow(color: s.color, radius: s.radius, y: s.y)
    }
}
