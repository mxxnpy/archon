#pragma once
// Shared fixtures for the adversarial suite: a fake backend, an in-memory trust
// store, an observing UI, a client listener and a RawPeer that speaks the wire
// protocol by hand over a raw socket (so tests can misbehave on purpose).

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "../test.hpp"
#include "archon/client.hpp"
#include "archon/crypto.hpp"
#include "archon/frame.hpp"
#include "archon/net.hpp"
#include "archon/server.hpp"

namespace adv {

using namespace arc;
using namespace std::chrono_literals;
using SClock = std::chrono::steady_clock;

inline int64_t ms_since(SClock::time_point t0) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(SClock::now() - t0).count();
}

template <class Pred>
inline bool wait_for(std::mutex& mu, Pred pred, int timeout_ms = 5000) {
    const auto until = SClock::now() + std::chrono::milliseconds(timeout_ms);
    while (SClock::now() < until) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (pred()) return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    std::lock_guard<std::mutex> lk(mu);
    return pred();
}

template <class Pred>
inline bool wait_until(Pred pred, int timeout_ms = 5000) {
    const auto until = SClock::now() + std::chrono::milliseconds(timeout_ms);
    while (SClock::now() < until) {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

class FakeBackend : public AudioBackend {
public:
    std::mutex mu;
    AudioSink* sink = nullptr;
    std::map<std::string, EndpointInfo> endpoints;
    std::map<std::string, SourceInfo> sources;
    int commands = 0;
    bool metering = false;
    int metering_changes = 0;

    FakeBackend() {
        endpoints["E1"] = EndpointInfo{"E1", "Speakers", 5000, false, true};
        SourceInfo s;
        s.id = "E1|100";
        s.endpoint_id = "E1";
        s.name = "Player";
        s.icon_key = "icon1";
        s.pid = 100;
        s.volume = 8000;
        s.playing = true;
        sources[s.id] = s;
    }
    bool start(AudioSink& snk, std::string*) override {
        std::lock_guard<std::mutex> lk(mu);
        sink = &snk;
        for (auto& [id, e] : endpoints) sink->endpoint_upsert(e);
        for (auto& [id, s] : sources) sink->source_upsert(s);
        return true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lk(mu);
        sink = nullptr;
    }
    void set_endpoint_volume(const std::string& id, uint16_t v) override {
        std::lock_guard<std::mutex> lk(mu);
        ++commands;
        auto it = endpoints.find(id);
        if (it == endpoints.end()) return;
        it->second.volume = v;
        if (sink) sink->endpoint_upsert(it->second);
    }
    void set_endpoint_mute(const std::string& id, bool m) override {
        std::lock_guard<std::mutex> lk(mu);
        ++commands;
        auto it = endpoints.find(id);
        if (it == endpoints.end()) return;
        it->second.muted = m;
        if (sink) sink->endpoint_upsert(it->second);
    }
    void set_source_volume(const std::string& id, uint16_t v) override {
        std::lock_guard<std::mutex> lk(mu);
        ++commands;
        auto it = sources.find(id);
        if (it == sources.end()) return;
        it->second.volume = v;
        if (sink) sink->source_upsert(it->second);
    }
    void set_source_mute(const std::string& id, bool m) override {
        std::lock_guard<std::mutex> lk(mu);
        ++commands;
        auto it = sources.find(id);
        if (it == sources.end()) return;
        it->second.muted = m;
        if (sink) sink->source_upsert(it->second);
    }
    void set_metering(bool on) override {
        std::lock_guard<std::mutex> lk(mu);
        metering = on;
        ++metering_changes;
    }
    bool icon_png(const std::string& key, Bytes& png) override {
        if (key != "icon1") return false;
        png = Bytes{0x89, 'P', 'N', 'G', 1, 2, 3};
        return true;
    }
    int command_count() {
        std::lock_guard<std::mutex> lk(mu);
        return commands;
    }
    bool is_metering() {
        std::lock_guard<std::mutex> lk(mu);
        return metering;
    }
};

class MemoryStore : public TrustStore {
public:
    std::mutex mu;
    std::map<Id16, PairedController> items;
    std::optional<PairedController> find(const Id16& id) override {
        std::lock_guard<std::mutex> lk(mu);
        auto it = items.find(id);
        if (it == items.end()) return std::nullopt;
        return it->second;
    }
    bool put(const PairedController& c) override {
        std::lock_guard<std::mutex> lk(mu);
        items[c.id] = c;
        return true;
    }
    bool remove(const Id16& id) override {
        std::lock_guard<std::mutex> lk(mu);
        return items.erase(id) > 0;
    }
    std::vector<PairedController> list() override {
        std::lock_guard<std::mutex> lk(mu);
        std::vector<PairedController> v;
        for (auto& [k, c] : items) v.push_back(c);
        return v;
    }
};

class Observer : public ServerObserver {
public:
    std::mutex mu;
    std::string code;
    uint64_t conn = 0;
    int requests = 0;
    int finished_ok = 0;
    int finished_fail = 0;
    size_t connected = 0;
    size_t max_connected = 0;
    std::vector<std::string> logs;
    void on_pair_request(uint64_t c, const std::string&, const std::string& cd) override {
        std::lock_guard<std::mutex> lk(mu);
        conn = c;
        code = cd;
        ++requests;
    }
    void on_pair_finished(uint64_t, bool ok, const std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        ok ? ++finished_ok : ++finished_fail;
    }
    void on_controllers_changed(const std::vector<ConnectedController>& now) override {
        std::lock_guard<std::mutex> lk(mu);
        connected = now.size();
        if (connected > max_connected) max_connected = connected;
    }
    void on_log(const std::string& line) override {
        std::lock_guard<std::mutex> lk(mu);
        if (logs.size() < 2000) logs.push_back(line);
    }
    size_t connected_now() {
        std::lock_guard<std::mutex> lk(mu);
        return connected;
    }
    bool logged(const std::string& needle) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto& l : logs)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
};

class Listener : public ClientListener {
public:
    std::mutex mu;
    ClientStatus status = ClientStatus::Idle;
    ErrorCode error = ErrorCode::None;
    std::vector<ClientStatus> history;
    std::vector<ErrorCode> errors;  // parallel to history
    std::optional<PairingRecord> record;
    std::map<std::string, EndpointInfo> endpoints;
    std::map<std::string, SourceInfo> sources;
    int synced = 0;
    int resets = 0;  // dumps started; a dump is in progress while resets > synced
    int readies = 0;
    std::map<uint16_t, uint8_t> levels;
    std::atomic<int> events{0};

    void on_status(ClientStatus s, ErrorCode e, const std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        status = s;
        error = e;
        history.push_back(s);
        errors.push_back(e);
    }
    // First link-ending status after `from` (Failed, or Reconnecting carrying a reason).
    std::optional<ErrorCode> first_end_reason(size_t from) {
        std::lock_guard<std::mutex> lk(mu);
        for (size_t i = from; i < history.size(); ++i)
            if (history[i] == ClientStatus::Failed ||
                (history[i] == ClientStatus::Reconnecting && errors[i] != ErrorCode::None))
                return errors[i];
        return std::nullopt;
    }
    void on_event(const ControllerEvent& ev) override {
        ++events;
        std::lock_guard<std::mutex> lk(mu);
        using K = ControllerEvent::Kind;
        switch (ev.kind) {
            case K::Paired: record = ev.pairing; break;
            case K::Ready: ++readies; break;
            case K::StateReset: endpoints.clear(); sources.clear(); ++resets; break;
            case K::StateSynced: ++synced; break;
            case K::Endpoint: endpoints[ev.endpoint.id] = ev.endpoint; break;
            case K::EndpointRemoved: endpoints.erase(ev.id); break;
            case K::Source: sources[ev.source.id] = ev.source; break;
            case K::SourceRemoved: sources.erase(ev.id); break;
            case K::Meters:
                for (auto& m : ev.meters) levels[m.slot] = m.level;
                break;
            default: break;
        }
    }
};

inline ClientTiming fast_timing() {
    ClientTiming t;
    t.backoff_min_ms = 50;
    t.backoff_max_ms = 200;
    return t;
}

struct AdvRig {
    FakeBackend backend;
    MemoryStore own_store;
    TrustStore* store;
    Observer observer;
    std::unique_ptr<Server> server;
    ServerConfig cfg;
    uint16_t port = 0;

    explicit AdvRig(ServerConfig c = {}, uint16_t fixed_port = 0, const Id16* agent = nullptr,
                    TrustStore* shared = nullptr)
        : store(shared ? shared : &own_store), cfg(c) {
        cfg.agent_id = agent ? *agent : random_array<16>();
        cfg.agent_name = "TEST-PC";
        cfg.loopback_only = true;
        // Every raw peer comes from 127.0.0.1: lift pairing flood control here
        // (it has its own test in test_integration.cpp).
        cfg.pair_request_interval_ms = 0;
        cfg.max_pair_requests_per_minute = 100000;
        cfg.port = fixed_port;
        server = std::make_unique<Server>(cfg, backend, *store, observer);
        std::string err;
        for (int i = 0; i < 50; ++i) {
            if (server->start(&err)) {
                port = server->port();
                break;
            }
            std::this_thread::sleep_for(100ms);
        }
    }
    // Registers a paired controller with a known key (as if pairing happened earlier).
    Key32 provision(const Id16& cid, const std::string& name = "raw") {
        PairedController pc;
        pc.id = cid;
        pc.name = name;
        pc.key = random_array<32>();
        store->put(pc);
        return pc.key;
    }
    PairingRecord record_for(const Key32& key) const {
        PairingRecord r;
        r.agent_id = cfg.agent_id;
        r.agent_name = cfg.agent_name;
        r.pair_key = key;
        return r;
    }
};

enum class Rx { Frame, Closed, Timeout, BadFrame, BadMac };

// A hand-driven peer. Every method has a timeout; nothing blocks forever.
struct RawPeer {
    net::socket_t fd = net::kInvalidSocket;
    FrameAssembler in;
    SecureChannel ch;
    bool secure = false;
    Id16 cid{};

    RawPeer() = default;
    RawPeer(const RawPeer&) = delete;
    RawPeer& operator=(const RawPeer&) = delete;
    ~RawPeer() { close(); }

    bool connect(uint16_t port) {
        std::string err;
        fd = net::connect_tcp("127.0.0.1", port, 2000, &err);
        return fd != net::kInvalidSocket;
    }
    void close() {
        if (fd != net::kInvalidSocket) net::close_socket(fd);
        fd = net::kInvalidSocket;
    }
    // Abortive close: RST instead of FIN (peer vanishes mid-stream).
    void abort_rst() {
        if (fd == net::kInvalidSocket) return;
        linger l{};
        l.l_onoff = 1;
        l.l_linger = 0;
        setsockopt(fd, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&l), sizeof l);
        close();
    }
    void half_close() {
#if defined(_WIN32)
        shutdown(fd, SD_SEND);
#else
        shutdown(fd, SHUT_WR);
#endif
    }
    void small_rcvbuf(int bytes) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&bytes), sizeof bytes);
    }

    bool send_all(const uint8_t* p, size_t n, int timeout_ms = 3000) {
        const auto until = SClock::now() + std::chrono::milliseconds(timeout_ms);
        size_t off = 0;
        while (off < n) {
            long w = net::send_some(fd, p + off, n - off);
            if (w > 0) {
                off += size_t(w);
                continue;
            }
            if (w != net::kWouldBlock) return false;
            if (SClock::now() >= until) return false;
            std::vector<net::PollItem> items{{fd, false, true}};
            net::poll(items, 20);
        }
        return true;
    }
    bool send_bytes(const Bytes& b, int timeout_ms = 3000) { return send_all(b.data(), b.size(), timeout_ms); }
    bool send_frame(const Bytes& payload) { return send_bytes(make_frame(payload)); }
    bool send_app(const Bytes& msg) { return send_frame(ch.seal(msg)); }

    Rx recv_frame(Bytes& out, int timeout_ms) {
        const auto until = SClock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            auto res = in.next(out);
            if (res == FrameAssembler::Result::Frame) return Rx::Frame;
            if (res == FrameAssembler::Result::Error) return Rx::BadFrame;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - SClock::now()).count();
            if (left <= 0) return Rx::Timeout;
            std::vector<net::PollItem> items{{fd, true, false}};
            net::poll(items, int(std::min<long long>(left, 50)));
            if (!items[0].readable && !items[0].failed) continue;
            uint8_t buf[16384];
            long n = net::recv_some(fd, buf, sizeof buf);
            if (n == net::kWouldBlock) continue;
            if (n <= 0) return Rx::Closed;
            in.feed(buf, size_t(n));
        }
    }
    // Next message (decrypted when the channel is up).
    Rx recv_msg(Bytes& msg, int timeout_ms) {
        Bytes payload;
        Rx r = recv_frame(payload, timeout_ms);
        if (r != Rx::Frame) return r;
        if (!secure) {
            msg = std::move(payload);
            return msg.empty() ? Rx::BadFrame : Rx::Frame;
        }
        if (!ch.open(payload.data(), payload.size(), msg) || msg.empty()) return Rx::BadMac;
        return Rx::Frame;
    }
    // Reads until an ERROR message arrives (returns its code) or the link closes / times out.
    std::optional<ErrorCode> wait_error(int timeout_ms, std::vector<uint8_t>* seen_types = nullptr) {
        const auto until = SClock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - SClock::now()).count();
            if (left <= 0) return std::nullopt;
            Bytes m;
            Rx r = recv_msg(m, int(left));
            if (r != Rx::Frame) return std::nullopt;
            if (seen_types) seen_types->push_back(m[0]);
            if (m[0] == uint8_t(MsgType::Error)) {
                Reader rd(m.data() + 1, m.size() - 1);
                ErrorMsg e;
                if (!decode_error(rd, e)) return ErrorCode::None;
                return e.code;
            }
        }
    }
    // True when the server closed the link within the timeout (frames are discarded).
    bool wait_closed(int timeout_ms) {
        const auto until = SClock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - SClock::now()).count();
            if (left <= 0) return false;
            Bytes p;
            Rx r = recv_frame(p, int(left));
            if (r == Rx::Closed) return true;
            if (r == Rx::Timeout) return false;
            if (r == Rx::BadFrame) return false;
        }
    }
    // Waits for a message of a given type (decrypting if secure); other messages are skipped.
    bool wait_type(MsgType t, int timeout_ms, Bytes* out = nullptr) {
        const auto until = SClock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(until - SClock::now()).count();
            if (left <= 0) return false;
            Bytes m;
            if (recv_msg(m, int(left)) != Rx::Frame) return false;
            if (m[0] == uint8_t(t)) {
                if (out) *out = std::move(m);
                return true;
            }
        }
    }

    // ---- handshake helpers --------------------------------------------------------
    Bytes hello_pair(const Id16& id, const std::string& name = "raw") {
        cid = id;
        Hello h;
        h.mode = Mode::Pair;
        h.controller_id = id;
        h.name = name;
        return encode_hello(h);
    }
    Bytes hello_resume(const Id16& id, const Key32& nonce_c, const std::string& name = "raw") {
        cid = id;
        Hello h;
        h.mode = Mode::Resume;
        h.controller_id = id;
        h.name = name;
        h.nonce = nonce_c;
        return encode_hello(h);
    }
    // Phase 1 of resume: sends HELLO, returns the agent's ack (nullopt on error/close).
    std::optional<HelloAck> resume_hello(const Id16& id, const Key32& nonce_c, int timeout_ms = 2000) {
        if (!send_frame(hello_resume(id, nonce_c))) return std::nullopt;
        Bytes m;
        if (recv_msg(m, timeout_ms) != Rx::Frame || m[0] != uint8_t(MsgType::HelloAck)) return std::nullopt;
        Reader r(m.data() + 1, m.size() - 1);
        HelloAck a;
        if (!decode_hello_ack(r, a)) return std::nullopt;
        return a;
    }
    // Full resume with a known pair key. Leaves the channel up; optionally reads the dump.
    bool resume(const Id16& id, const Key32& key, const Id16& agent_id, bool read_dump = true) {
        const Key32 nonce_c = random_array<32>();
        auto ack = resume_hello(id, nonce_c);
        if (!ack) return false;
        SessionKeys keys = derive_resume_keys(key, id, agent_id, nonce_c, ack->nonce, "raw", ack->name);
        if (!ct_equal(keys.tag_agent, ack->tag)) return false;
        if (!send_frame(encode_resume_confirm(keys.tag_controller))) return false;
        ch.init(keys.key_c2a, keys.key_a2c);
        secure = true;
        if (read_dump) return wait_type(MsgType::StateSynced, 3000);
        return true;
    }
};

// Pairing math done by hand on the controller side.
struct PairMath {
    Key32 yc{};
    Key32 tag{};
    Key32 key_c2a{}, key_a2c{}, tag_agent{};
};
inline PairMath pair_math(const std::string& code, const HelloAck& ack, const Id16& cid) {
    PairMath pm;
    const Key32 g = cpace_generator(code, ack.sid, ack.agent_id, cid);
    CpaceParty me;
    cpace_start(g, me);
    Key32 k;
    cpace_finish(me, ack.y, k);
    SessionKeys keys = derive_pair_keys(k, ack.sid, ack.agent_id, cid, ack.y, me.y, "raw", ack.name);
    pm.yc = me.y;
    pm.tag = keys.tag_controller;
    pm.key_c2a = keys.key_c2a;
    pm.key_a2c = keys.key_a2c;
    pm.tag_agent = keys.tag_agent;
    return pm;
}

inline std::optional<HelloAck> decode_ack_msg(const Bytes& m) {
    if (m.empty() || m[0] != uint8_t(MsgType::HelloAck)) return std::nullopt;
    Reader r(m.data() + 1, m.size() - 1);
    HelloAck a;
    if (!decode_hello_ack(r, a)) return std::nullopt;
    return a;
}

}  // namespace adv
