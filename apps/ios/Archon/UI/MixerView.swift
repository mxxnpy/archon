import SwiftUI
import UIKit

struct MixerView: View {
    let target: Target
    @EnvironmentObject var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduce
    @State private var confirmForget = false

    var body: some View {
        VStack(spacing: 0) {
            topBar
            if model.status == .failed || !model.synced {
                waiting
            } else {
                VStack(spacing: 0) {
                ScrollView {
                    VStack(spacing: 12) {
                        if model.panic { PanicBanner() }
                        if model.listening { ListeningBanner() }
                        if let err = model.listenError {
                            Text(ListenText.of(err)).font(.archonCaption).foregroundStyle(Archon.warn)
                                .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 4)
                                .onTapGesture { model.listenError = nil }
                        }
                        if let e = model.currentEndpoint { MasterArc(endpoint: e) }
                        LazyVGrid(columns: [GridItem(.flexible(), spacing: 12), GridItem(.flexible(), spacing: 12)],
                                  spacing: 12) {
                            if let mic = model.microphone {
                                MicCard(endpoint: mic, level: model.levels[mic.meter] ?? 0).appear(0)
                            }
                            ForEach(Array(model.currentSources.enumerated()), id: \.element.id) { i, s in
                                AppCard(source: s, icon: model.icons[s.iconKey], level: model.levels[s.meter] ?? 0)
                                    .appear(i + 1)
                            }
                        }
                        if model.currentSources.isEmpty {
                            Text("Nenhum app usando este dispositivo agora.").font(.archonCaption)
                                .foregroundStyle(Archon.muted).padding(24)
                        }
                    }
                    .padding(.horizontal, 16).padding(.bottom, 24)
                }
                .opacity(model.status == .connected ? 1 : 0.5)
                .allowsHitTesting(model.status == .connected)
                .accessibilityHidden(model.status != .connected)  // frozen for VoiceOver too
                if model.status != .connected {
                    StatusLine(color: Archon.warn, text: "Reconectando…")
                        .padding(.horizontal, 16).padding(.vertical, 10)
                        .background(Archon.raised, in: Capsule())
                        .padding(.vertical, 12)  // own row: never covers the arc or a card
                }
                }
            }
        }
        .onChange(of: model.status) { _ in
            UIAccessibility.post(notification: .announcement, argument: linkSpoken)
        }
        // Live levels only while the mixer is visible, and never with Reduce Motion / Low Power.
        .onAppear { model.setMetersVisible(!reduce && !ProcessInfo.processInfo.isLowPowerModeEnabled) }
        .onDisappear { model.setMetersVisible(false) }
        .confirmationDialog("Esquecer computador", isPresented: $confirmForget, titleVisibility: .visible) {
            Button("Esquecer", role: .destructive) { model.forget(target) }
            Button("Cancelar", role: .cancel) {}
        } message: {
            Text("\(target.name) vai precisar de um novo código para parear. Para revogar também no PC, use Dispositivos pareados na bandeja.")
        }
    }

    private var topBar: some View {
        HStack(spacing: 4) {
            Button { model.back() } label: {
                Image(systemName: "chevron.left").font(.system(size: 18, weight: .semibold))
                    .foregroundStyle(Archon.paper).frame(width: 44, height: 44)
            }
            .accessibilityLabel("Voltar")
            Spacer(minLength: 0)
            Menu {
                ForEach(model.outputs) { e in
                    Button { model.selectedEndpoint = e.id } label: {
                        if e.id == model.currentEndpoint?.id { Label(e.name, systemImage: "checkmark") } else { Text(e.name) }
                    }
                }
            } label: {
                HStack(spacing: 4) {
                    Text(model.currentEndpoint?.name ?? target.name).font(.archonCaption).foregroundStyle(Archon.paper)
                        .lineLimit(1)
                    if model.outputs.count > 1 {
                        Image(systemName: "chevron.down").font(.system(size: 11, weight: .semibold)).foregroundStyle(Archon.muted)
                    }
                }
                .padding(.horizontal, 14).frame(minHeight: 44)  // 44 pt touch target
                .background(Archon.surface, in: Capsule())
            }
            .disabled(model.outputs.count < 2)
            .accessibilityLabel("Escolher dispositivo de saída")
            Spacer(minLength: 0)
            Dot(color: linkColor).frame(width: 44, height: 44)
                .accessibilityLabel(Text(linkText))
            if model.canListen {
                Button { model.toggleListening() } label: {
                    Image(systemName: model.listening ? "headphones.circle.fill" : "headphones.circle")
                        .font(.system(size: 22, weight: .regular))
                        .foregroundStyle(model.listening ? Archon.accent : Archon.paper)
                        .frame(width: 44, height: 44)
                }
                .accessibilityLabel(model.listening ? "Parar de ouvir no celular" : "Ouvir o PC no celular")
                .disabled(model.status != .connected && !model.listening)
                .opacity(model.status == .connected || model.listening ? 1 : 0.4)
            }
            if model.canPanic {
                Button { model.togglePanic() } label: {
                    Image(systemName: model.panic ? "speaker.slash.circle.fill" : "speaker.slash.circle")
                        .font(.system(size: 22, weight: .regular))
                        .foregroundStyle(model.panic ? Archon.error : Archon.paper)
                        .frame(width: 44, height: 44)
                }
                .accessibilityLabel(model.panic ? "Reativar o som do PC" : "Silenciar tudo no PC")
                .disabled(model.status != .connected)  // never queue a stale toggle while away
                .opacity(model.status == .connected ? 1 : 0.4)
            }
            Menu {
                Button("Esquecer computador", role: .destructive) { confirmForget = true }
            } label: {
                Image(systemName: "ellipsis").foregroundStyle(Archon.paper).frame(width: 44, height: 44)
            }
            .accessibilityLabel("Mais opções")
        }
        .padding(.horizontal, 4)
    }

    private var linkColor: Color {
        switch model.status {
        case .connected: return Archon.ok
        case .failed, .idle: return Archon.error
        default: return Archon.warn
        }
    }

    private var linkText: LocalizedStringKey {
        switch model.status {
        case .connected: return "Conectado"
        case .failed, .idle: return "Desconectado"
        default: return "Reconectando…"
        }
    }

    private var linkSpoken: String {
        switch model.status {
        case .connected: return String(localized: "Conectado")
        case .failed, .idle: return String(localized: "Desconectado")
        default: return String(localized: "Reconectando…")
        }
    }

    private var waiting: some View {
        VStack(spacing: 20) {
            Spacer()
            VolumeArc(fraction: 0, lineWidth: 8, color: Archon.edge).frame(width: 180, height: 180)
            if model.status == .failed {
                Text(ErrorText.of(model.error)).font(.archonBody).foregroundStyle(Archon.paper).multilineTextAlignment(.center)
                Button { model.reconnect() } label: {
                    Text("Tentar de novo").font(.archonBody).foregroundStyle(Archon.ink)
                        .padding(.horizontal, 28).frame(minHeight: 48).background(Archon.accent, in: Capsule())
                }.buttonStyle(.plain)
                if [CoreErr.notRecognized, CoreErr.unknown, CoreErr.authFailed].contains(model.error) {
                    Button("Parear de novo") { model.repair(target) }
                        .font(.archonBody).foregroundStyle(Archon.accent).frame(minHeight: 44)
                }
            } else {
                StatusLine(color: Archon.warn, text: "Conectando…")
            }
            Spacer()
        }
        .padding(24)
    }
}

/// The finger always wins: local value while dragging, PC echoes ignored for
/// 400 ms after release; remote changes animate over 240 ms otherwise.
@MainActor
final class DragState: ObservableObject {
    @Published var dragging = false
    @Published var holding = false
    @Published var local: Double = 0
    var start: Double = 0
    var lastTick = -1
    private var holdTask: Task<Void, Never>?

    func shown(remote: Int) -> Double {
        dragging || holding ? local : Double(remote) / Double(CoreClient.maxVolume)
    }

    func begin(remote: Int) {
        holdTask?.cancel()
        start = Double(remote) / Double(CoreClient.maxVolume)
        local = start
        dragging = true
        lastTick = Int(local * 10)
    }

    /// Relative drag: tapping never jumps the volume. Returns the new 0..10000 value.
    func move(delta: Double, fullRange: Double) -> Int {
        local = min(max(start + delta / fullRange, 0), 1)
        let tick = Int(local * 10)
        if tick != lastTick {
            lastTick = tick
            if local <= 0 || local >= 1 { UIImpactFeedbackGenerator(style: .medium).impactOccurred() }
            else { UIImpactFeedbackGenerator(style: .light).impactOccurred(intensity: 0.6) }
        }
        return Int((local * Double(CoreClient.maxVolume)).rounded())
    }

    func end() {
        dragging = false
        holding = true
        holdTask = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(Motion.echoHold * 1_000_000_000))
            self?.holding = false
        }
    }
}

struct MasterArc: View {
    let endpoint: Endpoint
    @ScaledMetric(relativeTo: .largeTitle) private var masterSize: CGFloat = 64
    @EnvironmentObject var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduce
    @StateObject private var drag = DragState()

    var body: some View {
        let shown = drag.shown(remote: endpoint.volume)
        let pct = Int((shown * 100).rounded())
        VStack(spacing: 4) {
            ZStack {
                VolumeArc(fraction: shown, lineWidth: 8, color: endpoint.muted ? Archon.edge : Archon.accent, thumbRadius: 9)
                    .animation(drag.dragging || reduce ? nil : Motion.arc, value: shown)
                HStack(alignment: .lastTextBaseline, spacing: 0) {
                    Text("\(pct)").font(.system(size: min(masterSize, 64 * 1.3), weight: .semibold).monospacedDigit())
                        .foregroundStyle(endpoint.muted ? Archon.muted : Archon.paper)
                    Text("%").font(.archonBody).foregroundStyle(Archon.muted)
                }
            }
            .frame(width: 220, height: 220)
            .contentShape(Circle())
            .gesture(
                DragGesture(minimumDistance: 4)
                    .onChanged { g in
                        if !drag.dragging { drag.begin(remote: endpoint.volume) }
                        let v = drag.move(delta: g.translation.width - g.translation.height, fullRange: 260)
                        model.setVolume(endpoint: true, id: endpoint.id, volume: v)
                    }
                    .onEnded { _ in drag.end() }
            )
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("Volume geral")
            .accessibilityValue("\(pct)%")
            .accessibilityAdjustableAction { dir in
                let step = CoreClient.maxVolume / 20
                let v = endpoint.volume + (dir == .increment ? step : -step)
                model.setVolume(endpoint: true, id: endpoint.id, volume: min(max(v, 0), CoreClient.maxVolume))
            }
            MuteButton(label: String(localized: "Volume geral"), muted: endpoint.muted, visual: 40) {
                model.setMute(endpoint: true, id: endpoint.id, muted: !endpoint.muted)
            }
        }
        .padding(.vertical, 8)
    }
}

struct AppCard: View {
    let source: Source
    private var routedName: String? {
        guard !source.route.isEmpty else { return nil }
        return model.endpoints[source.route]?.name
    }

    private func spoken(_ name: String) -> String {
        var s = name
        if source.playing { s += String(localized: ", tocando") }
        if source.muted { s += String(localized: ", mudo") }
        if let out = routedName { s += String(localized: ", saída ") + out }
        return s
    }
    @ScaledMetric(relativeTo: .title) private var pctSize: CGFloat = 26
    let icon: UIImage?
    let level: Double
    @EnvironmentObject var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduce
    @StateObject private var drag = DragState()

    var body: some View {
        let name = source.system ? String(localized: "Sons do sistema") : source.name
        let shown = drag.shown(remote: source.volume)
        let pct = Int((shown * 100).rounded())
        let active = drag.dragging || drag.holding
        ZStack(alignment: .topTrailing) {
            VStack(spacing: 10) {
                ZStack {
                    VolumeArc(fraction: shown, lineWidth: active ? 6 : 4,
                              color: source.muted ? Archon.edge : Archon.accent, thumbRadius: active ? 7 : 0)
                        .animation(drag.dragging || reduce ? nil : Motion.arc, value: shown)
                    if active {
                        Text("\(pct)").font(.system(size: min(pctSize, 26 * 1.3), weight: .semibold).monospacedDigit())
                            .foregroundStyle(Archon.paper)
                    } else {
                        ZStack {
                            RoundedRectangle(cornerRadius: 14).fill(Archon.raised)
                            if let icon {
                                Image(uiImage: icon).resizable().interpolation(.high).frame(width: 32, height: 32)
                                    .opacity(source.muted ? 0.45 : 1)
                            } else {
                                Text(String(name.prefix(1)).uppercased()).font(.archonBody).foregroundStyle(Archon.muted)
                            }
                        }
                        .frame(width: 48, height: 48)
                    }
                }
                .frame(width: 96, height: 96)
                HStack(spacing: 6) {
                    if source.playing && !source.muted {
                        // Opacity follows the live peak, smoothed into 55–100 %.
                        Dot(color: Archon.ok, opacity: reduce ? 1 : 0.55 + 0.45 * min(max(level, 0), 1))
                            .animation(.linear(duration: 0.05), value: level)
                    }
                    Text(name).font(.archonCaption).foregroundStyle(source.muted ? Archon.muted : Archon.paper).lineLimit(1)
                }
                .padding(.horizontal, 14)
                if let out = routedName {
                    // Pinned to a specific output (long-press to change).
                    Text("→ \(out)").font(.archonCaption).foregroundStyle(Archon.muted).lineLimit(1)
                        .padding(.horizontal, 14).padding(.top, -6)
                }
            }
            .frame(maxWidth: .infinity)
            .padding(.top, 16).padding(.bottom, 18)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(spoken(name))
            .accessibilityValue("\(pct)%")
            .accessibilityAdjustableAction { dir in
                let step = CoreClient.maxVolume / 20
                let v = source.volume + (dir == .increment ? step : -step)
                model.setVolume(endpoint: false, id: source.id, volume: min(max(v, 0), CoreClient.maxVolume))
            }
            MuteButton(label: name, muted: source.muted) {
                model.setMute(endpoint: false, id: source.id, muted: !source.muted)
            }
            .padding(4)
        }
        .frame(maxHeight: .infinity, alignment: .top)  // same height and top as its row neighbour
        .background(Archon.surface, in: RoundedRectangle(cornerRadius: 24))
        .contentShape(RoundedRectangle(cornerRadius: 24))
        .contextMenu {
            if model.canRoute && !source.system {
                Section("Saída deste app") {
                    Button { model.setRoute(source: source.id, endpoint: "") } label: {
                        if source.route.isEmpty { Label("Padrão do PC", systemImage: "checkmark") } else { Text("Padrão do PC") }
                    }
                    ForEach(model.outputs) { e in
                        Button { model.setRoute(source: source.id, endpoint: e.id) } label: {
                            if source.route == e.id { Label(e.name, systemImage: "checkmark") } else { Text(e.name) }
                        }
                    }
                }
            }
        }
        // Horizontal drag changes the volume; vertical drags keep scrolling the grid.
        .simultaneousGesture(
            DragGesture(minimumDistance: 8)
                .onChanged { g in
                    if !drag.dragging {
                        guard abs(g.translation.width) > abs(g.translation.height) else { return }
                        drag.begin(remote: source.volume)
                    }
                    let v = drag.move(delta: g.translation.width, fullRange: 220)
                    model.setVolume(endpoint: false, id: source.id, volume: v)
                }
                .onEnded { _ in if drag.dragging { drag.end() } }
        )
        .accessibilityElement(children: .contain)
    }
}

/// The PC's microphone: same ring language as the app cards.
struct MicCard: View {
    let endpoint: Endpoint
    let level: Double
    @ScaledMetric(relativeTo: .title) private var pctSize: CGFloat = 26
    @EnvironmentObject var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduce
    @StateObject private var drag = DragState()

    var body: some View {
        let shown = drag.shown(remote: endpoint.volume)
        let pct = Int((shown * 100).rounded())
        let active = drag.dragging || drag.holding
        let name = String(localized: "Microfone")
        ZStack(alignment: .topTrailing) {
            VStack(spacing: 10) {
                ZStack {
                    VolumeArc(fraction: shown, lineWidth: active ? 6 : 4,
                              color: endpoint.muted ? Archon.edge : Archon.accent, thumbRadius: active ? 7 : 0)
                        .animation(drag.dragging || reduce ? nil : Motion.arc, value: shown)
                    if active {
                        Text("\(pct)").font(.system(size: min(pctSize, 26 * 1.3), weight: .semibold).monospacedDigit())
                            .foregroundStyle(Archon.paper)
                    } else {
                        ZStack {
                            RoundedRectangle(cornerRadius: 14).fill(Archon.raised)
                            Image(systemName: endpoint.muted ? "mic.slash" : "mic")
                                .font(.system(size: 20, weight: .regular))
                                .foregroundStyle(endpoint.muted ? Archon.muted : Archon.paper)
                        }
                        .frame(width: 48, height: 48)
                    }
                }
                .frame(width: 96, height: 96)
                HStack(spacing: 6) {
                    if !endpoint.muted && level > 0.02 {
                        Dot(color: Archon.ok, opacity: reduce ? 1 : 0.55 + 0.45 * min(max(level, 0), 1))
                            .animation(.linear(duration: 0.05), value: level)
                    }
                    Text(name).font(.archonCaption).foregroundStyle(endpoint.muted ? Archon.muted : Archon.paper).lineLimit(1)
                }
                .padding(.horizontal, 14)
            }
            .frame(maxWidth: .infinity)
            .padding(.top, 16).padding(.bottom, 18)
            .accessibilityElement(children: .ignore)
            .accessibilityLabel(name + ", " + endpoint.name + (endpoint.muted ? String(localized: ", mudo") : ""))
            .accessibilityValue("\(pct)%")
            .accessibilityAdjustableAction { dir in
                let step = CoreClient.maxVolume / 20
                let v = endpoint.volume + (dir == .increment ? step : -step)
                model.setVolume(endpoint: true, id: endpoint.id, volume: min(max(v, 0), CoreClient.maxVolume))
            }
            MuteButton(label: name, muted: endpoint.muted) {
                model.setMute(endpoint: true, id: endpoint.id, muted: !endpoint.muted)
            }
            .padding(4)
        }
        .frame(maxHeight: .infinity, alignment: .top)
        .background(Archon.surface, in: RoundedRectangle(cornerRadius: 24))
        .contentShape(RoundedRectangle(cornerRadius: 24))
        .simultaneousGesture(
            DragGesture(minimumDistance: 8)
                .onChanged { g in
                    if !drag.dragging {
                        guard abs(g.translation.width) > abs(g.translation.height) else { return }
                        drag.begin(remote: endpoint.volume)
                    }
                    let v = drag.move(delta: g.translation.width, fullRange: 220)
                    model.setVolume(endpoint: true, id: endpoint.id, volume: v)
                }
                .onEnded { _ in if drag.dragging { drag.end() } }
        )
        .accessibilityElement(children: .contain)
    }
}

/// Shown while the panic mute is on: the one place to undo it.
struct PanicBanner: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(spacing: 12) {
            Dot(color: Archon.error)
            Text("Tudo silenciado no PC").font(.archonCaption).foregroundStyle(Archon.paper)
            Spacer(minLength: 0)
            Button { model.togglePanic() } label: {
                Text("Reativar").font(.archonCaption.weight(.semibold)).foregroundStyle(Archon.ink)
                    .padding(.horizontal, 16).frame(minHeight: 36)
                    .background(Archon.accent, in: Capsule())
            }
            .buttonStyle(.plain)
            .frame(minHeight: 44)
        }
        .padding(.horizontal, 16).padding(.vertical, 6)
        .background(Archon.surface, in: RoundedRectangle(cornerRadius: 20))
        .accessibilityElement(children: .combine)
    }
}

/// Shown while the PC's audio plays on this phone.
struct ListeningBanner: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                Dot(color: Archon.accent)
                Text("Ouvindo o PC no celular").font(.archonCaption).foregroundStyle(Archon.paper)
                Spacer(minLength: 0)
                Button { model.stopListening() } label: {
                    Text("Parar").font(.archonCaption.weight(.semibold)).foregroundStyle(Archon.ink)
                        .padding(.horizontal, 16).frame(minHeight: 36)
                        .background(Archon.accent, in: Capsule())
                }
                .buttonStyle(.plain)
                .frame(minHeight: 44)
            }
            Toggle(isOn: $model.listenMutesPC) {
                Text("Silenciar o PC enquanto ouço").font(.archonCaption).foregroundStyle(Archon.muted)
            }
            .tint(Archon.accent)
            .onChange(of: model.listenMutesPC) { _ in
                // Applies to the stream right away: restart it with the new choice.
                if model.listening { model.stopListening(); model.startListening() }
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .background(Archon.surface, in: RoundedRectangle(cornerRadius: 20))
    }
}

enum ListenText {
    static func of(_ e: Int) -> LocalizedStringKey {
        switch e {
        case CoreErr.busy: return "O PC já está transmitindo para outros celulares ou não conseguiu capturar o som."
        case CoreErr.connectionLost: return "A saída de som do PC deixou de existir. Escolha outra e tente de novo."
        default: return "Não foi possível ouvir o PC agora. Tente de novo."
        }
    }
}
