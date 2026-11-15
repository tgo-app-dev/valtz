import SwiftUI

/// Settings › Fleet (DESIGN §11): this Mac as a member of a FLEET -- the
/// Valtz Macs of one network working as one. Its name among them,
/// whether it is discoverable; the fleet it is in (its secret changed,
/// left), or one found on the network joined, or one made; whether and
/// when it takes the others' jobs; the members, each with what it runs
/// and whether it is free; and, on pages under it, where this Mac's own
/// jobs run (Offload Policies) and what it offers them (Shared
/// Capabilities). While the page is open the network is looked over for
/// fleets to join.
struct FleetView: View {
    @Bindable var model: AppModel
    /// A page under this one opened (SettingsView's history).
    let open: (SettingsPlace) -> Void
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
                    thunderboltSection(f)
                } else {
                    joinSection(f)
                    createSection
                }
                jobsSection(c)
                pagesSection(f)
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
                .settingsMark(String(localized: "Name"))
            Toggle("Discoverable", isOn: Binding(
                get: { c.discoverable },
                set: { model.configureFleet(["discoverable": $0]) }))
                .settingsMark(String(localized: "Discoverable"))
        } header: {
            Text("This Mac")
                .settingsMark(String(localized: "This Mac"))
        } footer: {
            Text("Discoverable, this Mac can be seen on the network by its fleet's members, who send it jobs, and by Macs looking for a fleet to join. Not discoverable, it can still send its own jobs to the members it finds.")
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Its fleet

    private func fleetSection(_ f: FleetStatus, _ c: FleetConfig) -> some View {
        Section("Fleet") {
            LabeledContent("Fleet") { Text(verbatim: c.fleet) }
                .settingsMark(String(localized: "Fleet"))
            LabeledContent("Secret") {
                HStack {
                    Text("Set").foregroundStyle(.secondary)
                    Button("Change…") { changingSecret = true }
                }
            }
            .settingsMark(String(localized: "Secret"))
            if let port = f.port, port > 0 {
                LabeledContent("Listening") {
                    Text("Port \(String(port))").foregroundStyle(.secondary)
                }
                .settingsMark(String(localized: "Listening"))
            }
            Button("Leave Fleet", role: .destructive) {
                model.configureFleet(["fleet": ""])
            }
            .settingsMark(String(localized: "Leave Fleet"))
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
                FleetMemberRow(
                    member: m,
                    cables: f.thunderbolt.links[m.id] ?? [],
                    paired: f.pair?.member == m.id,
                    canSplit: f.thunderbolt.splits) {
                    model.pairFleet(f.pair?.member == m.id ? "" : m.id)
                }
            }
        } header: {
            Text("Members")
                .settingsMark(String(localized: "Members"))
        } footer: {
            Text("By default a job this Mac cannot run, or cannot run now because it is busy, goes to a member that can and is free — generations and the helper's prompts alike. Offload Policies set it task by task.")
                .foregroundStyle(.secondary)
        }
    }

    // MARK: Thunderbolt (DESIGN §11c)

    /// This Mac's Thunderbolt ports, and the member it is paired with: a
    /// MiniMax H3 clip that may be split runs half there, over the cable.
    @ViewBuilder
    private func thunderboltSection(_ f: FleetStatus) -> some View {
        let tb = f.thunderbolt
        if !tb.ports.isEmpty || f.pair != nil {
            Section {
                LabeledContent("Ports") {
                    Text(portsLine(tb))
                        .foregroundStyle(.secondary)
                        .multilineTextAlignment(.trailing)
                }
                .settingsMark(String(localized: "Thunderbolt"))
                if !tb.bridge.isEmpty {
                    LabeledContent("Thunderbolt Bridge") {
                        Text(verbatim: "\(tb.bridgeAddress)")
                            .foregroundStyle(.secondary)
                    }
                }
                LabeledContent("RDMA") {
                    Text(tb.rdma ? String(localized: "Enabled")
                                 : String(localized: "Not enabled"))
                        .foregroundStyle(.secondary)
                }
                .settingsMark(String(localized: "RDMA"))
                if let p = f.pair {
                    pairRows(p)
                }
            } header: {
                Text("Thunderbolt")
                    .settingsMark(String(localized: "Thunderbolt"))
            } footer: {
                Text(tb.splits
                     ? String(localized: "Pair this Mac with a member a Thunderbolt cable joins it to: a MiniMax H3 clip made without a LoRA or the Neural Engine (Favor’s Fine, or Custom) is then split between the two — each holds part of every block, their partial sums crossing the cable, RDMA where both Macs have it enabled (rdma_ctl, from recoveryOS). Other clips run as before.")
                     : String(localized: "This Valtz cannot split a DiT: its vpipe has no tensor parallelism."))
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private func pairRows(_ p: FleetPair) -> some View {
        LabeledContent("Paired With") {
            VStack(alignment: .trailing, spacing: 2) {
                Text(verbatim: p.name)
                Text(pairState(p))
                    .font(.caption)
                    .foregroundStyle(p.ready ? Color.secondary : Color.orange)
                    .multilineTextAlignment(.trailing)
            }
        }
        .settingsMark(String(localized: "Paired With"))
        Toggle("Over RDMA", isOn: Binding(
            get: { p.useRdma },
            set: { model.setPair(rdma: $0) }))
            .settingsMark(String(localized: "Over RDMA"))
        Toggle("Share by GPU", isOn: Binding(
            get: { p.autoShare },
            set: { model.setPair(share: $0 ? 0 : max(0.05, p.share)) }))
            .settingsMark(String(localized: "Share by GPU"))
        if !p.autoShare {
            LabeledContent("This Mac's Share") {
                HStack {
                    Slider(value: Binding(
                        get: { p.setShare },
                        set: { model.setPair(share: ($0 * 100).rounded() / 100) }),
                           in: 0.05...0.95)
                        .frame(width: 160)
                    Text(verbatim: "\(Int((p.setShare * 100).rounded()))%")
                        .monospacedDigit()
                        .frame(width: 40, alignment: .trailing)
                }
            }
        }
        if p.heads.count == 2 {
            LabeledContent("Heads") {
                Text("This Mac \(String(p.heads[0])) · \(p.name) \(String(p.heads[1]))")
                    .foregroundStyle(.secondary)
            }
        }
        Button("Unpair") { model.pairFleet("") }
            .settingsMark(String(localized: "Unpair"))
    }

    private func pairState(_ p: FleetPair) -> String {
        if p.ready {
            let over = p.rdma ? String(localized: "RDMA") : String(localized: "TCP")
            return p.links == 1
                ? String(localized: "Clips split over 1 cable, \(over)")
                : String(localized: "Clips split over \(String(p.links)) cables, \(over)")
        }
        return p.why.isEmpty ? String(localized: "Not now") : p.why
    }

    /// "en1 (cable, RDMA) · en2 · en6".
    private func portsLine(_ tb: FleetThunderbolt) -> String {
        if tb.ports.isEmpty { return String(localized: "None") }
        return tb.ports.map { p in
            var notes: [String] = []
            if p.active { notes.append(String(localized: "cable")) }
            if !p.rdma.isEmpty { notes.append(String(localized: "RDMA")) }
            if !p.address.isEmpty { notes.append(p.address) }
            return notes.isEmpty ? p.name
                : "\(p.name) (\(notes.joined(separator: ", ")))"
        }.joined(separator: " · ")
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
                .settingsMark(String(localized: "Join a Fleet"))
        } footer: {
            Text("The fleets whose members are discoverable on this network. Joining takes the fleet's secret.")
                .foregroundStyle(.secondary)
        }
    }

    private var createSection: some View {
        Section {
            TextField("Fleet name", text: $newFleet)
                .settingsMark(String(localized: "Fleet name"))
            SecureField("Secret", text: $newSecret)
                .settingsMark(String(localized: "Secret"))
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
                .settingsMark(String(localized: "Create a Fleet"))
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
            .settingsMark(String(localized: "Take Fleet Jobs"))
            Toggle("Only at Set Times", isOn: Binding(
                get: { c.schedule.on },
                set: {
                    var s = c.schedule
                    s.on = $0
                    model.configureFleet(["schedule": s.json])
                }))
            .disabled(c.accept == "never")
            .settingsMark(String(localized: "Only at Set Times"))
            if c.schedule.on && c.accept != "never" {
                ScheduleRows(schedule: c.schedule) {
                    model.configureFleet(["schedule": $0.json])
                }
            }
        } header: {
            Text("Fleet Jobs")
                .settingsMark(String(localized: "Fleet Jobs"))
        } footer: {
            Text(c.accepting
                 ? String(localized: "Taking jobs now. While this Mac runs one for a member, its window is given over to it until the job ends.")
                 : String(localized: "Not taking jobs now."))
                .foregroundStyle(.secondary)
        }
    }

    // MARK: The pages under it

    private func pagesSection(_ f: FleetStatus) -> some View {
        let p = f.policies
        let set = p.tasks.count + p.models.count
        let held = p.withheldTasks.count + p.withheldModels.count
        return Section {
            SettingsLinkRow(
                title: SettingsPlace.fleetOffload.title,
                detail: set == 0 ? FleetOffload.localFirst.title
                                 : String(localized: "Custom"),
                symbol: "arrow.triangle.branch", color: .orange) {
                open(.fleetOffload)
            }
            .settingsMark(SettingsPlace.fleetOffload.title)
            SettingsLinkRow(
                title: SettingsPlace.fleetSharing.title,
                detail: held == 0 ? String(localized: "Everything")
                    : String(localized: "\(String(held)) withheld"),
                symbol: "square.and.arrow.up", color: .green) {
                open(.fleetSharing)
            }
            .settingsMark(SettingsPlace.fleetSharing.title)
        } footer: {
            Text("Where this Mac's own jobs run, task by task and model by model; and what it offers the other members.")
                .foregroundStyle(.secondary)
        }
    }
}

/// A member: its name, whether it is reached and free, its Mac, what it
/// runs.
private struct FleetMemberRow: View {
    let member: FleetMember
    /// The Thunderbolt cables to it (DESIGN §11c).
    var cables: [FleetThunderbolt.Link] = []
    var paired = false
    /// This Mac may split a DiT.
    var canSplit = false
    var pair: () -> Void = {}

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
                if !cables.isEmpty {
                    Label(cableLine, systemImage: "bolt.horizontal")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            Spacer()
            if paired {
                Button("Unpair", action: pair)
                    .controlSize(.small)
            } else if !cables.isEmpty && canSplit && member.me?.splits == true {
                Button("Pair", action: pair)
                    .controlSize(.small)
                    .help(String(localized: "Split MiniMax H3 clips with this Mac over the Thunderbolt cable"))
            }
        }
        .padding(.vertical, 2)
    }

    /// "Thunderbolt · en1 ↔ en6 · RDMA · 0.4 ms".
    private var cableLine: String {
        let ports = cables.map { c in
            c.localPort.isEmpty || c.remotePort.isEmpty
                ? c.remoteAddress : "\(c.localPort) ↔ \(c.remotePort)"
        }.joined(separator: ", ")
        let rdma = cables.contains { $0.rdma }
        let ms = cables.map(\.rttMs).min() ?? 0
        let n = cables.count == 1 ? String(localized: "Thunderbolt")
            : String(localized: "Thunderbolt × \(String(cables.count))")
        return [n, ports, rdma ? String(localized: "RDMA") : nil,
                String(format: "%.1f ms", ms)]
            .compactMap { $0 }.joined(separator: " · ")
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
