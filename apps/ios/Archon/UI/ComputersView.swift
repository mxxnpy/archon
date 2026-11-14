import SwiftUI
import UIKit

struct ComputersView: View {
    @EnvironmentObject var model: AppModel
    @EnvironmentObject var discovery: Discovery
    @Environment(\.accessibilityReduceMotion) private var reduce
    @State private var adding = false
    @State private var address = ""
    @State private var invalid = false

    var body: some View {
        let rows = model.rows(found: discovery.found)
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text("Computadores").font(.archonLargeTitle).foregroundStyle(Archon.paper)
                    .accessibilityAddTraits(.isHeader)
                    .padding(.top, 8)
                Radar(rows: rows, active: model.status != .connected) { model.open($0.target) }
                    .frame(height: 300)
                    .dynamicTypeSize(...DynamicTypeSize.xLarge)

                if model.pairingRemovedNotice {
                    Button { model.pairingRemovedNotice = false } label: {
                        HStack(spacing: 12) {
                            Dot(color: Archon.error)
                            Text("Este pareamento foi removido no PC. Pareie de novo para controlar.")
                                .font(.archonCaption).foregroundStyle(Archon.paper).multilineTextAlignment(.leading)
                            Spacer(minLength: 0)
                        }
                        .padding(16)
                        .background(Archon.surface, in: RoundedRectangle(cornerRadius: 20))
                    }.buttonStyle(.plain)
                }

                if discovery.permissionDenied {
                    Text("Permita o acesso à Rede Local em Ajustes › Archon para encontrar o PC.")
                        .font(.archonCaption).foregroundStyle(Archon.warn).frame(maxWidth: .infinity)
                        .multilineTextAlignment(.center)
                } else if rows.isEmpty {
                    VStack(spacing: 4) {
                        Text("Procurando computadores…").font(.archonBody).foregroundStyle(Archon.paper)
                        Text("Abra o Archon no PC. O celular e o PC precisam estar na mesma rede (cabo ou Wi-Fi).")
                            .font(.archonCaption).foregroundStyle(Archon.muted)
                    }
                    .multilineTextAlignment(.center).frame(maxWidth: .infinity).padding(.bottom, 16)
                }

                ForEach(Array(rows.enumerated()), id: \.element.id) { i, row in
                    PCRowView(row: row) { model.open(row.target) }.appear(i)
                }

                Button { adding = true } label: {
                    Text("Adicionar por endereço").font(.archonBody).foregroundStyle(Archon.accent)
                        .frame(maxWidth: .infinity, minHeight: 48)
                }
            }
            .padding(.horizontal, 20)
        }
        .alert("Adicionar por endereço", isPresented: $adding) {
            TextField("192.168.0.10", text: $address).keyboardType(.URL).textInputAutocapitalization(.never)
                .autocorrectionDisabled()
            Button("Conectar") { if !model.openAddress(address) { invalid = true } }
            Button("Cancelar", role: .cancel) {}
        } message: {
            Text("Use o endereço mostrado no Archon do PC (ícone da bandeja › Dispositivos pareados).")
        }
        .alert("Endereço inválido", isPresented: $invalid) {
            Button("OK", role: .cancel) {}
        } message: { Text("Digite um endereço como 192.168.0.10 ou 192.168.0.10:47321") }
    }
}

private func statusColor(_ r: PCRow) -> Color { r.online ? Archon.ok : Archon.error }

private func statusText(_ r: PCRow) -> String {
    if !r.paired { return String(localized: "Novo, toque para parear") }
    return r.online ? String(localized: "Pareado, disponível") : String(localized: "Pareado, offline")
}

struct Radar: View {
    var rows: [PCRow]
    /// True while no PC is connected: the pulse keeps going the whole time the
    /// radar is on screen. Reduce Motion turns it off (the user's way to stop
    /// moving content); TimelineView also pauses it in the background.
    var active: Bool
    var pick: (PCRow) -> Void
    @Environment(\.accessibilityReduceMotion) private var reduce

    var body: some View {
        GeometryReader { g in
            let center = CGPoint(x: g.size.width / 2, y: 150)
            ZStack {
                TimelineView(.animation(minimumInterval: nil, paused: !active || reduce)) { ctx in
                    RadarRings(center: center, pulse: (active && !reduce) ? Self.phase(ctx.date) : 0)
                }
                ForEach(rows) { row in
                    RadarPoint(row: row, center: center, pick: pick)
                }
            }
            .animation(reduce ? nil : Motion.ui, value: rows)
        }
        .accessibilityElement(children: .contain)
    }

    /// 0..1 position in the 2.4 s pulse loop (never exactly 0, so the ring shows).
    static func phase(_ date: Date) -> CGFloat {
        let p = date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 2.4) / 2.4
        return CGFloat(max(p, 0.001))
    }
}

// Stable per-id angle: String.hashValue is randomized per launch, so use a deterministic hash.
private extension String {
    var stableHash: Int { unicodeScalars.reduce(5381) { ($0 &* 33) &+ Int($1.value) } }
}

private extension PCRow {
    var angleSeed: Int { id.stableHash }
}

struct PCRowView: View {
    var row: PCRow
    var tap: () -> Void
    var body: some View {
        Button(action: tap) {
            HStack(spacing: 14) {
                Dot(color: statusColor(row))
                VStack(alignment: .leading, spacing: 2) {
                    Text(row.target.name).font(.archonBody).foregroundStyle(row.online ? Archon.paper : Archon.muted)
                        .lineLimit(2)
                    Text(row.paired ? (row.online ? "Disponível" : "Offline") : "Novo · toque em Parear")
                        .font(.archonCaption).foregroundStyle(Archon.muted)
                }
                .padding(.vertical, 10)
                Spacer()
                if row.paired {
                    Image(systemName: "chevron.right").font(.system(size: 14, weight: .semibold)).foregroundStyle(Archon.muted)
                } else {
                    Text("Parear").font(.archonCaption.weight(.semibold)).foregroundStyle(Archon.ink)
                        .padding(.horizontal, 16).padding(.vertical, 8)
                        .background(Archon.accent, in: Capsule())
                }
            }
            .padding(.horizontal, 16)
            .frame(minHeight: 64)
            .background(Archon.surface, in: RoundedRectangle(cornerRadius: 20))
        }
        .buttonStyle(.plain)
        .accessibilityLabel("\(row.target.name), \(statusText(row))")
    }
}

/// Orbits, the pulse and the phone glyph (split out so the Swift
/// type-checker handles each piece quickly).
private struct RadarRings: View {
    let center: CGPoint
    let pulse: CGFloat

    var body: some View {
        ZStack {
            Circle().stroke(Archon.track, lineWidth: 1).frame(width: 140, height: 140).position(center)
            Circle().stroke(Archon.track, lineWidth: 1).frame(width: 260, height: 260).position(center)
            if pulse > 0 {
                Circle().stroke(Archon.accent.opacity(Double(1 - pulse)), lineWidth: 1)
                    .frame(width: 260 * pulse, height: 260 * pulse)
                    .position(center)
            }
            Image(systemName: "iphone").font(.system(size: 22, weight: .light))
                .foregroundStyle(Archon.paper).position(center)
        }
    }
}

/// One PC on the radar: the dot sits exactly on its orbit, the name hangs toward the centre.
private struct RadarPoint: View {
    let row: PCRow
    let center: CGPoint
    let pick: (PCRow) -> Void

    private var point: CGPoint {
        let radius: CGFloat = (row.paired && row.online) ? 70 : 130
        let degrees = Double(((row.angleSeed % 360) + 360) % 360)  // abs(Int.min) would trap
        let radians = degrees * Double.pi / 180
        let dx = radius * CGFloat(cos(radians))
        let dy = radius * CGFloat(sin(radians))
        return CGPoint(x: center.x + dx, y: center.y + dy)
    }

    var body: some View {
        let p = point
        // Outer orbit: name hangs toward the centre (stays on screen). Inner
        // orbit: name hangs outward (never covers the phone glyph).
        let inner = row.paired && row.online
        let labelLeft = inner ? p.x < center.x : p.x > center.x
        let x: CGFloat = labelLeft ? p.x + 4 - 70 : p.x - 4 + 70
        return Button { pick(row) } label: {
            HStack(spacing: 6) {
                if labelLeft {
                    label(alignRight: true)
                    Dot(color: statusColor(row))
                } else {
                    Dot(color: statusColor(row))
                    label(alignRight: false)
                }
            }
            .frame(width: 140, alignment: labelLeft ? .trailing : .leading)
        }
        .buttonStyle(.plain)
        .position(x: x, y: p.y)
        .transition(.scale(scale: 0.6).combined(with: .opacity))
        .accessibilityHidden(true)
    }

    /// Long names wrap to a second line (aligned toward the dot) instead of
    /// running across the radar; beyond two lines they end with "…".
    private func label(alignRight: Bool) -> some View {
        Text(row.target.name).font(.archonCaption)
            .foregroundStyle(row.online ? Archon.paper : Archon.muted)
            .lineLimit(2).truncationMode(.tail)
            .multilineTextAlignment(alignRight ? .trailing : .leading)
            .fixedSize(horizontal: false, vertical: true)
    }
}
