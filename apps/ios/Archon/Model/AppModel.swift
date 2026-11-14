import Foundation
import SwiftUI
import UIKit

struct Target: Equatable, Hashable {
    var agentId: String?
    var name: String
    var host: String
    var port: UInt16
}

struct Endpoint: Identifiable, Equatable {
    let id: String
    var name: String
    var volume: Int
    var muted: Bool
    var isDefault: Bool
    var meter: Int
    var input = false           // microphone
    var isDefaultInput = false  // the PC's default microphone
    var battery = -1            // wireless device battery 0..100 (-1 = none)
    /// "Fone Bluetooth · 80 %" when the device reports a battery.
    var label: String { battery >= 0 ? "\(name) · \(battery)%" : name }
}

struct Source: Identifiable, Equatable {
    let id: String
    var endpointId: String
    var name: String
    var iconKey: String
    var volume: Int
    var muted: Bool
    var playing: Bool
    var system: Bool
    var meter: Int
    var route = ""  // output this app is pinned to ("" = follows the PC's default)
    // 1.4: a browser tab (volume inside the browser; no output routing)
    var tab = false
    var group = ""   // browser ("Brave")
    var detail = ""  // site ("youtube.com")
}

/// 1.5: a profile as the phone sees it (the settings themselves stay on the PC).
struct ProfileInfo: Identifiable, Equatable {
    var id: String
    var name: String
    var active = false
    var automatic = false
    var restore = false
    var days: UInt8 = 0
    var startMin = 0
    var endMin = 0
    var network = ""
    var outputId = ""
    var outputName = ""
    var nEndpoints = 0
    var nApps = 0

    var isNew: Bool { id.hasPrefix("new:") }
    static func new(name: String) -> ProfileInfo { ProfileInfo(id: "new:" + UUID().uuidString, name: name) }

    static func clock(_ m: Int) -> String { String(format: "%02d:%02d", m / 60, m % 60) }

    /// "Seg–Sex 09:00–18:00 · Rede Casa · com Fone USB", or "Só manual".
    var summary: String {
        guard automatic else { return String(localized: "Só manual") }
        var parts: [String] = []
        if days != 0 || startMin != endMin {
            let letters = [String(localized: "dom"), String(localized: "seg"), String(localized: "ter"), String(localized: "qua"),
                           String(localized: "qui"), String(localized: "sex"), String(localized: "sáb")]
            var d = days == 0 ? String(localized: "todo dia") : (0..<7).filter { days & (1 << $0) != 0 }.map { letters[$0] }.joined(separator: ", ")
            if days == 0b0111110 { d = String(localized: "seg a sex") }
            if days == 0b1000001 { d = String(localized: "fim de semana") }
            parts.append(startMin == endMin ? d : d + " " + Self.clock(startMin) + "–" + Self.clock(endMin))
        }
        if !network.isEmpty { parts.append(String(localized: "rede \(network)")) }
        if !outputId.isEmpty { parts.append(String(localized: "com \(outputName)")) }
        return parts.joined(separator: " · ")
    }
}

struct PCRow: Identifiable, Equatable {
    var target: Target
    var paired: Bool
    var online: Bool
    var id: String { target.agentId ?? target.host }
}

enum Screen: Equatable {
    case computers
    case pairing(Target)
    case mixer(Target)
}

enum LinkStatus: Int {
    case idle, connecting, awaitingCode, verifying, connected, reconnecting, failed
}

enum CoreErr {
    static let none = 0, wrongCode = 4, locked = 5, rejected = 6, busy = 7, timeout = 9
    static let version = 2, unknown = 3, authFailed = 8, revoked = 10, connectFailed = 100, connectionLost = 101
    /// Unverified refusal (may be spoofed): ask the user, never auto-forget.
    static let notRecognized = 102, wrongAgent = 103
    /// Only an authenticated revocation (inside the encrypted channel) forgets the pairing.
    static func invalidatesPairing(_ e: Int) -> Bool { e == revoked }
}

/// App state. Core events arrive on a network thread and are applied on the main actor.
@MainActor
final class AppModel: ObservableObject {
    @Published private(set) var screen: Screen = .computers
    @Published private(set) var status: LinkStatus = .idle
    @Published private(set) var error: Int = CoreErr.none
    @Published private(set) var synced = false
    @Published private(set) var endpoints: [String: Endpoint] = [:]
    @Published private(set) var sources: [String: Source] = [:]
    @Published private(set) var icons: [String: UIImage] = [:]
    @Published private(set) var levels: [Int: Double] = [:]
    @Published var selectedEndpoint: String?
    /// What the connected PC supports (AGENT_INFO); 0 until it says.
    @Published private(set) var features: UInt32 = 0
    /// Everything on the PC muted by the panic mute.
    @Published private(set) var panic = false
    @Published private(set) var profiles: [String: ProfileInfo] = [:]  // 1.5
    @Published private(set) var pcNetwork = ""
    @Published var showProfiles = false
    /// "Ouvir no celular": the user wants the PC's audio on this phone.
    @Published private(set) var listening = false
    /// Why listening stopped on its own (CoreErr code), shown once.
    @Published var listenError: Int?
    /// Mute the PC's speakers while listening (remembered).
    @Published var listenMutesPC = UserDefaults.standard.object(forKey: "listenMutesPC") as? Bool ?? true {
        didSet { UserDefaults.standard.set(listenMutesPC, forKey: "listenMutesPC") }
    }
    private let player = StreamPlayer()
    @Published var pairingRemovedNotice = false
    @Published private(set) var paired: [PairedPC] = [] {
        didSet { discovery.pinned = Set(paired.map(\.agentId)) }
    }

    let discovery = Discovery()
    private let store = PairingStore()
    private var client: CoreClient!
    private var requestedIcons = Set<String>()
    private var connectedVia: String?
    /// Debug-only fixture state for UI screenshots (launch argument `-ArchonDemo <screen>`).
    private(set) var demoRows: [PCRow]?
    private(set) var demoCode: String?
    private var demo = false

    init() {
        paired = store.all()
        discovery.pinned = Set(paired.map(\.agentId))  // didSet doesn't run inside init
        let player = self.player
        client = CoreClient(controllerId: store.controllerId, name: UIDevice.current.name) { [weak self] ev in
            // Audio goes straight to the player's queue (50 per second): never via the main thread.
            if ev.type == ARC_EV_AUDIO {
                player.feed(packet: ev.data, lost: ev.lost)
                return
            }
            Task { @MainActor in self?.apply(ev) }
        }
        player.onStopRequest = { [weak self] in
            Task { @MainActor in self?.stopListening() }
        }
        #if DEBUG
        if let i = CommandLine.arguments.firstIndex(of: "-ArchonDemo"), i + 1 < CommandLine.arguments.count {
            loadDemo(CommandLine.arguments[i + 1])
        }
        #endif
    }

    #if DEBUG
    private func loadDemo(_ name: String) {
        demo = true
        let pc = Target(agentId: String(repeating: "a", count: 32), name: "DESKTOP-01", host: "192.168.0.10", port: 47321)
        demoRows = [
            PCRow(target: pc, paired: true, online: true),
            PCRow(target: Target(agentId: String(repeating: "b", count: 32), name: "NOTEBOOK-SALA", host: "192.168.0.22", port: 47321), paired: true, online: false),
            PCRow(target: Target(agentId: String(repeating: "c", count: 32), name: "ESTUDIO-PC", host: "192.168.0.31", port: 47321), paired: false, online: true),
        ]
        func mixer() {
            endpoints = [
                "e1": Endpoint(id: "e1", name: "SteelSeries Sonar - Gaming", volume: 6400, muted: false, isDefault: true, meter: 1),
                "e2": Endpoint(id: "e2", name: "Fone Bluetooth", volume: 5000, muted: false, isDefault: false, meter: 7, battery: 80),
                "m1": Endpoint(id: "m1", name: "Microfone (USB)", volume: 7500, muted: false, isDefault: false, meter: 8,
                               input: true, isDefaultInput: true),
            ]
            features = CoreClient.featureInputs | CoreClient.featureRouting | CoreClient.featurePanic | CoreClient.featureStream
            let list = [
                Source(id: "s1", endpointId: "e1", name: "Spotify", iconKey: "", volume: 7200, muted: false, playing: true, system: false, meter: 2),
                Source(id: "s2", endpointId: "e1", name: "Discord", iconKey: "", volume: 8000, muted: false, playing: true, system: false, meter: 3,
                       route: "e1"),
                Source(id: "s3", endpointId: "e1", name: "Counter-Strike 2", iconKey: "", volume: 5800, muted: false, playing: false, system: false, meter: 4),
                Source(id: "s4", endpointId: "e1", name: "Brave", iconKey: "", volume: 4500, muted: true, playing: false, system: false, meter: 5),
                Source(id: "s5", endpointId: "e1", name: "System sounds", iconKey: "", volume: 4000, muted: false, playing: false, system: true, meter: 6),
            ]
            sources = Dictionary(uniqueKeysWithValues: list.map { ($0.id, $0) })
            levels = [2: 0.9, 3: 0.4, 8: 0.6]
            synced = true
            screen = .mixer(pc)
        }
        switch name {
        case "pairing":
            screen = .pairing(pc); status = .awaitingCode; demoCode = "4829"
        case "pairing-wrong":
            screen = .pairing(pc); status = .failed; error = CoreErr.wrongCode
        case "mixer":
            mixer(); status = .connected
        case "reconnecting":
            mixer(); status = .reconnecting
        case "panic":
            mixer(); status = .connected; panic = true
        case "listen":
            mixer(); status = .connected; listening = true
        case "profiles":
            mixer(); status = .connected
            features |= CoreClient.featureProfiles
            pcNetwork = "MaHe-Casa-5G"
            for p in [
                ProfileInfo(id: "p1", name: "Trabalho", active: true, automatic: true, restore: true, days: 0b0111110,
                            startMin: 540, endMin: 1080, network: "MaHe-Casa-5G", nEndpoints: 3, nApps: 5),
                ProfileInfo(id: "p2", name: "Fones", automatic: true, outputId: "e2", outputName: "Fone Bluetooth",
                            nEndpoints: 3, nApps: 5),
                ProfileInfo(id: "p3", name: "Noite", automatic: true, restore: true, days: 0, startMin: 1320, endMin: 420,
                            nEndpoints: 3, nApps: 5),
                ProfileInfo(id: "p4", name: "Live", nEndpoints: 3, nApps: 6),
            ] { profiles[p.id] = p }
            showProfiles = true
        case "tabs":
            mixer(); status = .connected
            features |= CoreClient.featureTabs
            sources = sources.filter { $0.key == "s1" || $0.key == "s2" }
            for t in [
                Source(id: "t1", endpointId: "e1", name: "lofi hip hop radio - beats to relax/study to", iconKey: "", volume: 6000,
                       muted: false, playing: true, system: false, meter: 0, tab: true, group: "Brave", detail: "youtube.com"),
                Source(id: "t2", endpointId: "e1", name: "Podcast - Episódio 42", iconKey: "", volume: 10000,
                       muted: false, playing: true, system: false, meter: 0, tab: true, group: "Brave", detail: "open.spotify.com"),
                Source(id: "t3", endpointId: "e1", name: "Reunião da equipe", iconKey: "", volume: 8000,
                       muted: true, playing: false, system: false, meter: 0, tab: true, group: "Brave", detail: "meet.google.com"),
            ] { sources[t.id] = t }
        case "notrecognized":
            screen = .mixer(pc); status = .failed; error = CoreErr.notRecognized
        default:
            screen = .computers
        }
    }
    #endif

    // MARK: computers

    func rows(found: [String: DiscoveredPC]) -> [PCRow] {
        if let demoRows { return demoRows }
        // Discovered addresses are only candidates; they're saved after an
        // authenticated resume succeeds (anyone can advertise a PC's public id).
        var rows = paired.map { p -> PCRow in
            let live = found[p.agentId]
            // The name comes from the authenticated pairing record, never from mDNS
            // (a spoofed announcement must not relabel a paired PC).
            return PCRow(target: Target(agentId: p.agentId, name: p.name, host: live?.host ?? p.host,
                                        port: live?.port ?? p.port), paired: true, online: live != nil)
        }.sorted { $0.online && !$1.online }
        let others = found.values.filter { f in !paired.contains { $0.agentId == f.agentId } }
            .sorted { $0.name.lowercased() < $1.name.lowercased() }
            .map { PCRow(target: Target(agentId: $0.agentId, name: $0.name, host: $0.host, port: $0.port), paired: false, online: true) }
        rows.append(contentsOf: others)
        return rows
    }

    // MARK: lifecycle

    func foreground() {
        if demo { return }  // fixtures only: no discovery, no network
        discovery.start()
        if case .mixer(let t) = screen { connect(t) }
    }

    func background() {
        discovery.stop()
        // Listening keeps the link (and the audio) alive with the screen locked.
        if case .mixer = screen, !listening { client.disconnect() }
    }

    // MARK: navigation

    func open(_ t: Target) {
        pairingRemovedNotice = false
        if let id = t.agentId, store.load(id) != nil {
            screen = .mixer(t)
            connect(t)
        } else {
            screen = .pairing(t)
            resetLink()
            client.pair(host: t.host, port: t.port)
        }
    }

    @discardableResult
    func openAddress(_ raw: String) -> Bool {
        guard let (host, port) = Self.parse(raw) else { return false }
        open(Target(agentId: nil, name: host, host: host, port: port))
        return true
    }

    func back() {
        stopListening()
        client.disconnect()
        resetLink()
        screen = .computers
    }

    func forget(_ t: Target) {
        if let id = t.agentId { store.forget(id) }
        paired = store.all()
        back()
    }

    func submit(code: String) { client.submit(code: code) }

    func retryPairing() {
        guard case .pairing(let t) = screen else { return }
        resetLink()
        client.pair(host: t.host, port: t.port)
    }

    func reconnect() { if case .mixer(let t) = screen { connect(t) } }

    /// The PC didn't recognize us (unverified). Pair again with the last
    /// *authenticated* address; the old key is replaced only when the new
    /// pairing succeeds (store.save on PAIRED), never deleted up front.
    func repair(_ t: Target) {
        var target = t
        if let id = t.agentId, let known = store.load(id) { target.host = known.host; target.port = known.port }
        resetLink()
        screen = .pairing(target)
        client.pair(host: target.host, port: target.port)
    }

    // MARK: mixer

    func setMetersVisible(_ on: Bool) {
        if demo { return }
        client.setMeters(on)
        if !on { levels = [:] }
    }

    func setVolume(endpoint: Bool, id: String, volume: Int) {
        if endpoint { endpoints[id]?.volume = volume } else { sources[id]?.volume = volume }
        client.setVolume(endpoint: endpoint, id: id, volume: volume)
    }

    func setMute(endpoint: Bool, id: String, muted: Bool) {
        if endpoint { endpoints[id]?.muted = muted } else { sources[id]?.muted = muted }
        client.setMute(endpoint: endpoint, id: id, muted: muted)
    }

    /// Output devices only (microphones have their own card).
    var outputs: [Endpoint] {
        endpoints.values.filter { !$0.input }.sorted {
            if $0.isDefault != $1.isDefault { return $0.isDefault }
            return $0.name.lowercased() < $1.name.lowercased()
        }
    }

    var currentEndpoint: Endpoint? {
        if let s = selectedEndpoint, let e = endpoints[s], !e.input { return e }
        return outputs.first
    }

    /// The PC's default microphone (or any microphone).
    var microphone: Endpoint? {
        let mics = endpoints.values.filter(\.input)
        return mics.first { $0.isDefaultInput } ?? mics.min { $0.name < $1.name }
    }

    var canRoute: Bool { features & CoreClient.featureRouting != 0 }
    var canProfiles: Bool { features & CoreClient.featureProfiles != 0 }
    var sortedProfiles: [ProfileInfo] {
        profiles.values.sorted { $0.active != $1.active ? $0.active : $0.name.lowercased() < $1.name.lowercased() }
    }
    func saveProfile(_ p: ProfileInfo, capture: Bool) {
        if demo { return }
        client.saveProfile(p, capture: capture)
    }
    func deleteProfile(_ id: String) {
        if demo { return }
        client.deleteProfile(id)
    }
    func applyProfile(_ id: String) {
        if demo { return }
        client.applyProfile(id)
        UIImpactFeedbackGenerator(style: .light).impactOccurred()
    }
    var canListen: Bool { features & CoreClient.featureStream != 0 }

    func toggleListening() { listening ? stopListening() : startListening() }

    func startListening() {
        guard !listening, status == .connected else { return }
        do {
            try player.start()
        } catch {
            listenError = CoreErr.busy
            return
        }
        listening = true
        listenError = nil
        if !client.streamStart(mutePC: listenMutesPC) { stopListening() }
    }

    func stopListening() {
        guard listening else { return }
        listening = false
        client.streamStop()
        player.stop()
    }
    var canPanic: Bool { features & CoreClient.featurePanic != 0 }

    /// Send an app to an output; "" = follow the PC's default again.
    func setRoute(source: String, endpoint: String) {
        sources[source]?.route = endpoint
        client.setRoute(source: source, endpoint: endpoint)
    }

    func togglePanic() {
        panic.toggle()
        client.panicMute(panic)
        UINotificationFeedbackGenerator().notificationOccurred(.warning)
    }

    var currentSources: [Source] {
        guard let e = currentEndpoint else { return [] }
        return sources.values.filter { $0.endpointId == e.id && !$0.tab }.sorted {
            if $0.system != $1.system { return !$0.system }
            if $0.playing != $1.playing { return $0.playing }
            return $0.name.lowercased() < $1.name.lowercased()
        }
    }

    /// 1.4: browser tabs playing on the current output (playing first).
    var currentTabs: [Source] {
        guard let e = currentEndpoint else { return [] }
        return sources.values.filter { $0.endpointId == e.id && $0.tab }.sorted {
            if $0.playing != $1.playing { return $0.playing }
            if $0.group != $1.group { return $0.group < $1.group }
            return $0.name.lowercased() < $1.name.lowercased()
        }
    }

    // MARK: core events

    private func connect(_ t: Target) {
        guard let id = t.agentId, let p = store.load(id) else { screen = .computers; return }
        var addresses = [(t.host, t.port)]
        if p.host != t.host || p.port != t.port { addresses.append((p.host, p.port)) }
        connectedVia = nil
        client.resume(addresses: addresses, record: p.record)
    }

    private func resetLink() {
        stopListening()
        status = .idle; error = CoreErr.none; synced = false
        endpoints = [:]; sources = [:]; levels = [:]; selectedEndpoint = nil
        features = 0; panic = false
    }

    private func apply(_ e: CoreClient.Event) {
        switch e.type {
        case ARC_EV_STATUS:
            status = LinkStatus(rawValue: Int(e.status.rawValue)) ?? .failed
            error = Int(e.error.rawValue)
            if status == .connected, case .mixer(let t) = screen, let id = t.agentId,
               let bar = e.message.lastIndex(of: "|"), connectedVia != e.message,
               let port = UInt16(e.message[e.message.index(after: bar)...]) {
                connectedVia = e.message
                store.updateAddress(id, host: String(e.message[..<bar]), port: port)
                UserDefaults.standard.set(id, forKey: PanicRunner.lastPCKey)  // shortcuts act on this PC
            }
            // The PC stops streaming when the link drops: ask again once it is back.
            if status == .connected, listening {
                client.streamStart(mutePC: listenMutesPC)
            }
            if status == .failed, case .mixer(let t) = screen, CoreErr.invalidatesPairing(error) {
                if let id = t.agentId { store.forget(id) }
                paired = store.all()
                resetLink()
                pairingRemovedNotice = true
                screen = .computers
            }
        case ARC_EV_PAIRED:
            guard let record = e.data, case .pairing(let t) = screen,
                  let saved = store.save(record: record, host: t.host, port: t.port) else { return }
            paired = store.all()
            screen = .mixer(Target(agentId: saved.agentId, name: saved.name, host: saved.host, port: saved.port))
        case ARC_EV_STATE_RESET:
            endpoints = [:]; sources = [:]; profiles = [:]; synced = false
        case ARC_EV_PROFILE:
            profiles[e.id] = ProfileInfo(id: e.id, name: e.name, active: e.profileFlags & UInt8(ARC_PROFILE_ACTIVE) != 0,
                                         automatic: e.profileFlags & UInt8(ARC_PROFILE_AUTOMATIC) != 0,
                                         restore: e.profileFlags & UInt8(ARC_PROFILE_RESTORE) != 0, days: e.days,
                                         startMin: e.startMin, endMin: e.endMin, network: e.network, outputId: e.endpointId,
                                         outputName: e.detail, nEndpoints: e.nEndpoints, nApps: e.nApps)
        case ARC_EV_PROFILE_REMOVED:
            profiles[e.id] = nil
        case ARC_EV_CONTEXT:
            pcNetwork = e.network
        case ARC_EV_STATE_SYNCED:
            synced = true
        case ARC_EV_ENDPOINT:
            endpoints[e.id] = Endpoint(id: e.id, name: e.name, volume: e.volume, muted: e.muted, isDefault: e.isDefault,
                                       meter: Int(e.meter), input: e.input, isDefaultInput: e.isDefaultInput, battery: e.battery)
        case ARC_EV_ENDPOINT_REMOVED:
            endpoints[e.id] = nil
        case ARC_EV_SOURCE:
            sources[e.id] = Source(id: e.id, endpointId: e.endpointId, name: e.name, iconKey: e.iconKey, volume: e.volume,
                                   muted: e.muted, playing: e.playing, system: e.systemSounds, meter: Int(e.meter),
                                   route: e.route, tab: e.tab, group: e.group, detail: e.detail)
            if !e.iconKey.isEmpty, requestedIcons.insert(e.iconKey).inserted { client.requestIcon(e.iconKey) }
        case ARC_EV_SOURCE_REMOVED:
            sources[e.id] = nil
        case ARC_EV_AGENT_INFO:
            features = e.features
            panic = e.panic
        case ARC_EV_STREAM_STATE:
            // Stopped by the PC for a reason (output gone, too many listeners): say so.
            if !e.active, listening, e.error.rawValue != 0, Int(e.error.rawValue) != CoreErr.connectionLost {
                listenError = Int(e.error.rawValue)
                stopListening()
            } else if !e.active, listening, Int(e.error.rawValue) == CoreErr.connectionLost, status == .connected {
                listenError = CoreErr.connectionLost  // the captured output disappeared
                stopListening()
            }
        case ARC_EV_ICON:
            if let d = e.data, !d.isEmpty, let img = UIImage(data: d) { icons[e.iconKey] = img }
        case ARC_EV_METERS:
            guard let d = e.data else { return }
            var next = levels
            var i = d.startIndex
            while i + 2 < d.endIndex {
                next[Int(d[i]) << 8 | Int(d[i + 1])] = Double(d[i + 2]) / 255
                i += 3
            }
            levels = next
        default:
            break
        }
    }

    /// "192.168.0.10", "192.168.0.10:47321", "[fe80::1]:47321", "pc.local"
    static func parse(_ raw: String) -> (String, UInt16)? {
        let s = raw.trimmingCharacters(in: .whitespaces)
        guard !s.isEmpty else { return nil }
        if s.hasPrefix("["), let end = s.firstIndex(of: "]") {
            let host = String(s[s.index(after: s.startIndex)..<end])
            let rest = s[s.index(after: end)...].drop { $0 == ":" }
            return (host, UInt16(rest) ?? CoreClient.defaultPort)
        }
        let parts = s.split(separator: ":")
        if parts.count == 2 {
            guard let p = UInt16(parts[1]), p > 0 else { return nil }
            return (String(parts[0]), p)
        }
        return (s, CoreClient.defaultPort)
    }
}
