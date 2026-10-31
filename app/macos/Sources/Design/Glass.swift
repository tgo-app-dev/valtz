import SwiftUI

/// Where Liquid Glass goes, and where it does not.
///
/// Glass is the NAVIGATION layer: the window's toolbar, sidebar and
/// inspector draw it themselves, and the controls that float over the
/// stage -- a generation's status, a clip's bar, the markup's loupe --
/// take it through `vzStageOverlay()`. Content never does: the composer's
/// card, the timeline, an inspector section, a chip, a list row sit on
/// the window's own background or a material, so the glass above them has
/// something to bend. Glass is never stacked on glass, and it is always
/// the regular variant: the clear one needs a dimmed picture under it.
extension View {
    /// A control floating over the stage: regular glass in a capsule,
    /// grouped with its neighbours by the `GlassEffectContainer` around
    /// them.
    func vzStageOverlay() -> some View {
        glassEffect(.regular, in: Capsule())
    }

    /// A control floating over the stage that takes clicks: the glass
    /// answers them.
    func vzStageControl() -> some View {
        glassEffect(.regular.interactive(), in: Capsule())
    }
}
