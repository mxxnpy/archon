import Foundation
import Network

struct DiscoveredPC: Identifiable, Equatable {
    let agentId: String
    let name: String
    let host: String
    let port: UInt16
    var id: String { agentId }
}

/// Bonjour browsing for `_archon._tcp` with Network.framework (no multicast
/// entitlement needed). Each result is resolved to an IPv4 host:port by
/// opening a short-lived connection and reading its remote endpoint.
@MainActor
final class Discovery: ObservableObject {
    @Published private(set) var found: [String: DiscoveredPC] = [:]
    @Published private(set) var searching = false
    @Published private(set) var permissionDenied = false

    private var browser: NWBrowser?
    private var resolving: [NWEndpoint: NWConnection] = [:]
    private var byEndpoint: [NWEndpoint: String] = [:]

    func start() {
        guard browser == nil else { return }
        let b = NWBrowser(for: .bonjourWithTXTRecord(type: CoreClient.serviceType, domain: nil), using: .tcp)
        b.stateUpdateHandler = { [weak self] state in
            Task { @MainActor in
                guard let self else { return }
                switch state {
                case .ready: self.searching = true; self.permissionDenied = false
                case .waiting(let err):
                    self.searching = false
                    // Local Network permission declined shows up as a DNS-SD policy error.
                    if case .dns(let code) = err, code == -65570 { self.permissionDenied = true }
                case .failed, .cancelled: self.searching = false
                default: break
                }
            }
        }
        b.browseResultsChangedHandler = { [weak self] _, changes in
            Task { @MainActor in self?.apply(changes) }
        }
        b.start(queue: .main)
        browser = b
    }

    func stop() {
        browser?.cancel()
        browser = nil
        resolving.values.forEach { $0.cancel() }
        resolving.removeAll()
        searching = false
    }

    private func apply(_ changes: Set<NWBrowser.Result.Change>) {
        for change in changes {
            switch change {
            case .added(let r): resolve(r)
            case .changed(_, let r, _): resolve(r)
            case .removed(let r):
                if let id = byEndpoint.removeValue(forKey: r.endpoint) { found[id] = nil }
            default: break
            }
        }
    }

    private func resolve(_ result: NWBrowser.Result) {
        guard case .bonjour(let txt) = result.metadata, let id = txt["id"]?.lowercased(), id.count == 32,
              case .service(let name, _, _, _) = result.endpoint, resolving[result.endpoint] == nil else { return }
        let params = NWParameters.tcp
        if let ip = params.defaultProtocolStack.internetProtocol as? NWProtocolIP.Options { ip.version = .v4 }
        let conn = NWConnection(to: result.endpoint, using: params)
        resolving[result.endpoint] = conn
        conn.stateUpdateHandler = { [weak self, weak conn] state in
            Task { @MainActor in
                guard let self, let conn else { return }
                switch state {
                case .ready:
                    if case .hostPort(let host, let port) = conn.currentPath?.remoteEndpoint {
                        let h: String
                        switch host {
                        case .ipv4(let a): h = "\(a)"
                        case .ipv6(let a): h = "\(a)"
                        case .name(let n, _): h = n
                        @unknown default: h = "\(host)"
                        }
                        self.byEndpoint[result.endpoint] = id
                        self.found[id] = DiscoveredPC(agentId: id, name: name, host: h, port: port.rawValue)
                    }
                    conn.cancel()
                case .failed, .cancelled:
                    self.resolving[result.endpoint] = nil
                default: break
                }
            }
        }
        conn.start(queue: .main)
    }
}
