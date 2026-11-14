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
        let ok = await PanicRunner.run(on: true)
        return ok ? .result(dialog: "Tudo silenciado no PC.")
                  : .result(dialog: "Não foi possível falar com o PC. Ele precisa estar ligado, na mesma rede.")
    }
}

struct PanicRestoreIntent: AppIntent {
    static var title: LocalizedStringResource = "Reativar o som do PC"
    static var description = IntentDescription("Desfaz o “Silenciar tudo no PC”.")
    static var openAppWhenRun = false

    @MainActor
    func perform() async throws -> some IntentResult & ProvidesDialog {
        let ok = await PanicRunner.run(on: false)
        return ok ? .result(dialog: "Som do PC reativado.")
                  : .result(dialog: "Não foi possível falar com o PC. Ele precisa estar ligado, na mesma rede.")
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

@MainActor
enum PanicRunner {
    static let lastPCKey = "lastPC"

    static func run(on: Bool) async -> Bool {
        let store = PairingStore()
        let all = store.all()
        guard !all.isEmpty else { return false }
        let last = UserDefaults.standard.string(forKey: lastPCKey)
        let pc = all.first { $0.agentId == last } ?? all[0]
        let session = PanicSession()
        return await session.run(pc: pc, controllerId: store.controllerId, on: on)
    }
}

/// One short connection: resume, send PANIC_MUTE, disconnect.
@MainActor
final class PanicSession {
    private var client: CoreClient?
    private var cont: CheckedContinuation<Bool, Never>?

    func run(pc: PairedPC, controllerId: Data, on: Bool) async -> Bool {
        await withCheckedContinuation { c in
            cont = c
            let client = CoreClient(controllerId: controllerId, name: UIDevice.current.name) { [weak self] ev in
                guard ev.type == ARC_EV_STATUS else { return }
                let status = Int(ev.status.rawValue)
                Task { @MainActor in self?.on(status: status) }
            }
            self.client = client
            client.resume(host: pc.host, port: pc.port, record: pc.record)
            client.panicMute(on)  // queued until the secure channel is up
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: 6_000_000_000)
                self?.finish(false)
            }
        }
    }

    private func on(status: Int) {
        if status == LinkStatus.connected.rawValue {
            Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: 500_000_000)  // let the command leave
                self?.finish(true)
            }
        } else if status == LinkStatus.failed.rawValue {
            finish(false)
        }
    }

    private func finish(_ ok: Bool) {
        guard let c = cont else { return }
        cont = nil
        client?.disconnect()
        client = nil  // joins the network thread: safe, we are on the main actor
        c.resume(returning: ok)
    }
}
