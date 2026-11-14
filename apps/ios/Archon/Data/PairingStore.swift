import Foundation
import Security

struct PairedPC: Identifiable, Equatable {
    let agentId: String
    var name: String
    var host: String
    var port: UInt16
    let record: Data
    var id: String { agentId }
}

/// Pairing records live in the Keychain (this device only, after first unlock);
/// they are never synced to iCloud or included in backups to other devices.
final class PairingStore {
    private let service = "dev.archon.pairing"
    private let defaults = UserDefaults.standard

    /// Stable random id of this phone.
    /// Kept in the Keychain (this device only) so backups restored on another
    /// phone don't carry this phone's identity.
    lazy var controllerId: Data = {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                   kSecAttrService as String: "dev.archon.identity",
                                   kSecAttrAccount as String: "controllerId"]
        var out: CFTypeRef?
        let st = SecItemCopyMatching(base.merging([kSecReturnData as String: true]) { $1 } as CFDictionary, &out)
        if st == errSecSuccess, let d = out as? Data, d.count == 16 { return d }
        let legacy = defaults.data(forKey: "controllerId").flatMap { $0.count == 16 ? $0 : nil }
        // Keychain unavailable (e.g. locked before first unlock): use an id for
        // this run only and don't touch storage, so the real identity isn't replaced.
        guard st == errSecItemNotFound else { return legacy ?? CoreClient.generateId() }
        // The pre-Keychain id lived in UserDefaults, which backups carry to other
        // phones. Adopt it only on the phone that made it: pairing keys are
        // ThisDeviceOnly, so having any proves this is the same device.
        let id = (legacy != nil && hasPairings()) ? legacy! : CoreClient.generateId()
        let add = base.merging([kSecValueData as String: id,
                                kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]) { $1 }
        if SecItemAdd(add as CFDictionary, nil) == errSecSuccess { defaults.removeObject(forKey: "controllerId") }
        return id
    }()

    private func hasPairings() -> Bool {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecMatchLimit as String: kSecMatchLimitOne]
        return SecItemCopyMatching(q as CFDictionary, nil) == errSecSuccess
    }

    func all() -> [PairedPC] {
        let q: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecMatchLimit as String: kSecMatchLimitAll,
            kSecReturnAttributes as String: true,
            kSecReturnData as String: true,
        ]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess,
              let items = out as? [[String: Any]] else { return [] }
        return items.compactMap(decode).sorted { $0.name.lowercased() < $1.name.lowercased() }
    }

    func load(_ agentId: String) -> PairedPC? { all().first { $0.agentId == agentId } }

    @discardableResult
    func save(record: Data, host: String, port: UInt16) -> PairedPC? {
        guard let (agentId, name) = CoreClient.recordInfo(record) else { return nil }
        let pc = PairedPC(agentId: agentId, name: name, host: host, port: port, record: record)
        return write(pc) ? pc : nil
    }

    func updateAddress(_ agentId: String, host: String, port: UInt16) {
        guard var pc = load(agentId), pc.host != host || pc.port != port else { return }
        pc.host = host
        pc.port = port
        write(pc)
    }

    func forget(_ agentId: String) {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecAttrAccount as String: agentId]
        SecItemDelete(q as CFDictionary)
    }

    /// Update in place when the item exists (never delete-then-add: a failed add
    /// would lose the pairing), add otherwise. Returns false if the Keychain refused.
    @discardableResult
    private func write(_ pc: PairedPC) -> Bool {
        let meta = (try? JSONSerialization.data(withJSONObject: ["n": pc.name, "h": pc.host, "p": Int(pc.port)])) ?? Data()
        let query: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                    kSecAttrService as String: service,
                                    kSecAttrAccount as String: pc.agentId]
        let attrs: [String: Any] = [kSecAttrGeneric as String: meta, kSecValueData as String: pc.record,
                                    kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly]
        let st = SecItemUpdate(query as CFDictionary, attrs as CFDictionary)
        if st == errSecSuccess { return true }
        guard st == errSecItemNotFound else { return false }
        return SecItemAdd(query.merging(attrs) { $1 } as CFDictionary, nil) == errSecSuccess
    }

    private func decode(_ item: [String: Any]) -> PairedPC? {
        guard let agentId = item[kSecAttrAccount as String] as? String,
              let record = item[kSecValueData as String] as? Data,
              let meta = item[kSecAttrGeneric as String] as? Data,
              let o = try? JSONSerialization.jsonObject(with: meta) as? [String: Any] else { return nil }
        return PairedPC(agentId: agentId, name: o["n"] as? String ?? "PC", host: o["h"] as? String ?? "",
                        port: UInt16(o["p"] as? Int ?? Int(CoreClient.defaultPort)), record: record)
    }
}
