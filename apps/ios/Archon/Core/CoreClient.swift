import Foundation

/// Swift face of the C++ core (archon.h). Events arrive on a native network
/// thread; `onEvent` is invoked there and must hop to the main actor itself.
final class CoreClient {
    struct Event {
        var type: arc_event_type
        var status: arc_status
        var error: arc_error
        var message: String
        var id: String
        var endpointId: String
        var name: String
        var iconKey: String
        var pid: UInt32
        var volume: Int
        var muted: Bool
        var isDefault: Bool
        var playing: Bool
        var systemSounds: Bool
        var meter: UInt16
        var data: Data?
        // 1.2
        var input = false
        var isDefaultInput = false
        var route = ""
        var features: UInt32 = 0
        var panic = false
        // 1.3
        var active = false
        var lost = false
        var seq: UInt64 = 0
        // 1.4
        var tab = false
        var group = ""
        var detail = ""
    }

    static let featureInputs = UInt32(ARC_FEATURE_INPUTS)
    static let featureRouting = UInt32(ARC_FEATURE_ROUTING)
    static let featurePanic = UInt32(ARC_FEATURE_PANIC)
    static let featureStream = UInt32(ARC_FEATURE_STREAM)
    static let featureTabs = UInt32(ARC_FEATURE_TABS)

    static let maxVolume = Int(ARC_MAX_VOLUME)
    static let defaultPort = UInt16(ARC_DEFAULT_PORT)
    static let serviceType = "_archon._tcp"

    private var handle: OpaquePointer?
    private let onEvent: (Event) -> Void

    init(controllerId: Data, name: String, onEvent: @escaping (Event) -> Void) {
        self.onEvent = onEvent
        let user = Unmanaged.passUnretained(self).toOpaque()
        handle = controllerId.withUnsafeBytes { raw in
            name.withCString { cname in
                arc_client_create(raw.bindMemory(to: UInt8.self).baseAddress, cname, { user, ev in
                    guard let user, let ev else { return }
                    let me = Unmanaged<CoreClient>.fromOpaque(user).takeUnretainedValue()
                    me.onEvent(CoreClient.copy(ev.pointee))
                }, user)
            }
        }
    }

    deinit { arc_client_destroy(handle) }  // joins the network thread first

    func pair(host: String, port: UInt16) { arc_client_pair(handle, host, port) }

    @discardableResult
    func resume(host: String, port: UInt16, record: Data) -> Bool {
        record.withUnsafeBytes { raw in
            arc_client_resume(handle, host, port, raw.bindMemory(to: UInt8.self).baseAddress, raw.count) != 0
        }
    }

    /// Tries each address in turn; on CONNECTED the status message is "host|port".
    @discardableResult
    func resume(addresses: [(String, UInt16)], record: Data) -> Bool {
        let cstrs = addresses.map { strdup($0.0) }
        defer { cstrs.forEach { free($0) } }
        let hosts: [UnsafePointer<CChar>?] = cstrs.map { UnsafePointer($0) }
        let ports = addresses.map { $0.1 }
        return record.withUnsafeBytes { raw in
            hosts.withUnsafeBufferPointer { hp in
                ports.withUnsafeBufferPointer { pp in
                    arc_client_resume_multi(handle, hp.baseAddress, pp.baseAddress, addresses.count,
                                            raw.bindMemory(to: UInt8.self).baseAddress, raw.count) != 0
                }
            }
        }
    }

    func submit(code: String) { arc_client_submit_code(handle, code) }
    func disconnect() { arc_client_disconnect(handle) }

    func setVolume(endpoint: Bool, id: String, volume: Int) {
        let v = UInt16(max(0, min(CoreClient.maxVolume, volume)))
        if endpoint { arc_client_set_endpoint_volume(handle, id, v) } else { arc_client_set_source_volume(handle, id, v) }
    }

    func setMute(endpoint: Bool, id: String, muted: Bool) {
        if endpoint { arc_client_set_endpoint_mute(handle, id, muted ? 1 : 0) } else { arc_client_set_source_mute(handle, id, muted ? 1 : 0) }
    }

    func requestIcon(_ key: String) { arc_client_request_icon(handle, key) }
    func refresh() { arc_client_refresh(handle) }
    func setMeters(_ on: Bool) { arc_client_set_meters(handle, on ? 1 : 0) }
    /// Send an app to an output ("" = follow the PC's default).
    func setRoute(source: String, endpoint: String) { arc_client_set_source_route(handle, source, endpoint) }
    /// Mute every device on the PC / undo. Queued until the link is up.
    func panicMute(_ on: Bool) { arc_client_panic_mute(handle, on ? 1 : 0) }
    /// Listen to the PC on this phone; audio then arrives as ARC_EV_AUDIO.
    @discardableResult
    func streamStart(mutePC: Bool) -> Bool { arc_client_stream_start(handle, mutePC ? UInt32(ARC_STREAM_MUTE_PC) : 0, nil) != 0 }
    func streamStop() { arc_client_stream_stop(handle) }

    static func generateId() -> Data {
        var id = [UInt8](repeating: 0, count: 16)
        arc_generate_controller_id(&id)
        return Data(id)
    }

    /// (agent id hex, PC name) stored in a pairing record.
    static func recordInfo(_ record: Data) -> (agentId: String, name: String)? {
        var id = [UInt8](repeating: 0, count: 16)
        var name = [CChar](repeating: 0, count: 128)
        let ok = record.withUnsafeBytes { raw in
            arc_record_info(raw.bindMemory(to: UInt8.self).baseAddress, raw.count, &id, &name, name.count)
        }
        guard ok != 0 else { return nil }
        return (id.map { String(format: "%02x", $0) }.joined(), String(cString: name))
    }

    private static func s(_ p: UnsafePointer<CChar>?) -> String { p.map { String(cString: $0) } ?? "" }

    private static func copy(_ e: arc_event) -> Event {
        Event(type: e.type, status: e.status, error: e.error, message: s(e.message), id: s(e.id),
              endpointId: s(e.endpoint_id), name: s(e.name), iconKey: s(e.icon_key), pid: e.pid,
              volume: Int(e.volume), muted: e.muted != 0, isDefault: e.is_default != 0, playing: e.playing != 0,
              systemSounds: e.system_sounds != 0, meter: e.meter,
              data: e.data.map { Data(bytes: $0, count: e.data_len) },
              input: e.input != 0, isDefaultInput: e.is_default_input != 0, route: s(e.route),
              features: e.features, panic: e.panic != 0,
              active: e.active != 0, lost: e.lost != 0, seq: e.seq,
              tab: e.tab != 0, group: s(e.group), detail: s(e.detail))
    }
}
