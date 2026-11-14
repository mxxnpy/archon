import SwiftUI

/// 1.5: profiles — saved mixer states that the PC applies by hand or by itself
/// (days and hours, its network, an output such as headphones being present).
struct ProfilesView: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State private var editing: ProfileInfo?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 12) {
                    Text(model.pcNetwork.isEmpty ? String(localized: "O PC não está em nenhuma rede agora.")
                                                 : String(localized: "Rede do PC: \(model.pcNetwork)"))
                        .font(.archonCaption).foregroundStyle(Archon.muted)
                        .frame(maxWidth: .infinity, alignment: .leading).padding(.horizontal, 4)
                    if model.sortedProfiles.isEmpty {
                        Text("Salve o som do PC como está agora e aplique de novo com um toque — ou deixe o PC aplicar sozinho em certos dias, horários, redes ou quando um fone for conectado.")
                            .font(.archonCaption).foregroundStyle(Archon.muted).multilineTextAlignment(.center)
                            .padding(24)
                    }
                    ForEach(Array(model.sortedProfiles.enumerated()), id: \.element.id) { i, p in
                        ProfileCard(profile: p, onEdit: { editing = p }).appear(i)
                    }
                    Button {
                        editing = ProfileInfo.new(name: String(localized: "Perfil \(model.profiles.count + 1)"))
                    } label: {
                        Text("Salvar o som atual como perfil").font(.archonBody).foregroundStyle(Archon.ink)
                            .frame(maxWidth: .infinity, minHeight: 50)
                            .background(Archon.accent, in: Capsule())
                    }
                    .disabled(model.status != .connected || model.profiles.count >= 32)
                    .opacity(model.status == .connected ? 1 : 0.4)
                    .padding(.top, 8)
                }
                .padding(16)
            }
            .background(Archon.bg)
            .navigationTitle("Perfis")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Fechar") { dismiss() }.foregroundStyle(Archon.accent)
                }
            }
            .sheet(item: $editing) { p in ProfileEditor(profile: p).environmentObject(model) }
        }
        .preferredColorScheme(.dark)
    }
}

private struct ProfileCard: View {
    let profile: ProfileInfo
    let onEdit: () -> Void
    @EnvironmentObject var model: AppModel

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 6) {
                    if profile.active { Dot(color: Archon.accent) }
                    Text(profile.name).font(.archonBody).foregroundStyle(Archon.paper).lineLimit(2)
                }
                Text(profile.summary).font(.archonCaption).foregroundStyle(Archon.muted).lineLimit(3)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .accessibilityElement(children: .combine)
            .accessibilityValue(profile.active ? String(localized: "em uso") : "")
            Button { model.applyProfile(profile.id) } label: {
                Text("Aplicar").font(.archonCaption.weight(.semibold)).foregroundStyle(Archon.ink)
                    .padding(.horizontal, 16).frame(minHeight: 44)
                    .background(Archon.accent, in: Capsule())
            }
            .disabled(model.status != .connected)
            .accessibilityLabel(String(localized: "Aplicar \(profile.name)"))
            Button(action: onEdit) {
                Image(systemName: "slider.horizontal.3").foregroundStyle(Archon.paper)
                    .frame(width: 44, height: 44).background(Archon.raised, in: Circle())
            }
            .accessibilityLabel(String(localized: "Editar \(profile.name)"))
        }
        .padding(16)
        .background(Archon.surface, in: RoundedRectangle(cornerRadius: 24))
    }
}

private struct ProfileEditor: View {
    @EnvironmentObject var model: AppModel
    @Environment(\.dismiss) private var dismiss
    @State var profile: ProfileInfo
    @State private var useTime = false
    @State private var start = Date()
    @State private var end = Date()
    @State private var useNetwork = false
    @State private var confirmDelete = false

    private static let dayLetters = ["D", "S", "T", "Q", "Q", "S", "S"]
    private static let dayNames = [String(localized: "domingo"), String(localized: "segunda"), String(localized: "terça"),
                                   String(localized: "quarta"), String(localized: "quinta"), String(localized: "sexta"),
                                   String(localized: "sábado")]

    init(profile: ProfileInfo) {
        _profile = State(initialValue: profile)
        _useTime = State(initialValue: profile.days != 0 || profile.startMin != profile.endMin)
        _start = State(initialValue: Self.date(profile.startMin == profile.endMin ? 9 * 60 : profile.startMin))
        _end = State(initialValue: Self.date(profile.startMin == profile.endMin ? 18 * 60 : profile.endMin))
        _useNetwork = State(initialValue: !profile.network.isEmpty)
    }

    static func date(_ minutes: Int) -> Date {
        Calendar.current.date(bySettingHour: minutes / 60, minute: minutes % 60, second: 0, of: Date()) ?? Date()
    }
    static func minutes(_ d: Date) -> Int {
        let c = Calendar.current.dateComponents([.hour, .minute], from: d)
        return (c.hour ?? 0) * 60 + (c.minute ?? 0)
    }

    private var networkName: String { profile.network.isEmpty ? model.pcNetwork : profile.network }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    TextField("Nome", text: $profile.name)
                        .font(.archonBody).foregroundStyle(Archon.paper)
                        .padding(14).background(Archon.surface, in: RoundedRectangle(cornerRadius: 16))
                    if !profile.isNew {
                        Text("Guarda \(profile.nEndpoints) dispositivos e \(profile.nApps) apps.")
                            .font(.archonCaption).foregroundStyle(Archon.muted)
                    }
                    section {
                        Toggle("Aplicar sozinho", isOn: $profile.automatic).tint(Archon.accent)
                        if profile.automatic {
                            Text("Quando todas as condições ligadas valerem:").font(.archonCaption).foregroundStyle(Archon.muted)
                            Toggle("Dias e horário", isOn: $useTime).tint(Archon.accent)
                            if useTime {
                                HStack(spacing: 6) {
                                    ForEach(0..<7, id: \.self) { d in
                                        let on = profile.days & (1 << d) != 0
                                        Button {
                                            profile.days ^= UInt8(1 << d)
                                        } label: {
                                            Text(Self.dayLetters[d]).font(.archonCaption.weight(.semibold))
                                                .foregroundStyle(on ? Archon.ink : Archon.paper)
                                                .frame(width: 40, height: 40)
                                                .background(on ? Archon.accent : Archon.raised, in: Circle())
                                        }
                                        .accessibilityLabel(Self.dayNames[d])
                                        .accessibilityAddTraits(on ? .isSelected : [])
                                    }
                                }
                                Text(profile.days == 0 ? "Todos os dias" : "Só nos dias marcados")
                                    .font(.archonCaption).foregroundStyle(Archon.muted)
                                DatePicker("Das", selection: $start, displayedComponents: .hourAndMinute)
                                DatePicker("Até", selection: $end, displayedComponents: .hourAndMinute)
                            }
                            Toggle(isOn: $useNetwork) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Só nesta rede")
                                    Text(networkName.isEmpty ? String(localized: "PC sem rede agora") : networkName)
                                        .font(.archonCaption).foregroundStyle(Archon.muted)
                                }
                            }
                            .tint(Archon.accent)
                            .disabled(networkName.isEmpty && !useNetwork)
                            Picker("Quando esta saída estiver conectada", selection: $profile.outputId) {
                                Text("Qualquer").tag("")
                                ForEach(model.outputs) { e in Text(e.name).tag(e.id) }
                                if !profile.outputId.isEmpty, !model.outputs.contains(where: { $0.id == profile.outputId }) {
                                    Text(profile.outputName).tag(profile.outputId)  // unplugged right now
                                }
                            }
                            .tint(Archon.accent)
                            Toggle("Ao terminar, voltar como estava", isOn: $profile.restore).tint(Archon.accent)
                        }
                    }
                    if !profile.isNew {
                        Button { save(capture: true) } label: {
                            Text("Atualizar com o som atual").font(.archonBody).foregroundStyle(Archon.paper)
                                .frame(maxWidth: .infinity, minHeight: 50).background(Archon.raised, in: Capsule())
                        }
                        Button(role: .destructive) { confirmDelete = true } label: {
                            Text("Apagar perfil").font(.archonBody).foregroundStyle(Archon.error)
                                .frame(maxWidth: .infinity, minHeight: 44)
                        }
                    }
                }
                .padding(16)
                .foregroundStyle(Archon.paper)
            }
            .background(Archon.bg)
            .navigationTitle(profile.isNew ? "Novo perfil" : "Editar perfil")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) { Button("Cancelar") { dismiss() }.foregroundStyle(Archon.paper) }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Salvar") { save(capture: profile.isNew) }
                        .foregroundStyle(Archon.accent)
                        .disabled(profile.name.trimmingCharacters(in: .whitespaces).isEmpty || model.status != .connected)
                }
            }
            .confirmationDialog("Apagar \(profile.name)?", isPresented: $confirmDelete, titleVisibility: .visible) {
                Button("Apagar", role: .destructive) { model.deleteProfile(profile.id); dismiss() }
                Button("Cancelar", role: .cancel) {}
            }
        }
        .preferredColorScheme(.dark)
    }

    @ViewBuilder private func section<C: View>(@ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 14) { content() }
            .padding(16).frame(maxWidth: .infinity, alignment: .leading)
            .background(Archon.surface, in: RoundedRectangle(cornerRadius: 24))
    }

    private func save(capture: Bool) {
        var p = profile
        p.name = p.name.trimmingCharacters(in: .whitespaces)
        if p.automatic && useTime {
            p.startMin = Self.minutes(start)
            p.endMin = Self.minutes(end)
        } else {
            p.days = 0
            p.startMin = 0
            p.endMin = 0
        }
        p.network = p.automatic && useNetwork ? networkName : ""
        if !p.automatic { p.outputId = "" }
        p.outputName = model.endpoints[p.outputId]?.name ?? (p.outputId.isEmpty ? "" : p.outputName)
        model.saveProfile(p, capture: capture)
        dismiss()
    }
}
