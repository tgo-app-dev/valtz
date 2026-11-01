import AppKit
import SwiftUI

/// Valtz's colour roles. The accent is the system's (a person's choice in
/// System Settings): primary actions, selection and keyed values take
/// `Color.accentColor` / `NSColor.controlAccentColor`. States are the
/// system's too: red for an error, orange for a warning or something
/// unused, yellow for warm. Panels take the window's own background or a
/// material, never a colour of their own.
extension Color {
    /// A card's edge: a hairline of the text colour, barely there.
    static let vzHairline = Color.primary.opacity(0.08)
    /// What lies behind a picture on the stage: a shade darker than the
    /// window, so a white picture has an edge.
    static let vzStageBackdrop = Color(nsColor: .underPageBackgroundColor)
}
