// Protocol 1.2: microphones, per-app output routing, panic mute, AGENT_INFO.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "archon/client.hpp"
#include "archon/crypto.hpp"
#include "archon/protocol.hpp"
#include "archon/server.hpp"
#include "test.hpp"

using namespace arc;
using namespace std::chrono_literals;

namespace {

class Store : public TrustStore {
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

// Speakers (unmuted), headphones (already muted by the user), a microphone, one app.
class Backend : public AudioBackend {
public:
    std::mutex mu;
    AudioSink* sink = nullptr;
    std::map<std::string, EndpointInfo> eps;
    std::map<std::string, SourceInfo> srcs;
    std::vector<std::pair<std::string, std::string>> routes;
    std::vector<std::pair<std::string, bool>> mutes;

    Backend() {
        eps["OUT1"] = EndpointInfo{"OUT1", "Speakers", 5000, false, true};
        eps["OUT2"] = EndpointInfo{"OUT2", "Headphones", 4000, true, false};
        EndpointInfo mic{"MIC1", "Microphone", 7000, false, false};
        mic.input = true;
        mic.is_default_input = true;
        eps["MIC1"] = mic;
        SourceInfo s;
        s.id = "S1";
        s.endpoint_id = "OUT1";
        s.name = "Player";
        s.volume = 10000;
        srcs[s.id] = s;
    }
    bool start(AudioSink& snk, std::string*) override {
        std::lock_guard<std::mutex> lk(mu);
        sink = &snk;
        for (auto& [id, e] : eps) sink->endpoint_upsert(e);
        for (auto& [id, s] : srcs) sink->source_upsert(s);
        return true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lk(mu);
        sink = nullptr;
    }
    void set_endpoint_volume(const std::string& id, uint16_t v) override {
        std::lock_guard<std::mutex> lk(mu);
        auto it = eps.find(id);
        if (it == eps.end()) return;
        it->second.volume = v;
        if (sink) sink->endpoint_upsert(it->second);
    }
    void set_endpoint_mute(const std::string& id, bool m) override {
        std::lock_guard<std::mutex> lk(mu);
        mutes.emplace_back(id, m);
        auto it = eps.find(id);
        if (it == eps.end()) return;
        it->second.muted = m;
        if (sink) sink->endpoint_upsert(it->second);
    }
    void set_source_volume(const std::string&, uint16_t) override {}
    void set_source_mute(const std::string&, bool) override {}
    bool icon_png(const std::string&, Bytes&) override { return false; }
    void set_source_route(const std::string& id, const std::string& ep) override {
        std::lock_guard<std::mutex> lk(mu);
        routes.emplace_back(id, ep);
        auto it = srcs.find(id);
        if (it == srcs.end()) return;
        it->second.route = ep;
        it->second.endpoint_id = ep.empty() ? "OUT1" : ep;
        if (sink) sink->source_upsert(it->second);
    }
    uint32_t features() const override { return kFeatureInputs | kFeatureRouting; }
    std::string os_name() const override { return "test"; }

    size_t mute_calls() {
        std::lock_guard<std::mutex> lk(mu);
        return mutes.size();
    }
    bool muted(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        return eps[id].muted;
    }
};

class Obs : public ServerObserver {
public:
    std::atomic<int> changes{0};
    std::atomic<bool> panic{false};
    void on_panic_changed(bool a) override {
        panic = a;
        ++changes;
    }
};

class Listener : public ClientListener {
public:
    std::mutex mu;
    ClientStatus status = ClientStatus::Idle;
    AgentInfo info;
    int infos = 0;
    std::map<std::string, EndpointInfo> eps;
    std::map<std::string, SourceInfo> srcs;
    void on_status(ClientStatus s, ErrorCode, const std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        status = s;
    }
    void on_event(const ControllerEvent& ev) override {
        std::lock_guard<std::mutex> lk(mu);
        using K = ControllerEvent::Kind;
        switch (ev.kind) {
            case K::AgentInfo: info = ev.agent; ++infos; break;
            case K::StateReset: eps.clear(); srcs.clear(); break;
            case K::Endpoint: eps[ev.endpoint.id] = ev.endpoint; break;
            case K::Source: srcs[ev.source.id] = ev.source; break;
            default: break;
        }
    }
};

bool eventually(const std::function<bool()>& f, int ms = 3000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (f()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return f();
}

ClientTiming quick() {
    ClientTiming t;
    t.backoff_min_ms = 50;
    t.backoff_max_ms = 200;
    return t;
}

struct Rig {
    Backend be;
    Store store;
    Obs obs;
    ServerConfig cfg;
    std::unique_ptr<Server> srv;
    Id16 cid = random_array<16>();
    PairingRecord rec;

    Rig() {
        cfg.agent_id = random_array<16>();
        cfg.agent_name = "PC";
        cfg.loopback_only = true;
        cfg.port = 0;
        srv = std::make_unique<Server>(cfg, be, store, obs);
        std::string err;
        started = srv->start(&err);
        PairedController pc;
        pc.id = cid;
        pc.name = "phone";
        pc.key = random_array<32>();
        store.put(pc);
        rec.agent_id = cfg.agent_id;
        rec.agent_name = "PC";
        rec.pair_key = pc.key;
    }
    ~Rig() { srv->stop(); }
    bool started = false;
};

bool connected(Listener& l) {
    std::lock_guard<std::mutex> lk(l.mu);
    return l.status == ClientStatus::Connected && l.infos > 0 && l.srcs.count("S1");
}

}  // namespace

TEST(v12_codec_roundtrip_and_backward_compat) {
    EndpointInfo mic{"MIC", "Mic", 1234, true, false};
    mic.input = true;
    mic.is_default_input = true;
    mic.meter = 7;
    Bytes b = encode_endpoint(mic);
    Reader r1(b.data() + 1, b.size() - 1);
    EndpointInfo back;
    CHECK(decode_endpoint(r1, back));
    CHECK(back == mic);

    SourceInfo s;
    s.id = "S";
    s.endpoint_id = "E";
    s.name = "App";
    s.route = "HEADSET";
    b = encode_source(s);
    Reader r2(b.data() + 1, b.size() - 1);
    SourceInfo sb;
    CHECK(decode_source(r2, sb));
    CHECK(sb == s);

    // A 1.1 agent sends no route: same message without the trailing str.
    s.route.clear();
    b = encode_source(s);
    b.resize(b.size() - 2);  // drop the empty str (u16 length 0)
    Reader r3(b.data() + 1, b.size() - 1);
    sb.route = "stale";
    CHECK(decode_source(r3, sb));
    CHECK(sb.route.empty());

    AgentInfo a{kFeatureInputs | kFeaturePanic, true, "windows"};
    b = encode_agent_info(a);
    b.push_back(0x55);  // a later version may append fields
    Reader r4(b.data() + 1, b.size() - 1);
    AgentInfo ab;
    CHECK(decode_agent_info(r4, ab));
    CHECK(ab == a);

    b = encode_set_route("S1", "");
    Reader r5(b.data() + 1, b.size() - 1);
    std::string sid = "x", eid = "x";
    CHECK(decode_set_route(r5, sid, eid));
    CHECK(sid == "S1");
    CHECK(eid.empty());
}

TEST(v12_agent_info_and_microphone_in_dump) {
    Rig rig;
    REQUIRE(rig.started);
    Listener l;
    Client c(rig.cid, "phone", l, quick());
    c.resume("127.0.0.1", rig.srv->port(), rig.rec);
    REQUIRE(eventually([&] { return connected(l); }));
    std::lock_guard<std::mutex> lk(l.mu);
    CHECK(l.info.features == (kFeatureInputs | kFeatureRouting | kFeaturePanic));
    CHECK(!l.info.panic);
    CHECK(l.info.os == "test");
    REQUIRE(l.eps.count("MIC1"));
    CHECK(l.eps["MIC1"].input);
    CHECK(l.eps["MIC1"].is_default_input);
    CHECK(!l.eps["MIC1"].is_default);
    CHECK(!l.eps["OUT1"].input);
}

TEST(v12_route_only_existing_source_to_an_output) {
    Rig rig;
    REQUIRE(rig.started);
    Listener l;
    Client c(rig.cid, "phone", l, quick());
    c.resume("127.0.0.1", rig.srv->port(), rig.rec);
    REQUIRE(eventually([&] { return connected(l); }));
    c.set_source_route("S1", "MIC1");  // an input: refused
    c.set_source_route("S9", "OUT2");  // unknown source: refused
    c.set_source_route("S1", "NOPE");  // unknown endpoint: refused
    c.set_source_route("S1", "OUT2");
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(l.mu);
        return l.srcs["S1"].route == "OUT2" && l.srcs["S1"].endpoint_id == "OUT2";
    }));
    c.set_source_route("S1", "");  // back to the default
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(l.mu);
        return l.srcs["S1"].route.empty() && l.srcs["S1"].endpoint_id == "OUT1";
    }));
    std::lock_guard<std::mutex> lk(rig.be.mu);
    REQUIRE(rig.be.routes.size() == 2);
    CHECK(rig.be.routes[0] == std::make_pair(std::string("S1"), std::string("OUT2")));
    CHECK(rig.be.routes[1] == std::make_pair(std::string("S1"), std::string()));
}

TEST(v12_panic_mutes_only_unmuted_devices_and_restores_those) {
    Rig rig;
    REQUIRE(rig.started);
    Listener l;
    Client c(rig.cid, "phone", l, quick());
    c.resume("127.0.0.1", rig.srv->port(), rig.rec);
    REQUIRE(eventually([&] { return connected(l); }));
    c.panic_mute(true);
    CHECK(eventually([&] { return rig.be.muted("OUT1") && rig.be.muted("MIC1"); }));
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(l.mu);
        return l.info.panic;
    }));
    CHECK(rig.obs.panic.load());
    CHECK(rig.be.mute_calls() == 2);  // OUT2 was muted by the user: untouched

    c.panic_mute(false);
    CHECK(eventually([&] { return !rig.be.muted("OUT1") && !rig.be.muted("MIC1"); }));
    CHECK(rig.be.muted("OUT2"));  // the user's own mute survives
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(l.mu);
        return !l.info.panic;
    }));
    CHECK(rig.be.mute_calls() == 4);
}

TEST(v12_panic_forgets_a_device_unmuted_by_hand) {
    Rig rig;
    REQUIRE(rig.started);
    rig.srv->panic_mute(true);  // local (tray / hotkey): no controller needed
    REQUIRE(eventually([&] { return rig.be.muted("OUT1") && rig.be.muted("MIC1"); }));
    std::this_thread::sleep_for(1200ms);  // past the window that ignores our own echoes
    rig.be.set_endpoint_mute("OUT1", false);  // the user unmutes the speakers on the PC
    std::this_thread::sleep_for(100ms);
    const size_t before = rig.be.mute_calls();
    rig.srv->panic_mute(false);
    CHECK(eventually([&] { return !rig.be.muted("MIC1"); }));
    std::this_thread::sleep_for(100ms);
    CHECK(rig.be.mute_calls() == before + 1);  // only the microphone: OUT1 was forgotten
    CHECK(rig.obs.changes.load() == 2);
}

TEST(v12_panic_requested_before_the_link_is_up_is_sent_once) {
    // A shortcut connects only to deliver this: it must not be dropped like
    // ordinary offline commands, and must not repeat on later reconnects.
    Rig rig;
    REQUIRE(rig.started);
    Listener l;
    Client c(rig.cid, "phone", l, quick());
    c.resume("127.0.0.1", rig.srv->port(), rig.rec);
    c.panic_mute(true);  // the link is not up yet
    CHECK(eventually([&] { return rig.be.muted("OUT1") && rig.be.muted("MIC1"); }));
    CHECK(eventually([&] { return rig.obs.panic.load(); }));
    const int changes = rig.obs.changes.load();
    rig.srv->panic_mute(false);  // undone on the PC
    CHECK(eventually([&] { return !rig.be.muted("OUT1"); }));
    // Force a reconnect: the old request must not come back.
    c.disconnect();
    c.resume("127.0.0.1", rig.srv->port(), rig.rec);
    REQUIRE(eventually([&] { return connected(l); }));
    std::this_thread::sleep_for(200ms);
    CHECK(!rig.be.muted("OUT1"));
    CHECK(rig.obs.changes.load() == changes + 1);
}

namespace {

// Like the real backend: requests are applied later, in order, on one worker
// thread (the WASAPI backend posts them to its COM thread).
class AsyncBackend : public AudioBackend {
public:
    std::mutex mu;
    AudioSink* sink = nullptr;
    std::map<std::string, EndpointInfo> eps;

    AsyncBackend() {
        eps["OUT1"] = EndpointInfo{"OUT1", "Speakers", 5000, false, true};
        EndpointInfo mic{"MIC1", "Microphone", 7000, false, false};
        mic.input = true;
        eps["MIC1"] = mic;
        worker_ = std::thread([this] {
            std::unique_lock<std::mutex> lk(qmu_);
            for (;;) {
                qcv_.wait(lk, [&] { return quit_ || !queue_.empty(); });
                if (queue_.empty()) return;
                auto job = queue_.front();
                queue_.pop_front();
                lk.unlock();
                std::this_thread::sleep_for(40ms);
                user_set_mute(job.first, job.second);
                lk.lock();
            }
        });
    }
    ~AsyncBackend() override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            quit_ = true;
        }
        qcv_.notify_all();
        worker_.join();
    }
    bool start(AudioSink& snk, std::string*) override {
        std::lock_guard<std::mutex> lk(mu);
        sink = &snk;
        for (auto& [id, e] : eps) sink->endpoint_upsert(e);
        return true;
    }
    void stop() override {
        std::lock_guard<std::mutex> lk(mu);
        sink = nullptr;
    }
    void set_endpoint_volume(const std::string&, uint16_t) override {}
    void set_endpoint_mute(const std::string& id, bool m) override {
        {
            std::lock_guard<std::mutex> lk(qmu_);
            queue_.emplace_back(id, m);
        }
        qcv_.notify_one();
    }
    void set_source_volume(const std::string&, uint16_t) override {}
    void set_source_mute(const std::string&, bool) override {}
    bool icon_png(const std::string&, Bytes&) override { return false; }
    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<std::pair<std::string, bool>> queue_;
    bool quit_ = false;
    std::thread worker_;
    // A change made on the PC itself (or the OS applying a request): reported at once.
    void user_set_mute(const std::string& id, bool m) {
        std::lock_guard<std::mutex> lk(mu);
        auto it = eps.find(id);
        if (it == eps.end()) return;
        it->second.muted = m;
        if (sink) sink->endpoint_upsert(it->second);
    }
    void plug(const EndpointInfo& e) {
        std::lock_guard<std::mutex> lk(mu);
        eps[e.id] = e;
        if (sink) sink->endpoint_upsert(e);
    }
    bool muted(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        return eps[id].muted;
    }
};

struct AsyncRig {
    AsyncBackend be;
    Store store;
    Obs obs;
    ServerConfig cfg;
    std::unique_ptr<Server> srv;
    bool started = false;
    AsyncRig() {
        cfg.agent_id = random_array<16>();
        cfg.agent_name = "PC";
        cfg.loopback_only = true;
        cfg.port = 0;
        srv = std::make_unique<Server>(cfg, be, store, obs);
        std::string err;
        started = srv->start(&err);
    }
    ~AsyncRig() { srv->stop(); }
    // The privacy invariant: the panic flag never claims "muted" with a live device.
    bool consistent() {
        const bool p = obs.panic.load();
        const bool all = be.muted("OUT1") && be.muted("MIC1");
        const bool none = !be.muted("OUT1") && !be.muted("MIC1");
        return p ? all : none;
    }
};

}  // namespace

TEST(v12_panic_quick_toggles_stay_consistent_with_async_os) {
    AsyncRig rig;
    REQUIRE(rig.started);
    for (int round = 0; round < 3; ++round) {
        rig.srv->panic_mute(true);
        rig.srv->panic_mute(false);  // right after: the model must stay coherent
        std::this_thread::sleep_for(400ms);
        CHECK(rig.consistent());
        rig.srv->panic_toggle();
        rig.srv->panic_toggle();
        std::this_thread::sleep_for(400ms);
        CHECK(rig.consistent());
    }
    rig.srv->panic_mute(false);
    CHECK(eventually([&] { return !rig.obs.panic.load() && rig.consistent(); }));
}

TEST(v12_panic_keeps_a_mute_requested_just_before_it) {
    AsyncRig rig;
    REQUIRE(rig.started);
    Listener l;
    PairedController pc;
    pc.id = random_array<16>();
    pc.name = "phone";
    pc.key = random_array<32>();
    rig.store.put(pc);
    PairingRecord rec{rig.cfg.agent_id, "PC", pc.key};
    Client c(pc.id, "phone", l, quick());
    c.resume("127.0.0.1", rig.srv->port(), rec);
    REQUIRE(eventually([&] {
        std::lock_guard<std::mutex> lk(l.mu);
        return l.status == ClientStatus::Connected && l.infos > 0;
    }));
    c.set_endpoint_mute("MIC1", true);  // the user mutes the mic...
    std::this_thread::sleep_for(10ms);   // ...and panics before the OS applied it
    rig.srv->panic_mute(true);
    REQUIRE(eventually([&] { return rig.be.muted("OUT1") && rig.be.muted("MIC1"); }));
    std::this_thread::sleep_for(300ms);
    rig.srv->panic_mute(false);
    CHECK(eventually([&] { return !rig.be.muted("OUT1"); }));
    std::this_thread::sleep_for(200ms);
    CHECK(rig.be.muted("MIC1"));  // the user's own mute survives the panic
}

TEST(v12_panic_mutes_a_device_plugged_in_during_it) {
    AsyncRig rig;
    REQUIRE(rig.started);
    rig.srv->panic_mute(true);
    REQUIRE(eventually([&] { return rig.be.muted("OUT1") && rig.be.muted("MIC1"); }));
    EndpointInfo usb{"USB1", "USB headset", 6000, false, false};
    rig.be.plug(usb);
    CHECK(eventually([&] { return rig.be.muted("USB1"); }));
    std::this_thread::sleep_for(300ms);
    rig.srv->panic_mute(false);
    CHECK(eventually([&] { return !rig.be.muted("USB1") && !rig.be.muted("OUT1") && !rig.be.muted("MIC1"); }));
}

TEST(v12_hand_unmute_inside_the_echo_window_is_respected) {
    AsyncRig rig;
    REQUIRE(rig.started);
    rig.srv->panic_mute(true);
    REQUIRE(eventually([&] { return rig.be.muted("MIC1"); }));
    rig.be.user_set_mute("MIC1", false);  // unmuted on the PC right away (inside the 1 s window)
    std::this_thread::sleep_for(1500ms);  // the held-back report is applied when the request expires
    rig.srv->panic_mute(false);
    CHECK(eventually([&] { return !rig.be.muted("OUT1"); }));
    std::this_thread::sleep_for(200ms);
    CHECK(!rig.be.muted("MIC1"));
}
