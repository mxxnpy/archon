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
    static let version = 2, unknown = 3, authFailed = 8, revoked = 10, connectFailed = 100
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
    @Published var pairingRemovedNotice = false
    @Published private(set) var paired: [PairedPC] = []

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
        client = CoreClient(controllerId: store.controllerId, name: UIDevice.current.name) { [weak self] ev in
            Task { @MainActor in self?.apply(ev) }
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
            endpoints = ["e1": Endpoint(id: "e1", name: "SteelSeries Sonar - Gaming", volume: 6400, muted: false, isDefault: true, meter: 1)]
            let list = [
                Source(id: "s1", endpointId: "e1", name: "Spotify", iconKey: "", volume: 7200, muted: false, playing: true, system: false, meter: 2),
                Source(id: "s2", endpointId: "e1", name: "Discord", iconKey: "", volume: 8000, muted: false, playing: true, system: false, meter: 3),
                Source(id: "s3", endpointId: "e1", name: "Counter-Strike 2", iconKey: "", volume: 5800, muted: false, playing: false, system: false, meter: 4),
                Source(id: "s4", endpointId: "e1", name: "Brave", iconKey: "", volume: 4500, muted: true, playing: false, system: false, meter: 5),
                Source(id: "s5", endpointId: "e1", name: "System sounds", iconKey: "", volume: 4000, muted: false, playing: false, system: true, meter: 6),
            ]
            sources = Dictionary(uniqueKeysWithValues: list.map { ($0.id, $0) })
            levels = [2: 0.9, 3: 0.4]
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
            return PCRow(target: Target(agentId: p.agentId, name: live?.name ?? p.name, host: live?.host ?? p.host,
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
        if case .mixer = screen { client.disconnect() }
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

    var currentEndpoint: Endpoint? {
        if let s = selectedEndpoint, let e = endpoints[s] { return e }
        return endpoints.values.first { $0.isDefault } ?? endpoints.values.min { $0.name < $1.name }
    }

    var currentSources: [Source] {
        guard let e = currentEndpoint else { return [] }
        return sources.values.filter { $0.endpointId == e.id }.sorted {
            if $0.system != $1.system { return !$0.system }
            if $0.playing != $1.playing { return $0.playing }
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
        status = .idle; error = CoreErr.none; synced = false
        endpoints = [:]; sources = [:]; levels = [:]; selectedEndpoint = nil
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
            endpoints = [:]; sources = [:]; synced = false
        case ARC_EV_STATE_SYNCED:
            synced = true
        case ARC_EV_ENDPOINT:
            endpoints[e.id] = Endpoint(id: e.id, name: e.name, volume: e.volume, muted: e.muted, isDefault: e.isDefault,
                                       meter: Int(e.meter))
        case ARC_EV_ENDPOINT_REMOVED:
            endpoints[e.id] = nil
        case ARC_EV_SOURCE:
            sources[e.id] = Source(id: e.id, endpointId: e.endpointId, name: e.name, iconKey: e.iconKey, volume: e.volume,
                                   muted: e.muted, playing: e.playing, system: e.systemSounds, meter: Int(e.meter))
            if !e.iconKey.isEmpty, requestedIcons.insert(e.iconKey).inserted { client.requestIcon(e.iconKey) }
        case ARC_EV_SOURCE_REMOVED:
            sources[e.id] = nil
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
