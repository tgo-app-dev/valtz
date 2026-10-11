import SwiftUI

/// Settings › Experimental: what is still being built, switched on here to
/// try it -- off until it is. The AGENT (DESIGN §9b): the prompt's Agent
/// tab, where a request is worked into the document by a model calling
/// the editor's tools -- pages and words, pictures, clips cut on a
/// timeline with music -- and, while it is on, where that model thinks
/// (AgentSettingsSection).
struct ExperimentalView: View {
    @Bindable var model: AppModel

    var body: some View {
        Form {
            Section { SettingsHeader(page: .experimental) }
            Section {
                Toggle(isOn: Binding(
                    get: { model.agentEnabled },
                    set: { model.setAgentEnabled($0) })) {
                    Label {
                        Text("Agent")
                    } icon: {
                        Image(systemName: Modality.agent.symbol)
                    }
                }
            } footer: {
                Text("The Agent tab over the prompt: ask for a deck, a poster or a video of your clips with music, and a model builds it in your document with the editor's tools, a step at a time. Switched off, the tab goes, and a session at work stops.")
                    .foregroundStyle(.secondary)
            }
            if model.agentEnabled {
                AgentSettingsSection(model: model)
            }
        }
        .formStyle(.grouped)
    }
}
