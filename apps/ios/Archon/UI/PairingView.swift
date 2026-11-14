import SwiftUI
import UIKit

struct PairingView: View {
    let target: Target
    @EnvironmentObject var model: AppModel
    @Environment(\.accessibilityReduceMotion) private var reduce
    @State private var code = ""
    @State private var wrong = false
    @FocusState private var focused: Bool

    var body: some View {
        ScrollView {
        VStack(spacing: 0) {
            HStack {
                Button { model.back() } label: {
                    Image(systemName: "chevron.left").font(.system(size: 18, weight: .semibold))
                        .foregroundStyle(Archon.paper).frame(width: 44, height: 44)
                }
                .accessibilityLabel("Voltar")
                Spacer()
            }
            Text("Código do PC").font(.archonTitle).foregroundStyle(Archon.paper).accessibilityAddTraits(.isHeader)
            Text(target.name).font(.archonCaption).foregroundStyle(Archon.muted)
                .padding(.bottom, 28)

            ZStack {
                CodeRing(code: code, status: model.status, wrong: wrong,
                         size: min(310, UIScreen.main.bounds.width - 40))
                    .onTapGesture { focused = true }
                // Invisible field: system number pad, one-time-code autofill and paste.
                TextField("", text: $code)
                    .keyboardType(.numberPad)
                    .textContentType(.oneTimeCode)
                    .focused($focused)
                    .frame(width: 1, height: 1).opacity(0.01)
                    .accessibilityLabel("Código de pareamento, 6 dígitos")
                    .onChange(of: code) { v in
                        let digits = String(v.filter(\.isNumber).prefix(6))
                        if digits != v { code = digits; return }
                        if digits.count > 0 { UIImpactFeedbackGenerator(style: .light).impactOccurred() }
                        if digits.count == 6 { model.submit(code: digits) }
                    }
                    .disabled(model.status != .awaitingCode)
            }

            status.padding(.top, 28)
            Spacer(minLength: 32)
            if model.status == .failed && model.error != CoreErr.rejected {
                Button { code = ""; model.retryPairing() } label: {
                    Text("Tentar de novo").font(.archonBody).foregroundStyle(Archon.ink)
                        .frame(maxWidth: .infinity, minHeight: 52)
                        .background(Archon.accent, in: Capsule())
                }
                .buttonStyle(.plain)
                .padding(.bottom, 20)
            }
        }
        .padding(.horizontal, 20)
        }
        .scrollDismissesKeyboard(.never)
        .onChange(of: model.status) { s in
            UIAccessibility.post(notification: .announcement, argument: Self.spoken(s, model.error))
            if s == .awaitingCode { focused = true }
            if s == .failed {
                if model.error == CoreErr.wrongCode {
                    UINotificationFeedbackGenerator().notificationOccurred(.error)
                    wrong = true
                    DispatchQueue.main.asyncAfter(deadline: .now() + 0.6) { wrong = false; code = "" }
                } else {
                    code = ""
                }
            }
            if s == .connected { UINotificationFeedbackGenerator().notificationOccurred(.success) }
        }
        .onAppear {
            if let demo = model.demoCode { code = demo }  // debug screenshots only
            focused = true
        }
    }

    static func spoken(_ s: LinkStatus, _ e: Int) -> String {
        switch s {
        case .awaitingCode: return String(localized: "Aguardando o código")
        case .verifying: return String(localized: "Verificando…")
        case .connected: return String(localized: "Conectado")
        case .failed: return ErrorText.spoken(e)  // say why, not just "failed"
        default: return String(localized: "Conectando…")
        }
    }

    @ViewBuilder private var status: some View {
        switch model.status {
        case .idle, .connecting, .reconnecting: StatusLine(color: Archon.warn, text: "Conectando…")
        case .awaitingCode: StatusLine(color: Archon.warn, text: "Aguardando o código")
        case .verifying: StatusLine(color: Archon.warn, text: "Verificando…")
        case .connected: StatusLine(color: Archon.ok, text: "Conectado")
        case .failed: StatusLine(color: Archon.error, text: ErrorText.of(model.error))
        }
    }
}

enum ErrorText {
    static func spoken(_ e: Int) -> String {
        switch e {
        case CoreErr.wrongCode: return String(localized: "Código errado — veja o novo código no PC.")
        case CoreErr.locked: return String(localized: "Muitos códigos errados. Aguarde alguns minutos.")
        case CoreErr.rejected: return String(localized: "O pareamento foi recusado no PC.")
        case CoreErr.busy: return String(localized: "O PC está pareando outro aparelho. Tente de novo em instantes.")
        case CoreErr.timeout: return String(localized: "Demorou demais. Tente de novo.")
        case CoreErr.version: return String(localized: "Atualize o Archon no PC e no celular.")
        case CoreErr.connectFailed: return String(localized: "Não foi possível falar com o PC. Veja se ele está ligado e na mesma rede.")
        case CoreErr.revoked: return String(localized: "Este pareamento foi removido no PC. Pareie de novo para controlar.")
        case CoreErr.notRecognized, CoreErr.unknown, CoreErr.authFailed:
            return String(localized: "O PC não reconheceu este celular. Se você removeu o pareamento no PC, pareie de novo.")
        default: return String(localized: "Conexão perdida.")
        }
    }

    static func of(_ e: Int) -> LocalizedStringKey {
        switch e {
        case CoreErr.wrongCode: return "Código errado — veja o novo código no PC."
        case CoreErr.locked: return "Muitos códigos errados. Aguarde alguns minutos."
        case CoreErr.rejected: return "O pareamento foi recusado no PC."
        case CoreErr.busy: return "O PC está pareando outro aparelho. Tente de novo em instantes."
        case CoreErr.timeout: return "Demorou demais. Tente de novo."
        case CoreErr.version: return "Atualize o Archon no PC e no celular."
        case CoreErr.connectFailed: return "Não foi possível falar com o PC. Veja se ele está ligado e na mesma rede."
        case CoreErr.revoked: return "Este pareamento foi removido no PC. Pareie de novo para controlar."
        case CoreErr.notRecognized, CoreErr.unknown, CoreErr.authFailed:
            return "O PC não reconheceu este celular. Se você removeu o pareamento no PC, pareie de novo."
        case CoreErr.wrongAgent: return "Outro aparelho respondeu no endereço do PC. Tentando de novo…"
        default: return "Conexão perdida."
        }
    }
}

struct CodeRing: View {
    var code: String
    var status: LinkStatus
    var wrong: Bool
    @Environment(\.accessibilityReduceMotion) private var reduce
    @ScaledMetric(relativeTo: .title) private var orbSize: CGFloat = 24
    @ScaledMetric(relativeTo: .largeTitle) private var counterSize: CGFloat = 44
    @State private var spin = 0.0
    @State private var shake = 0.0

    var size: CGFloat = 310
    private var orb: CGFloat { size * 52 / 310 }

    var body: some View {
        let n = code.count
        ZStack {
            Circle().stroke(Archon.track, lineWidth: 3).frame(width: size - orb, height: size - orb)
            Circle().trim(from: 0, to: CGFloat(n) / 6)
                .stroke(wrong ? Archon.error : Archon.accent, style: StrokeStyle(lineWidth: 3, lineCap: .round))
                .rotationEffect(.degrees(-90 + spin))
                .frame(width: size - orb, height: size - orb)
                .animation(reduce ? nil : Motion.arc, value: n)
            ForEach(0..<6, id: \.self) { i in
                let a = (-90 + 60 * Double(i)) * .pi / 180
                let r = (size - orb) / 2
                let filled = i < n
                let current = i == n && status == .awaitingCode
                ZStack {
                    Circle().fill(wrong ? Archon.error : filled ? Archon.accent : Archon.raised)
                    if current { Circle().stroke(Archon.accent, lineWidth: 1.5) }
                    if filled {
                        Text(String(Array(code)[i])).font(.system(size: min(orbSize, 24 * 1.3), weight: .semibold).monospacedDigit()).foregroundStyle(Archon.ink)
                    } else if current {
                        Rectangle().fill(Archon.accent).frame(width: 2, height: 22)
                    }
                }
                .frame(width: orb, height: orb)
                .scaleEffect(filled ? 1 : 0.85)
                .animation(reduce ? nil : Motion.pop, value: filled)
                .offset(x: r * cos(a), y: r * sin(a))
            }
            Text("\(n)/6").font(.system(size: min(counterSize, 44 * 1.3), weight: .semibold).monospacedDigit()).foregroundStyle(Archon.paper)
        }
        .frame(width: size, height: size)
        .rotationEffect(.degrees(shake))
        .contentShape(Circle())
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(n) de 6 dígitos")
        .onChange(of: status) { s in
            // Verifying: the progress arc turns slowly and stops by itself after 5 s.
            if s == .verifying && !reduce {
                withAnimation(.linear(duration: 5)) { spin = 360 }
            } else {
                spin = 0
            }
        }
        .onChange(of: wrong) { w in
            guard w, !reduce else { return }
            Task { @MainActor in
                for t in [8.0, -8, 5, -3, 0] {
                    withAnimation(.easeOut(duration: 0.07)) { shake = t }
                    try? await Task.sleep(nanoseconds: 70_000_000)
                }
            }
        }
    }
}
