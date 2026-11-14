import AppIntents
import Foundation
import UIKit

// Panic mute from outside the app: Shortcuts, Siri, Back Tap
// (Settings › Accessibility › Touch › Back Tap › choose the shortcut).
// Runs without opening the app: connects to the last PC used, sends the
// command, disconnects.

struct PanicMuteIntent: AppIntent {
    static var title: LocalizedStringResource = "Silenciar tudo no PC"
    static var description = IntentDescription("Silencia todas as saídas e microfones do último PC usado no Archon.")
    static var openAppWhenRun = false

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        switch await PanicRunner.run(on: true) {
        case .done: return .result(dialog: "Tudo silenciado no PC.")
        case .unsupported: return .result(dialog: "Atualize o Archon no PC para usar este atalho.")
        case .failed: return .result(dialog: "Não foi possível falar com o PC. Ele precisa estar ligado, na mesma rede.")
        }
    }
}

struct PanicRestoreIntent: AppIntent {
    static var title: LocalizedStringResource = "Reativar o som do PC"
    static var description = IntentDescription("Desfaz o “Silenciar tudo no PC”.")
    static var openAppWhenRun = false
    // Turning the PC's microphone back on must not work from a locked phone.
    static var authenticationPolicy: IntentAuthenticationPolicy = .requiresAuthentication

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        switch await PanicRunner.run(on: false) {
        case .done: return .result(dialog: "Som do PC reativado.")
        case .unsupported: return .result(dialog: "Atualize o Archon no PC para usar este atalho.")
        case .failed: return .result(dialog: "Não foi possível falar com o PC. Ele precisa estar ligado, na mesma rede.")
        }
    }
}

struct ArchonShortcuts: AppShortcutsProvider {
    static var appShortcuts: [AppShortcut] {
        AppShortcut(intent: PanicMuteIntent(),
                    phrases: ["Silenciar o PC no \(.applicationName)", "Mute the PC with \(.applicationName)"])
        AppShortcut(intent: PanicRestoreIntent(),
                    phrases: ["Reativar o som do PC no \(.applicationName)", "Unmute the PC with \(.applicationName)"])
    }
}

enum PanicOutcome { case done, unsupported, failed }

@MainActor
enum PanicRunner {
    static let lastPCKey = "lastPC"

    static func run(on: Bool) async -> PanicOutcome {
        let store = PairingStore()
        let all = store.all()
        guard !all.isEmpty else { return .failed }
        let last = UserDefaults.standard.string(forKey: lastPCKey)
        let pc = all.first { $0.agentId == last } ?? all[0]
        let session = PanicSession()
        return await session.run(pc: pc, controllerId: store.controllerId, on: on)
    }
}

/// One short connection: resume, wait for the PC's state (AGENT_INFO), send
/// PANIC_MUTE if needed, and report success only when the PC confirms it.
@MainActor
final class PanicSession {
    private var client: CoreClient?
    private var cont: CheckedContinuation<PanicOutcome, Never>?
    private var want = true
    private var sent = false

    func run(pc: PairedPC, controllerId: Data, on: Bool) async -> PanicOutcome {
        want = on
        return await withCheckedContinuation { c in
            cont = c
            let client = CoreClient(controllerId: controllerId, name: UIDevice.current.name) { [weak self] ev in
                let type = ev.type, status = Int(ev.status.rawValue), features = ev.features, panic = ev.panic
                Task { @MainActor in self?.handle(type: type, status: status, features: features, panic: panic) }
            }
            self.client = client
            client.resume(host: pc.host, port: pc.port, record: pc.record)
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: 6_000_000_000)
                self?.finish(.failed)
            }
        }
    }

    private func handle(type: arc_event_type, status: Int, features: UInt32, panic: Bool) {
        if type == ARC_EV_STATUS && status == LinkStatus.failed.rawValue { finish(.failed); return }
        guard type == ARC_EV_AGENT_INFO else { return }
        if features & CoreClient.featurePanic == 0 { finish(.unsupported); return }
        if panic == want { finish(.done); return }  // confirmed by the PC (or already so)
        if !sent {
            sent = true
            client?.panicMute(want)  // the link is up: sent now; the PC answers with AGENT_INFO
        }
    }

    private func finish(_ outcome: PanicOutcome) {
        guard let c = cont else { return }
        cont = nil
        client?.disconnect()
        client = nil  // joins the network thread: safe, we are on the main actor
        c.resume(returning: outcome)
    }
}
