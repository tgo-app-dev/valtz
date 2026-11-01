import SwiftUI

/// What a region says with nothing in it: the system's own empty state,
/// with Valtz's words. Each is a view of its own so the words are
/// written once.
enum EmptyState {
    /// The inspector with nothing on the stage.
    struct NoSelection: View {
        var body: some View {
            ContentUnavailableView(
                "Nothing to Inspect", systemImage: "info.circle",
                description: Text("Put something on the stage, or select a layer."))
        }
    }

    /// Layers, with no picture or clip on the stage.
    struct NoLayers: View {
        var body: some View {
            ContentUnavailableView(
                "No Layers", systemImage: "square.3.layers.3d",
                description: Text("A picture or a clip on the stage has layers."))
        }
    }

    /// Assets, before the first generation or import.
    struct NoAssets: View {
        var body: some View {
            ContentUnavailableView(
                "No Assets Yet", systemImage: "photo.stack",
                description: Text("Everything you make or add lands here."))
        }
    }

    /// The Agent panel with no conversation.
    struct AgentIdle: View {
        var body: some View {
            ContentUnavailableView(
                "No Conversation", systemImage: "wand.and.stars",
                description: Text("Choose Agent in the composer (⌘4) and ask for something to make."))
        }
    }

    /// The activity popover with nothing running.
    struct NoActivity: View {
        var body: some View {
            ContentUnavailableView(
                "Nothing Running", systemImage: "checkmark.circle",
                description: Text("Generations and exports show here as they run."))
        }
    }
}
