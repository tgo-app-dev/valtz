import SwiftUI

/// Settings › Fleet (DESIGN §11): this Mac as a member of a FLEET -- the
/// Valtz Macs of one network working as one. Its name among them,
/// whether it is discoverable; the fleet it is in (its secret changed,
/// left), or one found on the network joined, or one made; whether and
/// when it takes the others' jobs; the members, each with what it runs
/// and whether it is free; and what this Mac offers them. While the page
/// is open the network is looked over for fleets to join.
struct FleetView: View {
    @Bindable var model: AppModel
    @State private var name = ""
    @State private var newFleet = ""
    @State private var newSecret = ""
    @State private var joining: FleetSeen?
    @State private var changingSecret = false

    var body: some View {
        Form {
            Section { SettingsHeader(page: .fleet) }
            if let f = model.fleet, f.enabled, let c = f.config {
                thisMac(c)
                if c.joined {
                    fleetSection(f, c)
                    membersSection(f)
                } else {
                    joinSection(f)
                    createSection
                }
                jobsSection(c)
                offered(f)
            } else {
                Section {
                    Text("This Valtz takes part in no fleet.")
                        .foregroundStyle(.secondary)
                }
            }
        }
        .formStyle(.grouped)
        .onAppear {
            model.reloadFleet()
            model.browseFleets(true)
            name = model.fleet?.config?.memberName ?? ""
        }
        .onDisappear { model.browseFleets(false) }
        .sheet(item: $joining) { seen in
            FleetSecretSheet(title: String(localized: "Join “\(seen.fleet)”"),
                             action: String(localized: "Join"),
                             note: String(localized: "Ask a member of the fleet for its secret.")) {
                secret in
                model.configureFleet(["fleet": seen.fleet, "secret": secret])
            }
        }
        .sheet(isPresented: $changingSecret) {
            FleetSecretSheet(title: String(localized: "Change the Secret"),
                             action: String(localized: "Change"),
                             note: String(localized: "Every member needs the new secret: a member still holding the old one is no longer let in.")) {
                secret in
                model.configureFleet(["secret": secret])
            }
        }
    }

    // MARK: This Mac

    private func thisMac(_ c: FleetConfig) -> some View {
        Section {
            TextField("Name", text: $name, prompt: Text(c.memberName))
                .onSubmit {
                    if !name.trimmingCharacters(in: .whitespaces).isEmpty {
                        model.configureFleet(["member_name": name])
                    }
                }
            Toggle("Discoverable", isOn: Binding(
                get: { c.discoverable },
                set: { model.configureFleet(["discoverable": $0]) }))
        } header: {
            Text("This Mac")
        } footer: {
            Text("Discoverable, this Mac can be seen on the network by its fleet's members, who send it jobs, and by Macs looking for a fleet to join. Not discoverable, it can still send its own jobs to the members it finds.")
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Its fleet

    private func fleetSection(_ f: FleetStatus, _ c: FleetConfig) -> some View {
        Section("Fleet") {
            LabeledContent("Fleet") { Text(verbatim: c.fleet) }
            LabeledContent("Secret") {
                HStack {
                    Text("Set").foregroundStyle(.secondary)
                    Button("Change…") { changingSecret = true }
                }
            }
            if let port = f.port, port > 0 {
                LabeledContent("Listening") {
                    Text("Port \(String(port))").foregroundStyle(.secondary)
                }
            }
            Button("Leave Fleet", role: .destructive) {
                model.configureFleet(["fleet": ""])
            }
        }
    }

    private func membersSection(_ f: FleetStatus) -> some View {
        Section {
            let members = f.members ?? []
            if members.isEmpty {
                Text("No other member found yet. A member is found while it is discoverable on this network.")
                    .foregroundStyle(.secondary)
            }
            ForEach(members) { m in
                FleetMemberRow(member: m)
            }
        } header: {
            Text("Members")
        } footer: {
            Text("A job this Mac cannot run, or cannot run now because it is busy, goes to a member that can and is free — generations and the helper's prompts alike.")
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Joining, making

    private func joinSection(_ f: FleetStatus) -> some View {
        Section {
            let seen = f.fleets ?? []
            if seen.isEmpty {
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Looking for fleets on this network…")
                        .foregroundStyle(.secondary)
                }
            }
            ForEach(seen) { s in
                HStack {
                    Image(systemName: "point.3.connected.trianglepath.dotted")
                        .foregroundStyle(.secondary)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: s.fleet)
                        Text(s.members == 1
                             ? String(localized: "1 discoverable member")
                             : String(localized: "\(String(s.members)) discoverable members"))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                    }
                    Spacer()
                    Button("Join…") { joining = s }
                }
            }
        } header: {
            Text("Join a Fleet")
        } footer: {
            Text("The fleets whose members are discoverable on this network. Joining takes the fleet's secret.")
                .foregroundStyle(.secondary)
        }
    }

    private var createSection: some View {
        Section {
            TextField("Fleet name", text: $newFleet)
            SecureField("Secret", text: $newSecret)
            Button("Create Fleet") {
                if model.configureFleet(["fleet": newFleet,
                                         "secret": newSecret,
                                         "discoverable": true]) {
                    newFleet = ""
                    newSecret = ""
                }
            }
            .disabled(newFleet.trimmingCharacters(in: .whitespaces).isEmpty
                      || newSecret.count < 8)
        } header: {
            Text("Create a Fleet")
        } footer: {
            Text("Every member shares the fleet's secret, at least 8 characters: it opens every connection between them. Valtz keeps a key made from it, never the secret itself.")
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Jobs

    private func jobsSection(_ c: FleetConfig) -> some View {
        Section {
            Picker("Take Fleet Jobs", selection: Binding(
                get: { c.accept },
                set: { model.configureFleet(["accept": $0]) })) {
                Text("Always").tag("always")
                Text("Ask Each Time").tag("ask")
                Text("Never").tag("never")
            }
            Toggle("Only at Set Times", isOn: Binding(
                get: { c.schedule.on },
                set: {
                    var s = c.schedule
                    s.on = $0
                    model.configureFleet(["schedule": s.json])
                }))
            .disabled(c.accept == "never")
            if c.schedule.on && c.accept != "never" {
                ScheduleRows(schedule: c.schedule) {
                    model.configureFleet(["schedule": $0.json])
                }
            }
        } header: {
            Text("Fleet Jobs")
        } footer: {
            Text(c.accepting
                 ? String(localized: "Taking jobs now. While this Mac runs one for a member, its window is given over to it until the job ends.")
                 : String(localized: "Not taking jobs now."))
                .foregroundStyle(.secondary)
        }
    }

    @ViewBuilder
    private func offered(_ f: FleetStatus) -> some View {
        if let me = f.me {
            Section("Offered to the Fleet") {
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
            }
        }
    }
}

/// A member: its name, whether it is reached and free, its Mac, what it
/// runs.
private struct FleetMemberRow: View {
    let member: FleetMember

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Image(systemName: "desktopcomputer")
                .foregroundStyle(member.state == "connected"
                                 ? Color.accentColor : .secondary)
                .frame(width: 22)
            VStack(alignment: .leading, spacing: 2) {
                Text(verbatim: member.name)
                Text(state)
                    .font(.caption)
                    .foregroundStyle(member.state == "refused"
                                     ? Color.red : .secondary)
                if let me = member.me, member.state == "connected" {
                    Text(machine(me))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    let f = (me.features ?? []).map { CapabilityStatus.localizedLabel($0) }
                    if !f.isEmpty {
                        Text(f.joined(separator: ", "))
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                            .lineLimit(2)
                    }
                }
            }
            Spacer()
        }
        .padding(.vertical, 2)
    }

    private var state: String {
        switch member.state {
        case "connected":
            guard let me = member.me else {
                return String(localized: "Connected")
            }
            if me.serving == true {
                return String(localized: "Connected · working on a fleet job")
            }
            if me.busy == true {
                return String(localized: "Connected · busy")
            }
            if me.accepting != true {
                return String(localized: "Connected · not taking jobs now")
            }
            return String(localized: "Connected · free")
        case "refused":
            return String(localized: "Not let in: it holds another secret")
        default:
            return String(localized: "Found · not reached yet")
        }
    }

    private func machine(_ me: FleetSelf) -> String {
        let chip = me.machine?.chip ?? ""
        let ram = me.machine?.ramGb.map { String($0) } ?? "?"
        return String(localized: "\(chip) · \(ram) GB · \(String((me.installed ?? []).count)) models")
    }
}

/// When jobs are taken: the days, and the hours of each.
private struct ScheduleRows: View {
    let schedule: FleetSchedule
    let set: (FleetSchedule) -> Void

    var body: some View {
        LabeledContent("Days") {
            HStack(spacing: 4) {
                ForEach(0..<7, id: \.self) { i in
                    let on = schedule.days & (1 << i) != 0
                    Button {
                        var s = schedule
                        s.days ^= 1 << i
                        set(s)
                    } label: {
                        Text(verbatim: Self.dayLetter(i))
                            .font(.caption.weight(.semibold))
                            .frame(width: 22, height: 22)
                            .background(Circle().fill(on ? Color.accentColor
                                                         : Color.secondary.opacity(0.15)))
                            .foregroundStyle(on ? Color(nsColor: .textBackgroundColor)
                                                : .primary)
                    }
                    .buttonStyle(.plain)
                    .help(Self.dayName(i))
                }
            }
        }
        DatePicker("From", selection: minutes(\.from),
                   displayedComponents: .hourAndMinute)
        DatePicker("To", selection: minutes(\.to),
                   displayedComponents: .hourAndMinute)
    }

    /// Minutes of the day as a time, today.
    private func minutes(_ k: WritableKeyPath<FleetSchedule, Int>)
        -> Binding<Date> {
        Binding(get: {
            Calendar.current.startOfDay(for: .now)
                .addingTimeInterval(Double(schedule[keyPath: k]) * 60)
        }, set: { d in
            let c = Calendar.current.dateComponents([.hour, .minute], from: d)
            var s = schedule
            s[keyPath: k] = (c.hour ?? 0) * 60 + (c.minute ?? 0)
            set(s)
        })
    }

    /// Monday first, in the UI's language.
    private static func dayLetter(_ i: Int) -> String {
        Calendar.current.veryShortWeekdaySymbols[(i + 1) % 7]
    }

    private static func dayName(_ i: Int) -> String {
        Calendar.current.weekdaySymbols[(i + 1) % 7]
    }
}

/// A fleet's secret asked for: to join it, or to change it.
private struct FleetSecretSheet: View {
    let title: String
    let action: String
    let note: String
    let done: (String) -> Void
    @State private var secret = ""
    @Environment(\.dismiss) private var dismiss

    init(title: String, action: String, note: String,
         done: @escaping (String) -> Void) {
        self.title = title
        self.action = action
        self.note = note
        self.done = done
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(verbatim: title).font(.headline)
            SecureField("Secret", text: $secret)
                .frame(width: 320)
                .onSubmit(go)
            Text(verbatim: note)
                .font(.callout)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
                .frame(width: 320, alignment: .leading)
            HStack {
                Spacer()
                Button("Cancel", role: .cancel) { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button(action, action: go)
                    .keyboardShortcut(.defaultAction)
                    .disabled(secret.count < 8)
            }
        }
        .padding(20)
    }

    private func go() {
        guard secret.count >= 8 else { return }
        done(secret)
        secret = ""
        dismiss()
    }
}
