import SwiftUI

/// Settings › Fleet › Offload Policies (DESIGN §11): where this Mac's own
/// jobs run while it is in a fleet. Each TASK has a policy -- never sent,
/// local first (the default), fleet first, always on a member -- and a
/// MODEL may be set apart from its task's: a model's own comes first. The
/// models listed are those on this Mac or offered by a member, and any
/// set apart.
struct FleetOffloadView: View {
    @Bindable var model: AppModel

    var body: some View {
        Form {
            if let f = model.fleet, f.enabled {
                policiesSection(f)
                tasksSection(f)
                modelsSection(f)
            } else {
                Section {
                    Text("This Valtz takes part in no fleet.")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .onAppear { model.reloadFleet() }
    }

    /// What each policy does.
    private func policiesSection(_ f: FleetStatus) -> some View {
        Section {
            ForEach(FleetOffload.allCases) { p in
                HStack(alignment: .firstTextBaseline, spacing: 12) {
                    Text(verbatim: p.title)
                        .fontWeight(.semibold)
                        .frame(width: 92, alignment: .leading)
                    Text(verbatim: p.meaning)
                        .foregroundStyle(.secondary)
                        .fixedSize(horizontal: false, vertical: true)
                    Spacer(minLength: 0)
                }
                .settingsMark(p.title)
            }
        } footer: {
            if f.config?.joined != true {
                Text("They apply while this Mac is in a fleet. It is in none now: every job runs here.")
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func tasksSection(_ f: FleetStatus) -> some View {
        Section {
            ForEach(f.tasks ?? []) { t in
                let label = FleetTaskLabel.of(t.op)
                Picker(selection: Binding(
                    get: { FleetOffload.named(f.policies.task(t.op)) },
                    set: {
                        model.setFleetOffload(
                            task: t.op, $0 == .localFirst ? "" : $0.rawValue)
                    })) {
                    ForEach(FleetOffload.allCases) { p in
                        Text(verbatim: p.title).tag(p)
                    }
                } label: {
                    Label(label.title, systemImage: label.symbol)
                }
                .settingsMark(label.title)
            }
        } header: {
            Text("Tasks")
                .settingsMark(String(localized: "Tasks"))
        } footer: {
            Text("A chat with the helper goes to a member's own helper.")
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func modelsSection(_ f: FleetStatus) -> some View {
        let listed = Self.listed(f)
        Section {
            if listed.isEmpty {
                Text("No model is on this Mac or offered by a member yet.")
                    .foregroundStyle(.secondary)
            }
            ForEach(listed) { m in
                Picker(selection: Binding(
                    get: { f.policies.models[m.id] ?? "" },
                    set: { model.setFleetOffload(model: m.id, $0) })) {
                    Text("As Its Task").tag("")
                    Divider()
                    ForEach(FleetOffload.allCases) { p in
                        Text(verbatim: p.title).tag(p.rawValue)
                    }
                } label: {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: m.name)
                        Text(verbatim: m.tasks.joined(separator: ", "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .settingsMark(m.name)
            }
        } header: {
            Text("Models")
                .settingsMark(String(localized: "Models"))
        } footer: {
            Text("A model's own policy comes before its task's. Listed: the models on this Mac or offered by a member.")
                .foregroundStyle(.secondary)
        }
    }

    /// A model listed: once, with the tasks it does.
    struct Listed: Identifiable {
        var id: String
        var name: String
        var tasks: [String]
    }

    /// The models on this Mac, offered by a member or set apart, in the
    /// tasks' order. (A chat names no model.)
    static func listed(_ f: FleetStatus) -> [Listed] {
        let offered = Set((f.members ?? []).flatMap { $0.me?.installed ?? [] })
        var out: [Listed] = []
        for t in f.tasks ?? [] where t.op != "chat" && t.op != "agent-turn" {
            for m in t.models where m.installed || offered.contains(m.id)
                || f.policies.models[m.id] != nil {
                let task = FleetTaskLabel.of(t.op).title
                if let i = out.firstIndex(where: { $0.id == m.id }) {
                    out[i].tasks.append(task)
                } else {
                    out.append(Listed(id: m.id, name: m.name, tasks: [task]))
                }
            }
        }
        return out
    }
}

/// Settings › Fleet › Shared Capabilities (DESIGN §11): what this Mac
/// offers the other members -- every task its engine runs and every model
/// on it, until withheld. A withheld one is not said to be here, so no
/// member sends it (and one that does is declined).
struct FleetSharingView: View {
    @Bindable var model: AppModel

    var body: some View {
        Form {
            if let f = model.fleet, f.enabled {
                tasksSection(f)
                modelsSection(f)
                offeredSection(f)
            } else {
                Section {
                    Text("This Valtz takes part in no fleet.")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .onAppear { model.reloadFleet() }
    }

    private func tasksSection(_ f: FleetStatus) -> some View {
        Section {
            ForEach(f.tasks ?? []) { t in
                let label = FleetTaskLabel.of(t.op)
                Toggle(isOn: Binding(
                    get: { !f.policies.withheldTasks.contains(t.op) },
                    set: { model.setFleetShared(task: t.op, $0) })) {
                    VStack(alignment: .leading, spacing: 2) {
                        Label(label.title, systemImage: label.symbol)
                        if !t.runs {
                            Text("Not run on this Mac")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
                .disabled(!t.runs)
                .settingsMark(label.title)
            }
        } header: {
            Text("Tasks")
                .settingsMark(String(localized: "Tasks"))
        } footer: {
            Text("A task withheld is not offered: the members run it themselves or send it to another member.")
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func modelsSection(_ f: FleetStatus) -> some View {
        let here = FleetOffloadView.listed(f).filter { m in
            (f.tasks ?? []).contains { t in
                t.models.contains { $0.id == m.id && $0.installed }
            }
        }
        Section {
            if here.isEmpty {
                Text("No model is on this Mac yet.")
                    .foregroundStyle(.secondary)
            }
            ForEach(here) { m in
                Toggle(isOn: Binding(
                    get: { !f.policies.withheldModels.contains(m.id) },
                    set: { model.setFleetShared(model: m.id, $0) })) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: m.name)
                        Text(verbatim: m.tasks.joined(separator: ", "))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                }
                .settingsMark(m.name)
            }
        } header: {
            Text("Models")
                .settingsMark(String(localized: "Models"))
        } footer: {
            Text("Only models on this Mac are offered. One downloaded later is offered too, until it is withheld.")
                .foregroundStyle(.secondary)
        }
    }

    /// What the members are told this Mac offers, as it stands.
    @ViewBuilder
    private func offeredSection(_ f: FleetStatus) -> some View {
        if let me = f.me {
            Section {
                let features = me.features ?? []
                if features.isEmpty {
                    Text("Nothing: this Mac runs no model now.")
                        .foregroundStyle(.secondary)
                } else {
                    Text(features.map { CapabilityStatus.localizedLabel($0) }
                        .joined(separator: ", "))
                        .foregroundStyle(.secondary)
                }
                LabeledContent("Models") {
                    Text(String((me.installed ?? []).count))
                }
                if me.busy == true {
                    Label("Busy now", systemImage: "hourglass")
                        .foregroundStyle(.secondary)
                }
            } header: {
                Text("Offered Now")
                    .settingsMark(String(localized: "Offered Now"))
            }
        }
    }
}
