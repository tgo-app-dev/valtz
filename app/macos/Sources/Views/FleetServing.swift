import SwiftUI

/// The window while this Mac works on a FLEET JOB (DESIGN §11): given
/// over to it. Its own controls are out of reach under a veil; the mark
/// turns blue, a light passing over it every five seconds; under it, who
/// the job is for, what it makes, and its progress -- a bar and the
/// phase, no preview (the picture is the member's). It goes when the job
/// ends.
struct FleetServingOverlay: View {
    let serving: FleetServing
    @State private var sweeps = 0

    var body: some View {
        ZStack {
            // A veil: the window's controls still seen, out of reach.
            Rectangle()
                .fill(Color(nsColor: .windowBackgroundColor).opacity(0.82))
                .ignoresSafeArea()
            VStack(spacing: 16) {
                MarkView(animating: false, sweep: sweeps, blue: true)
                    .frame(width: 200, height: 200)
                Text("Working on a fleet job for \(serving.from)")
                    .font(.title3.weight(.semibold))
                if !serving.title.isEmpty {
                    Text(verbatim: serving.title)
                        .foregroundStyle(.secondary)
                        .lineLimit(2)
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 420)
                }
                VStack(spacing: 6) {
                    if let p = serving.progress, p > 0 {
                        ProgressView(value: min(1, p))
                    } else {
                        ProgressView().progressViewStyle(.linear)
                    }
                    Text(caption)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .monospacedDigit()
                }
                .frame(width: 320)
                Text("This Mac's window is back when the job ends.")
                    .font(.footnote)
                    .foregroundStyle(.tertiary)
            }
            .padding(40)
        }
        // Every click is the overlay's: the window's controls are out of
        // reach while it shows.
        .contentShape(Rectangle())
        .onTapGesture {}
        .task {
            // The light at once, then every five seconds.
            sweeps += 1
            while !Task.isCancelled {
                try? await Task.sleep(for: .seconds(5))
                sweeps += 1
            }
        }
        .accessibilityElement(children: .combine)
    }

    /// The phase as a generation's progress words it ("Generating ·
    /// 47%"); before a count, "Preparing…".
    private var caption: String {
        let kind: Modality = serving.op == "generate-video" ? .video
            : serving.op == "generate-audio" || serving.op == "generate-speech"
                ? .audio : .image
        if let ph = serving.phase {
            return ph.caption(kind: kind)
        }
        return String(localized: "Preparing…")
    }
}

extension View {
    /// A member's offer, asked of the person ("Ask Each Time"): accepted
    /// or declined -- unanswered, the member hears no after a minute.
    func fleetAskAlert(_ model: AppModel) -> some View {
        let ask = model.fleetAsks.first
        return alert(
            ask.map { String(localized: "\($0.from) asks this Mac to run a job") }
                ?? "",
            isPresented: Binding(
                get: { model.fleetAsks.first != nil },
                set: { if !$0, let a = model.fleetAsks.first {
                    model.fleetAsks.removeAll { $0.job == a.job }
                } }),
            presenting: ask
        ) { a in
            Button("Accept") { model.answerFleet(a, accept: true) }
                .keyboardShortcut(.defaultAction)
            Button("Decline", role: .cancel) {
                model.answerFleet(a, accept: false)
            }
        } message: { a in
            Text(a.title.isEmpty
                 ? String(localized: "While it runs, this Mac's window is given over to it.")
                 : String(localized: "“\(a.title)”. While it runs, this Mac's window is given over to it."))
        }
    }
}
