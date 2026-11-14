// End-to-end tests: real Server and real Client over loopback TCP, with a
// fake audio backend and an in-memory trust store.

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>

#include "archon/client.hpp"
#include "archon/crypto.hpp"
#include "archon/frame.hpp"
#include "archon/net.hpp"
#include "archon/server.hpp"
#include "test.hpp"

using namespace arc;
using namespace std::chrono_literals;

namespace {

template <class Pred>
bool wait_for(std::mutex& mu, Pred pred, int timeout_ms = 5000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < until) {
        {
            std::lock_guard<std::mutex> lk(mu);
            if (pred()) return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    std::lock_guard<std::mutex> lk(mu);
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

    FakeBackend() {
        endpoints["E1"] = EndpointInfo{"E1", "Alto-falantes", 5000, false, true};
        SourceInfo s;
        s.id = "E1|100";
        s.endpoint_id = "E1";
        s.name = "Spotify";
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
    // Like the OS: apply, then notify (the notification reaches all controllers).
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
    }
    bool icon_png(const std::string& key, Bytes& png) override {
        if (key != "icon1") return false;
        png = Bytes{0x89, 'P', 'N', 'G', 1, 2, 3};
        return true;
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
    }
};

class Listener : public ClientListener {
public:
    std::mutex mu;
    ClientStatus status = ClientStatus::Idle;
    ErrorCode error = ErrorCode::None;
    std::vector<ClientStatus> history;
    std::optional<PairingRecord> record;
    std::map<std::string, EndpointInfo> endpoints;
    std::map<std::string, SourceInfo> sources;
    int synced = 0;
    std::map<std::string, Bytes> icons;
    std::map<uint16_t, uint8_t> levels;

    void on_status(ClientStatus s, ErrorCode e, const std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        status = s;
        error = e;
        history.push_back(s);
    }
    void on_event(const ControllerEvent& ev) override {
        std::lock_guard<std::mutex> lk(mu);
        using K = ControllerEvent::Kind;
        switch (ev.kind) {
            case K::Paired: record = ev.pairing; break;
            case K::StateReset: endpoints.clear(); sources.clear(); break;
            case K::StateSynced: ++synced; break;
            case K::Endpoint: endpoints[ev.endpoint.id] = ev.endpoint; break;
            case K::EndpointRemoved: endpoints.erase(ev.id); break;
            case K::Source: sources[ev.source.id] = ev.source; break;
            case K::SourceRemoved: sources.erase(ev.id); break;
            case K::Icon: icons[ev.id] = ev.data; break;
            case K::Meters:
                for (auto& m : ev.meters) levels[m.slot] = m.level;
                break;
            default: break;
        }
    }
    bool failed_with(ErrorCode e) { return status == ClientStatus::Failed && error == e; }
};

// Tests pair many times from 127.0.0.1: lift the per-address flood limit (covered separately).
ServerConfig test_cfg() {
    ServerConfig c;
    c.pair_request_interval_ms = 0;
    c.max_pair_requests_per_minute = 1000;
    return c;
}

struct Rig {
    FakeBackend backend;
    MemoryStore store;
    Observer observer;
    std::unique_ptr<Server> server;
    uint16_t port = 0;

    explicit Rig(ServerConfig cfg = test_cfg(), uint16_t fixed_port = 0) {
        cfg.agent_id = random_array<16>();
        cfg.agent_name = "TEST-PC";
        cfg.loopback_only = true;
        cfg.port = fixed_port;
        server = std::make_unique<Server>(cfg, backend, store, observer);
        std::string err;
        if (server->start(&err)) port = server->port();
    }
};

ClientTiming fast_timing() {
    ClientTiming t;
    t.backoff_min_ms = 50;
    t.backoff_max_ms = 200;
    return t;
}

// Pairs `client` with the rig; returns the pairing record.
std::optional<PairingRecord> pair(Rig& rig, Client& client, Listener& l) {
    const int before = [&] { std::lock_guard<std::mutex> lk(rig.observer.mu); return rig.observer.requests; }();
    client.pair("127.0.0.1", rig.port);
    if (!wait_for(rig.observer.mu, [&] { return rig.observer.requests > before; })) return std::nullopt;
    if (!wait_for(l.mu, [&] { return l.status == ClientStatus::AwaitingCode; })) return std::nullopt;
    std::string code;
    {
        std::lock_guard<std::mutex> lk(rig.observer.mu);
        code = rig.observer.code;
    }
    client.submit_code(code);
    if (!wait_for(l.mu, [&] { return l.status == ClientStatus::Connected && l.synced > 0; })) return std::nullopt;
    std::lock_guard<std::mutex> lk(l.mu);
    return l.record;
}

}  // namespace

TEST(integration_pair_control_and_resume) {
    Rig rig;
    REQUIRE(rig.port != 0);
    Listener l;
    const Id16 cid = random_array<16>();
    std::optional<PairingRecord> rec;
    {
        Client c(cid, "Meu iPhone", l, fast_timing());
        rec = pair(rig, c, l);
        REQUIRE(rec.has_value());
        CHECK(rec->agent_name == "TEST-PC");
        {
            std::lock_guard<std::mutex> lk(l.mu);
            CHECK(l.endpoints.count("E1") == 1 && l.endpoints["E1"].is_default);
            CHECK(l.sources.count("E1|100") == 1 && l.sources["E1|100"].name == "Spotify");
        }
        CHECK(rig.store.find(cid).has_value());

        // Phone -> PC, and the change comes back as a confirmed update.
        c.set_source_volume("E1|100", 2500);
        CHECK(wait_for(l.mu, [&] { return l.sources["E1|100"].volume == 2500; }));
        c.set_endpoint_mute("E1", true);
        CHECK(wait_for(l.mu, [&] { return l.endpoints["E1"].muted; }));
        c.set_source_mute("E1|100", true);
        CHECK(wait_for(l.mu, [&] { return l.sources["E1|100"].muted; }));
        c.set_endpoint_volume("E1", 10000);
        CHECK(wait_for(l.mu, [&] { return l.endpoints["E1"].volume == 10000; }));

        // PC -> phone: a new app starts, then closes.
        SourceInfo chrome;
        chrome.id = "E1|200";
        chrome.endpoint_id = "E1";
        chrome.name = "Google Chrome";
        rig.server->source_upsert(chrome);
        CHECK(wait_for(l.mu, [&] { return l.sources.count("E1|200") == 1; }));
        rig.server->source_removed("E1|200");
        CHECK(wait_for(l.mu, [&] { return l.sources.count("E1|200") == 0; }));

        c.request_icon("icon1");
        CHECK(wait_for(l.mu, [&] { return l.icons.count("icon1") && l.icons["icon1"].size() == 7; }));
        c.request_icon("missing");
        CHECK(wait_for(l.mu, [&] { return l.icons.count("missing") && l.icons["missing"].empty(); }));
    }
    // Reopen the app: resume with the stored record, no code needed.
    Listener l2;
    Client c2(cid, "Meu iPhone", l2, fast_timing());
    c2.resume("127.0.0.1", rig.port, *rec);
    CHECK(wait_for(l2.mu, [&] { return l2.status == ClientStatus::Connected && l2.synced > 0; }));
    {
        std::lock_guard<std::mutex> lk(l2.mu);
        CHECK(l2.sources["E1|100"].volume == 2500);  // state survived on the PC side
    }
    std::lock_guard<std::mutex> lk(rig.observer.mu);
    CHECK(rig.observer.requests == 1);  // resume never shows a code
}

TEST(integration_coalesces_slider_drag) {
    Rig rig;
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    REQUIRE(pair(rig, c, l).has_value());
    int before;
    {
        std::lock_guard<std::mutex> lk(rig.backend.mu);
        before = rig.backend.commands;
    }
    for (int v = 0; v <= 10000; v += 10) c.set_source_volume("E1|100", uint16_t(v));
    CHECK(wait_for(l.mu, [&] { return l.sources["E1|100"].volume == 10000; }));
    std::lock_guard<std::mutex> lk(rig.backend.mu);
    CHECK(rig.backend.commands - before < 1001);  // not every intermediate value reached the PC
}

TEST(integration_wrong_code_then_lockout) {
    ServerConfig cfg;
    cfg.max_failed_pairings = 3;
    cfg.pair_request_interval_ms = 0;  // all attempts come from 127.0.0.1
    Rig rig(cfg);
    for (int attempt = 0; attempt < 3; ++attempt) {
        Listener l;
        Client c(random_array<16>(), "attacker", l, fast_timing());
        c.pair("127.0.0.1", rig.port);
        REQUIRE(wait_for(l.mu, [&] { return l.status == ClientStatus::AwaitingCode; }));
        std::string code;
        {
            std::lock_guard<std::mutex> lk(rig.observer.mu);
            code = rig.observer.code;
        }
        code[0] = code[0] == '9' ? '0' : char(code[0] + 1);  // always wrong
        c.submit_code(code);
        CHECK(wait_for(l.mu, [&] { return l.failed_with(ErrorCode::WrongCode); }));
    }
    Listener l;
    Client c(random_array<16>(), "attacker", l, fast_timing());
    c.pair("127.0.0.1", rig.port);
    CHECK(wait_for(l.mu, [&] { return l.failed_with(ErrorCode::PairingLocked); }));
    CHECK(rig.store.list().empty());
    std::lock_guard<std::mutex> lk(rig.observer.mu);
    CHECK(rig.observer.finished_fail == 3 && rig.observer.finished_ok == 0);
}

TEST(integration_reject_on_pc) {
    Rig rig;
    Listener l;
    Client c(random_array<16>(), "stranger", l, fast_timing());
    c.pair("127.0.0.1", rig.port);
    REQUIRE(wait_for(rig.observer.mu, [&] { return rig.observer.requests == 1; }));
    uint64_t conn;
    {
        std::lock_guard<std::mutex> lk(rig.observer.mu);
        conn = rig.observer.conn;
    }
    rig.server->reject_pairing(conn);
    CHECK(wait_for(l.mu, [&] { return l.failed_with(ErrorCode::PairingRejected); }));
}

TEST(integration_only_one_pairing_at_a_time) {
    Rig rig;
    Listener l1, l2;
    Client c1(random_array<16>(), "a", l1, fast_timing());
    Client c2(random_array<16>(), "b", l2, fast_timing());
    c1.pair("127.0.0.1", rig.port);
    REQUIRE(wait_for(l1.mu, [&] { return l1.status == ClientStatus::AwaitingCode; }));
    c2.pair("127.0.0.1", rig.port);
    CHECK(wait_for(l2.mu, [&] { return l2.failed_with(ErrorCode::Busy); }));
}

TEST(integration_resume_unknown_and_forged_records) {
    Rig rig;
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    PairingRecord fake;
    fake.agent_id = random_array<16>();
    fake.pair_key = random_array<32>();
    c.resume("127.0.0.1", rig.port, fake);
    CHECK(wait_for(l.mu, [&] { return l.failed_with(ErrorCode::NotRecognized); }));

    // Known controller id but wrong key: the controller must refuse the agent.
    Listener l2;
    const Id16 cid = random_array<16>();
    Client real(cid, "phone", l2, fast_timing());
    auto rec = pair(rig, real, l2);
    REQUIRE(rec.has_value());
    real.disconnect();
    PairingRecord forged = *rec;
    forged.pair_key[0] ^= 1;
    Listener l3;
    Client c3(cid, "phone", l3, fast_timing());
    c3.resume("127.0.0.1", rig.port, forged);
    // With a forged key the agent's proof doesn't verify: the phone treats the
    // peer as the wrong PC and keeps retrying (it never deletes anything).
    CHECK(wait_for(l3.mu, [&] { return l3.status == ClientStatus::Reconnecting && l3.error == ErrorCode::WrongAgent; }));
}

TEST(integration_revoke_kicks_and_blocks) {
    Rig rig;
    Listener l;
    const Id16 cid = random_array<16>();
    Client c(cid, "phone", l, fast_timing());
    auto rec = pair(rig, c, l);
    REQUIRE(rec.has_value());
    rig.server->revoke(cid);
    CHECK(wait_for(l.mu, [&] { return l.failed_with(ErrorCode::Revoked); }));
    CHECK(!rig.store.find(cid).has_value());
    Listener l2;
    Client c2(cid, "phone", l2, fast_timing());
    c2.resume("127.0.0.1", rig.port, *rec);
    CHECK(wait_for(l2.mu, [&] { return l2.failed_with(ErrorCode::NotRecognized); }));
}

TEST(integration_broadcast_to_two_controllers) {
    Rig rig;
    Listener la, lb;
    Client a(random_array<16>(), "phone", la, fast_timing());
    Client b(random_array<16>(), "tablet", lb, fast_timing());
    REQUIRE(pair(rig, a, la).has_value());
    REQUIRE(pair(rig, b, lb).has_value());
    CHECK(wait_for(rig.observer.mu, [&] { return rig.observer.connected == 2; }));
    a.set_source_volume("E1|100", 1234);
    CHECK(wait_for(lb.mu, [&] { return lb.sources["E1|100"].volume == 1234; }));
}

TEST(integration_reconnects_after_agent_restart) {
    MemoryStore shared;
    const Id16 cid = random_array<16>();
    Listener l;
    Client c(cid, "phone", l, fast_timing());
    uint16_t port = 0;
    {
        Rig rig;
        port = rig.port;
        auto rec = pair(rig, c, l);
        REQUIRE(rec.has_value());
        for (auto& p : rig.store.list()) shared.put(p);
        rig.server->stop();
        CHECK(wait_for(l.mu, [&] { return l.status == ClientStatus::Reconnecting; }));
    }
    // New agent process on the same port with the same identity and store.
    FakeBackend backend;
    Observer obs;
    ServerConfig cfg;
    cfg.loopback_only = true;
    cfg.port = port;
    cfg.agent_name = "TEST-PC";
    {
        std::lock_guard<std::mutex> lk(l.mu);
        REQUIRE(l.record.has_value());
        cfg.agent_id = l.record->agent_id;
    }
    Server server(cfg, backend, shared, obs);
    std::string err;
    bool started = false;
    for (int i = 0; i < 50 && !started; ++i) {
        started = server.start(&err);
        if (!started) std::this_thread::sleep_for(100ms);
    }
    REQUIRE(started);
    CHECK(wait_for(l.mu, [&] { return l.status == ClientStatus::Connected; }, 8000));
}

TEST(integration_level_meters_subscription) {
    Rig rig;
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    REQUIRE(pair(rig, c, l).has_value());
    uint16_t src_slot = 0, ep_slot = 0;
    {
        std::lock_guard<std::mutex> lk(l.mu);
        src_slot = l.sources["E1|100"].meter;
        ep_slot = l.endpoints["E1"].meter;
    }
    CHECK(src_slot != 0 && ep_slot != 0 && src_slot != ep_slot);
    {
        std::lock_guard<std::mutex> lk(rig.backend.mu);
        CHECK(!rig.backend.metering);  // off until someone subscribes
    }
    c.set_meters(true);
    CHECK(wait_for(rig.backend.mu, [&] { return rig.backend.metering; }));
    rig.server->meters({{"E1|100", 200}, {"E1", 90}, {"unknown", 255}});
    CHECK(wait_for(l.mu, [&] { return l.levels.count(src_slot) && l.levels[src_slot] == 200 && l.levels[ep_slot] == 90; }));
    {
        std::lock_guard<std::mutex> lk(l.mu);
        CHECK(l.levels.size() == 2);  // the unknown id was dropped
    }
    c.set_meters(false);
    CHECK(wait_for(rig.backend.mu, [&] { return !rig.backend.metering; }));
    // Subscription survives a reconnect and stops when the controller leaves.
    c.set_meters(true);
    CHECK(wait_for(rig.backend.mu, [&] { return rig.backend.metering; }));
    c.disconnect();
    CHECK(wait_for(rig.backend.mu, [&] { return !rig.backend.metering; }));
}

TEST(integration_spoofed_agent_cannot_make_phone_forget) {
    // SEC-1: a LAN attacker answers on the PC's address with the PC's (public)
    // agent id but cannot prove it. The phone must fail with WrongAgent and
    // keep trying the real address; nothing reports "revoked".
    Rig real;
    Listener l;
    const Id16 cid = random_array<16>();
    Client c(cid, "phone", l, fast_timing());
    auto rec = pair(real, c, l);
    REQUIRE(rec.has_value());
    c.disconnect();

    FakeBackend fb;
    MemoryStore empty;
    Observer obs;
    ServerConfig cfg;
    cfg.agent_id = rec->agent_id;  // spoof the id advertised over mDNS
    cfg.agent_name = "TEST-PC";
    cfg.loopback_only = true;
    cfg.port = 0;
    Server spoof(cfg, fb, empty, obs);
    std::string err;
    REQUIRE(spoof.start(&err));

    Listener l2;
    Client c2(cid, "phone", l2, fast_timing());
    // Spoofer first (as if mDNS pointed there), the real PC second.
    c2.resume({{"127.0.0.1", spoof.port()}, {"127.0.0.1", real.port}}, *rec);
    CHECK(wait_for(l2.mu, [&] { return l2.status == ClientStatus::Connected; }, 8000));
    std::lock_guard<std::mutex> lk(l2.mu);
    CHECK(l2.error != ErrorCode::Revoked);
}

TEST(integration_plaintext_revoked_is_not_trusted) {
    // A raw peer sends a plaintext ERROR 10 (revoked) as the HELLO_ACK reply.
    net::init();
    uint16_t port = 0;
    std::string err;
    net::socket_t ls = net::listen_tcp(0, true, &port, &err);
    REQUIRE(ls != net::kInvalidSocket);
    std::thread fake([&] {
        std::vector<net::PollItem> it{{ls, true, false}};
        net::poll(it, 3000);
        net::socket_t s = net::accept_tcp(ls, nullptr);
        if (s == net::kInvalidSocket) return;
        uint8_t buf[512];
        std::vector<net::PollItem> r{{s, true, false}};
        net::poll(r, 3000);
        net::recv_some(s, buf, sizeof buf);
        const Bytes f = make_frame(encode_error({ErrorCode::Revoked, "revoked"}));
        net::send_some(s, f.data(), f.size());
        std::this_thread::sleep_for(300ms);
        net::close_socket(s);
    });
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    PairingRecord rec;
    rec.agent_id = random_array<16>();
    rec.pair_key = random_array<32>();
    c.resume("127.0.0.1", port, rec);
    CHECK(wait_for(l.mu, [&] { return l.failed_with(ErrorCode::NotRecognized); }));
    fake.join();
    net::close_socket(ls);
}

TEST(integration_idle_sockets_cannot_starve_controllers) {
    // SEC-2: many silent sockets must not block a real controller.
    ServerConfig cfg = test_cfg();
    cfg.max_sockets = 8;
    cfg.max_pending_handshakes = 4;
    Rig rig(cfg);
    std::vector<net::socket_t> idle;
    for (int i = 0; i < 12; ++i) {
        std::string e;
        idle.push_back(net::connect_tcp("127.0.0.1", rig.port, 2000, &e));
    }
    std::this_thread::sleep_for(200ms);
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    CHECK(pair(rig, c, l).has_value());
    for (auto s : idle) net::close_socket(s);
}

TEST(integration_pairing_requests_are_rate_limited) {
    // SEC-3: one source can't flood the PC with pairing popups.
    Rig rig{ServerConfig{}};
    for (int i = 0; i < 3; ++i) {
        Listener l;
        Client c(random_array<16>(), "spam", l, fast_timing());
        c.pair("127.0.0.1", rig.port);
        wait_for(l.mu, [&] { return l.status == ClientStatus::AwaitingCode || l.status == ClientStatus::Failed; });
        c.disconnect();
        std::this_thread::sleep_for(50ms);
    }
    std::lock_guard<std::mutex> lk(rig.observer.mu);
    CHECK(rig.observer.requests == 1);  // later ones refused as busy within the interval
}

TEST(integration_any_unverified_refusal_moves_to_next_address) {
    // SEC-C1: a spoofer answering with a plaintext ERROR other than 3/8/10
    // (here "unsupported version") must not stop the phone before it tries
    // the real PC at the next address.
    Rig real;
    Listener l;
    const Id16 cid = random_array<16>();
    Client c(cid, "phone", l, fast_timing());
    auto rec = pair(real, c, l);
    REQUIRE(rec.has_value());
    c.disconnect();
    net::init();
    uint16_t port = 0;
    std::string err;
    net::socket_t ls = net::listen_tcp(0, true, &port, &err);
    REQUIRE(ls != net::kInvalidSocket);
    std::atomic<bool> go{true};
    std::thread fake([&] {
        while (go) {
            std::vector<net::PollItem> it{{ls, true, false}};
            if (net::poll(it, 100) <= 0) continue;
            net::socket_t s = net::accept_tcp(ls, nullptr);
            if (s == net::kInvalidSocket) continue;
            const Bytes f = make_frame(encode_error({ErrorCode::UnsupportedVersion, "nope"}));
            net::send_some(s, f.data(), f.size());
            std::this_thread::sleep_for(50ms);
            net::close_socket(s);
        }
    });
    Listener l2;
    Client c2(cid, "phone", l2, fast_timing());
    c2.resume({{"127.0.0.1", port}, {"127.0.0.1", real.port}}, *rec);
    CHECK(wait_for(l2.mu, [&] { return l2.status == ClientStatus::Connected; }, 8000));
    go = false;
    fake.join();
    net::close_socket(ls);
}

TEST(integration_all_refused_reports_not_recognized_over_spoofed_reason) {
    // R-6: when every address refused, a spoofer's chosen refusal ("update the
    // app") must not hide the real PC's "not recognized" (the actionable one).
    Rig real;  // knows nothing about this phone -> plaintext "unknown controller"
    PairingRecord rec;
    rec.agent_id = random_array<16>();
    rec.pair_key = random_array<32>();
    net::init();
    uint16_t port = 0;
    std::string err;
    net::socket_t ls = net::listen_tcp(0, true, &port, &err);
    REQUIRE(ls != net::kInvalidSocket);
    std::atomic<bool> go{true};
    std::thread fake([&] {
        while (go) {
            std::vector<net::PollItem> it{{ls, true, false}};
            if (net::poll(it, 100) <= 0) continue;
            net::socket_t s = net::accept_tcp(ls, nullptr);
            if (s == net::kInvalidSocket) continue;
            const Bytes f = make_frame(encode_error({ErrorCode::UnsupportedVersion, "update"}));
            net::send_some(s, f.data(), f.size());
            std::this_thread::sleep_for(50ms);
            net::close_socket(s);
        }
    });
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    c.resume({{"127.0.0.1", real.port}, {"127.0.0.1", port}}, rec);  // the spoofer answers last
    CHECK(wait_for(l.mu, [&] { return l.status == ClientStatus::Failed; }, 8000));
    {
        std::lock_guard<std::mutex> lk(l.mu);
        CHECK(l.failed_with(ErrorCode::NotRecognized));
    }
    c.disconnect();
    go = false;
    fake.join();
    net::close_socket(ls);
}

TEST(integration_injected_framing_garbage_after_ready_reconnects) {
    // R-2: an on-path attacker injecting a bogus length prefix into a live
    // channel must only cause a reconnect, never a permanent failure.
    Rig real;
    Listener l;
    const Id16 cid = random_array<16>();
    Client c(cid, "phone", l, fast_timing());
    auto rec = pair(real, c, l);
    REQUIRE(rec.has_value());
    c.disconnect();
    net::init();
    uint16_t port = 0;
    std::string err;
    net::socket_t ls = net::listen_tcp(0, true, &port, &err);
    REQUIRE(ls != net::kInvalidSocket);
    std::atomic<bool> go{true};
    std::atomic<int> conns{0};
    std::thread relay([&] {
        while (go) {
            std::vector<net::PollItem> it{{ls, true, false}};
            if (net::poll(it, 100) <= 0) continue;
            net::socket_t a = net::accept_tcp(ls, nullptr);
            if (a == net::kInvalidSocket) continue;
            std::string e;
            net::socket_t b = net::connect_tcp("127.0.0.1", real.port, 1000, &e);
            if (b == net::kInvalidSocket) { net::close_socket(a); continue; }
            const bool inject = conns.fetch_add(1) == 0;
            const auto start = std::chrono::steady_clock::now();
            bool injected = false;
            while (go) {
                std::vector<net::PollItem> p{{a, true, false}, {b, true, false}};
                net::poll(p, 50);
                uint8_t buf[16384];
                bool closed = false;
                for (int i = 0; i < 2 && !closed; ++i) {
                    if (!p[i].readable && !p[i].failed) continue;
                    long n = net::recv_some(p[i].fd, buf, sizeof buf);
                    if (n == net::kWouldBlock) continue;
                    if (n <= 0) { closed = true; break; }
                    net::socket_t to = i == 0 ? b : a;
                    for (long off = 0; off < n;) {
                        long w = net::send_some(to, buf + off, size_t(n - off));
                        if (w == net::kWouldBlock) { std::this_thread::sleep_for(1ms); continue; }
                        if (w <= 0) { closed = true; break; }
                        off += w;
                    }
                }
                if (closed) break;
                if (inject && !injected && std::chrono::steady_clock::now() - start > 600ms) {
                    const uint8_t junk[4] = {0xFF, 0xFF, 0xFF, 0xFF};
                    net::send_some(a, junk, sizeof junk);
                    injected = true;
                }
            }
            net::close_socket(a);
            net::close_socket(b);
        }
    });
    Listener l2;
    Client c2(cid, "phone", l2, fast_timing());
    c2.resume({{"127.0.0.1", port}}, *rec);
    // Connected, broken by the junk, then connected again through a clean link.
    CHECK(wait_for(l2.mu, [&] { return conns.load() >= 2 && l2.status == ClientStatus::Connected; }, 8000));
    {
        std::lock_guard<std::mutex> lk(l2.mu);
        for (ClientStatus st : l2.history) CHECK(st != ClientStatus::Failed);
    }
    c2.disconnect();
    go = false;
    relay.join();
    net::close_socket(ls);
}

TEST(integration_unlock_lifts_lock_but_keeps_escalation) {
    // R-1: the tray "unlock" lets the user pair right away, but an attacker
    // still present must not get the shortest lockout again.
    struct LockObs : Observer {
        int lockouts = -1;
        int64_t until = -1;
        void on_pairing_lockout(int n, int64_t u) override {
            std::lock_guard<std::mutex> lk(mu);
            lockouts = n;
            until = u;
        }
    };
    FakeBackend fb;
    MemoryStore store;
    LockObs obs;
    ServerConfig cfg = test_cfg();
    cfg.agent_id = random_array<16>();
    cfg.loopback_only = true;
    cfg.port = 0;
    cfg.initial_lockouts = 3;
    cfg.locked_until_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count() + 600;
    Server s(cfg, fb, store, obs);
    std::string err;
    REQUIRE(s.start(&err));
    s.reset_pairing_lockout();
    CHECK(wait_for(obs.mu, [&] { return obs.until == 0; }));
    {
        std::lock_guard<std::mutex> lk(obs.mu);
        CHECK(obs.lockouts == 3);
    }
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    c.pair("127.0.0.1", s.port());
    CHECK(wait_for(l.mu, [&] { return l.status == ClientStatus::AwaitingCode; }));
    c.disconnect();
}

TEST(connect_accepts_ipv4_with_interface_suffix) {
    // iOS prints addresses taken from a live path as "192.168.0.5%en0"; the
    // phone gave up with "couldn't reach the PC" without sending a packet.
    net::init();
    uint16_t port = 0;
    std::string err;
    net::socket_t ls = net::listen_tcp(0, true, &port, &err);
    REQUIRE(ls != net::kInvalidSocket);
    for (const char* host : {"127.0.0.1%en0", "127.0.0.1%lo0", "127.0.0.1%1"}) {
        net::socket_t s = net::connect_tcp(host, port, 2000, &err);
        CHECK(s != net::kInvalidSocket);
        if (s == net::kInvalidSocket) std::printf("  %s: %s\n", host, err.c_str());
        else net::close_socket(s);
    }
    net::close_socket(ls);
}

TEST(lan_only_address_filter) {
    CHECK(net::is_local_network_address("192.168.0.10"));
    CHECK(net::is_local_network_address("10.1.2.3"));
    CHECK(net::is_local_network_address("172.20.0.1"));
    CHECK(net::is_local_network_address("::ffff:192.168.1.5"));
    CHECK(net::is_local_network_address("fe80::1%12"));
    CHECK(net::is_local_network_address("fd12:3456::1"));
    CHECK(net::is_local_network_address("127.0.0.1"));
    CHECK(!net::is_local_network_address("8.8.8.8"));
    CHECK(!net::is_local_network_address("172.32.0.1"));
    CHECK(!net::is_local_network_address("2001:4860::1"));
    CHECK(!net::is_local_network_address("::ffff:1.1.1.1"));
}

TEST(integration_failed_handshake_flood_cannot_block_resume) {
    // R2-1: one address opening sockets that each get an ERROR (bad hello)
    // must not keep slots: a legitimate resume still gets in quickly.
    ServerConfig cfg = test_cfg();
    cfg.max_sockets = 12;
    cfg.max_pending_handshakes = 8;
    Rig rig(cfg);
    Listener l;
    const Id16 cid = random_array<16>();
    Client c(cid, "phone", l, fast_timing());
    auto rec = pair(rig, c, l);
    REQUIRE(rec.has_value());
    c.disconnect();
    std::atomic<bool> go{true};
    std::thread flood([&] {
        while (go) {
            std::string e;
            net::socket_t s = net::connect_tcp("127.0.0.1", rig.port, 500, &e);
            if (s == net::kInvalidSocket) continue;
            const uint8_t junk[5] = {0, 0, 0, 1, 0x7F};  // valid frame, unexpected message -> ERROR
            net::send_some(s, junk, sizeof junk);
            std::this_thread::sleep_for(5ms);  // keep the socket open (lingering after the error)
        }
    });
    std::this_thread::sleep_for(300ms);
    Listener l2;
    Client c2(cid, "phone", l2, fast_timing());
    const auto t0 = std::chrono::steady_clock::now();
    c2.resume("127.0.0.1", rig.port, *rec);
    const bool ok = wait_for(l2.mu, [&] { return l2.status == ClientStatus::Connected; }, 3000);
    go = false;
    flood.join();
    CHECK(ok);
    CHECK(std::chrono::steady_clock::now() - t0 < 3s);
}

TEST(integration_not_recognized_needs_every_address) {
    // R2-2: an impostor at one address while the real PC is offline (refused
    // connection) must not stop the phone: it keeps retrying.
    Rig impostor;  // knows nothing about this phone -> plaintext "unknown controller"
    PairingRecord rec;
    rec.agent_id = random_array<16>();
    rec.pair_key = random_array<32>();
    uint16_t dead_port = 0;
    {
        std::string e;
        net::socket_t ls = net::listen_tcp(0, true, &dead_port, &e);  // grab a free port, then close it
        net::close_socket(ls);
    }
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    c.resume({{"127.0.0.1", impostor.port}, {"127.0.0.1", dead_port}}, rec);
    std::this_thread::sleep_for(1500ms);
    std::lock_guard<std::mutex> lk(l.mu);
    CHECK(l.status != ClientStatus::Failed);
}

TEST(integration_lockout_escalation_survives_restart) {
    // SEC-3: the agent reports lockout state and restores it on start.
    struct LockObs : Observer {
        int lockouts = -1;
        int64_t until = 0;
        void on_pairing_lockout(int n, int64_t u) override {
            std::lock_guard<std::mutex> lk(mu);
            lockouts = n;
            until = u;
        }
    };
    FakeBackend fb;
    MemoryStore store;
    LockObs obs;
    ServerConfig cfg = test_cfg();
    cfg.agent_id = random_array<16>();
    cfg.loopback_only = true;
    cfg.port = 0;
    cfg.initial_lockouts = 3;
    cfg.locked_until_unix = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count() + 60;
    Server s(cfg, fb, store, obs);
    std::string err;
    REQUIRE(s.start(&err));
    Listener l;
    Client c(random_array<16>(), "phone", l, fast_timing());
    c.pair("127.0.0.1", s.port());
    CHECK(wait_for(l.mu, [&] { return l.failed_with(ErrorCode::PairingLocked); }));
}
