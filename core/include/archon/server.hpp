#pragma once
// Agent-side server: accepts controllers, runs pairing/resume, mirrors the
// audio model to every authenticated controller and applies their commands.
// OS specifics (audio API, UI, storage protection, mDNS) are injected.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "archon/protocol.hpp"

namespace arc {

// Receives audio model changes. Implementations must be thread-safe.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual void endpoint_upsert(const EndpointInfo& e) = 0;
    virtual void endpoint_removed(const std::string& id) = 0;
    virtual void source_upsert(const SourceInfo& s) = 0;
    virtual void source_removed(const std::string& id) = 0;
    // Peak levels (0..255) by endpoint or source id, only while metering is on.
    virtual void meters(const std::vector<std::pair<std::string, uint8_t>>& levels) { (void)levels; }
};

// OS audio integration. Command methods are called from the server thread.
class AudioBackend {
public:
    virtual ~AudioBackend() = default;
    // Starts monitoring; must report the complete current state to `sink`.
    virtual bool start(AudioSink& sink, std::string* err) = 0;
    virtual void stop() = 0;
    virtual void set_endpoint_volume(const std::string& id, uint16_t volume) = 0;
    virtual void set_endpoint_mute(const std::string& id, bool muted) = 0;
    virtual void set_source_volume(const std::string& id, uint16_t volume) = 0;
    virtual void set_source_mute(const std::string& id, bool muted) = 0;
    // PNG for an icon key announced in SourceInfo. False if unavailable.
    virtual bool icon_png(const std::string& key, Bytes& png) = 0;
    // Start/stop sampling peak levels (~20 Hz) and reporting them via AudioSink::meters.
    // Only on while at least one controller shows the mixer, to save CPU and battery.
    virtual void set_metering(bool on) { (void)on; }
};

struct PairedController {
    Id16 id{};
    std::string name;
    Key32 key{};
    int64_t paired_at = 0;  // unix seconds
    int64_t last_seen = 0;
};

// Persistent list of paired controllers. Implementations must be thread-safe.
class TrustStore {
public:
    virtual ~TrustStore() = default;
    virtual std::optional<PairedController> find(const Id16& id) = 0;
    virtual bool put(const PairedController& c) = 0;
    virtual bool remove(const Id16& id) = 0;
    virtual std::vector<PairedController> list() = 0;
};

struct ConnectedController {
    uint64_t conn_id = 0;
    Id16 id{};
    std::string name;
    std::string address;
};

// Callbacks run on the server thread; UIs must marshal to their own thread.
class ServerObserver {
public:
    virtual ~ServerObserver() = default;
    virtual void on_pair_request(uint64_t /*conn_id*/, const std::string& /*controller_name*/,
                                 const std::string& /*code*/) {}
    virtual void on_pair_finished(uint64_t /*conn_id*/, bool /*success*/,
                                  const std::string& /*controller_name*/) {}
    virtual void on_controllers_changed(const std::vector<ConnectedController>& /*now*/) {}
    virtual void on_log(const std::string& /*line*/) {}
    // Pairing lockout state changed (persist it so restarts don't reset escalation).
    virtual void on_pairing_lockout(int /*lockouts*/, int64_t /*locked_until_unix*/) {}
};

struct ServerConfig {
    Id16 agent_id{};
    std::string agent_name;
    uint16_t port = kDefaultPort;
    bool loopback_only = false;
    size_t max_controllers = 8;
    size_t max_sockets = 32;
    int hello_timeout_ms = 5000;       // a new socket must say HELLO this fast
    int handshake_timeout_ms = 10000;
    int code_timeout_ms = 120000;
    int idle_timeout_ms = 15000;
    int max_failed_pairings = 5;
    int failure_window_ms = 10 * 60 * 1000;
    int lockout_ms = 5 * 60 * 1000;    // doubles on every consecutive lockout (max 24 h)
    size_t max_output_buffer = 1 << 20;
    // Unauthenticated sockets can't starve real controllers: beyond these
    // limits the oldest pending handshake is evicted to make room.
    size_t max_pending_handshakes = 16;
    size_t max_pending_per_address = 4;
    // Pairing-request flood control (popups on the PC).
    int pair_request_interval_ms = 20000;  // per source address (≤ 3/min from one host)
    int max_pair_requests_per_minute = 8;  // all sources: one host can't exhaust it
    // Lockout escalation survives restarts: the agent persists these via
    // ServerObserver::on_pairing_lockout and passes them back here.
    int initial_lockouts = 0;
    int64_t locked_until_unix = 0;
    // Refuse peers that are not on the local network (see net::is_local_network_address).
    bool lan_only = true;
};

class Server final : public AudioSink {
public:
    Server(ServerConfig cfg, AudioBackend& backend, TrustStore& store, ServerObserver& observer);
    ~Server() override;
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    bool start(std::string* err);
    void stop();
    uint16_t port() const { return port_; }

    // Thread-safe controls (usually called from the UI thread).
    void reject_pairing(uint64_t conn_id);
    void revoke(const Id16& controller_id);

    // AudioSink (thread-safe)
    void endpoint_upsert(const EndpointInfo& e) override;
    void endpoint_removed(const std::string& id) override;
    void source_upsert(const SourceInfo& s) override;
    void source_removed(const std::string& id) override;
    void meters(const std::vector<std::pair<std::string, uint8_t>>& levels) override;

private:
    struct Conn;
    struct Impl;
    void post(std::function<void()> fn);

    ServerConfig cfg_;
    AudioBackend& backend_;
    TrustStore& store_;
    ServerObserver& observer_;
    std::unique_ptr<Impl> impl_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    uint16_t port_ = 0;
};

}  // namespace arc
