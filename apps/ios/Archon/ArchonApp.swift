import SwiftUI

@main
struct ArchonApp: App {
    @StateObject private var model = AppModel()
    @Environment(\.scenePhase) private var phase

    var body: some Scene {
        WindowGroup {
            RootView()
                .environmentObject(model)
                .environmentObject(model.discovery)
                .preferredColorScheme(.dark)
                .tint(Archon.accent)
        }
        .onChange(of: phase) { p in
            // Nothing runs in the background: no battery or data use while hidden.
            if p == .active { model.foreground() } else if p == .background { model.background() }
        }
    }
}

struct RootView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduce

    var body: some View {
        ZStack {
            Archon.bg.ignoresSafeArea()
            switch model.screen {
            case .computers: ComputersView().transition(.opacity)
            case .pairing(let t): PairingView(target: t).transition(.opacity)
            case .mixer(let t): MixerView(target: t).transition(.opacity)
            }
        }
        .animation(reduce ? nil : .timingCurve(0.2, 0, 0, 1, duration: 0.3), value: model.screen)
    }
}
