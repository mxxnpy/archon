// Protocol 1.3: listening to the PC on the phone (control messages, sealed UDP
// datagrams, reordering, receiver, server stream policy, end-to-end delivery).

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "archon/client.hpp"
#include "archon/crypto.hpp"
#include "archon/net.hpp"
#include "archon/protocol.hpp"
#include "archon/server.hpp"
#include "archon/stream.hpp"
#include "test.hpp"

using namespace arc;
using namespace std::chrono_literals;

namespace {

bool eventually(const std::function<bool()>& f, int ms = 3000) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        if (f()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return f();
}

Bytes pkt(uint8_t tag, size_t n = 40) { return Bytes(n, tag); }

struct Got {
    uint64_t seq;
    Bytes packet;
    bool lost;
};

class Collect : public StreamListener {
public:
    std::mutex mu;
    std::vector<Got> got;
    void on_audio(const uint8_t* p, size_t n, uint64_t seq, bool lost) override {
        std::lock_guard<std::mutex> lk(mu);
        got.push_back({seq, Bytes(p, p + n), lost});
    }
    size_t size() {
        std::lock_guard<std::mutex> lk(mu);
        return got.size();
    }
};

// ---- server-side fixtures -------------------------------------------------------------

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

class Backend : public AudioBackend {
public:
    std::mutex mu;
    AudioSink* sink = nullptr;
    std::map<std::string, EndpointInfo> eps;
    Backend() {
        eps["OUT1"] = EndpointInfo{"OUT1", "Speakers", 5000, false, true};
        EndpointInfo mic{"MIC1", "Mic", 7000, false, false};
        mic.input = true;
        eps["MIC1"] = mic;
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
        std::lock_guard<std::mutex> lk(mu);
        auto it = eps.find(id);
        if (it == eps.end()) return;
        it->second.muted = m;
        if (sink) sink->endpoint_upsert(it->second);
    }
    void set_source_volume(const std::string&, uint16_t) override {}
    void set_source_mute(const std::string&, bool) override {}
    bool icon_png(const std::string&, Bytes&) override { return false; }
    bool muted(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        return eps[id].muted;
    }
    void remove(const std::string& id) {
        std::lock_guard<std::mutex> lk(mu);
        eps.erase(id);
        if (sink) sink->endpoint_removed(id);
    }
};

// Stands in for capture + Opus: sends `frames` sealed datagrams to the phone.
class FakeStreams : public StreamService {
public:
    std::mutex mu;
    struct Started {
        uint64_t id;
        std::string endpoint, host;
        uint16_t port;
        Key32 key;
    };
    std::vector<Started> started;
    std::vector<uint64_t> stopped;
    int frames = 5;
    bool fail = false;
    bool start(uint64_t id, const std::string& ep, const std::string& host, uint16_t port, const Key32& key,
               std::string* err) override {
        if (fail) {
            if (err) *err = "no capture";
            return false;
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            started.push_back({id, ep, host, port, key});
        }
        std::thread([host, port, key, n = frames] {
            std::string e;
            net::socket_t s = net::bind_udp(0, nullptr, &e);
            for (int i = 0; i < n; ++i) {
                const Bytes payload = pkt(uint8_t(0x10 + i));
                const Bytes d = seal_audio(key, uint64_t(i), uint32_t(i * 960), kStreamCodecOpus, payload.data(), payload.size());
                net::send_udp(s, host, port, d.data(), d.size());
                std::this_thread::sleep_for(2ms);
            }
            net::close_socket(s);
        }).detach();
        return true;
    }
    void stop(uint64_t id) override {
        std::lock_guard<std::mutex> lk(mu);
        stopped.push_back(id);
    }
    size_t n_started() {
        std::lock_guard<std::mutex> lk(mu);
        return started.size();
    }
    size_t n_stopped() {
        std::lock_guard<std::mutex> lk(mu);
        return stopped.size();
    }
};

class Obs : public ServerObserver {
public:
    std::atomic<size_t> streams{0};
    void on_streams_changed(size_t n) override { streams = n; }
};

class Listener : public ClientListener {
public:
    std::mutex mu;
    ClientStatus status = ClientStatus::Idle;
    AgentInfo info;
    int infos = 0;
    StreamState stream;
    int states = 0;
    std::vector<Got> audio;
    void on_status(ClientStatus s, ErrorCode, const std::string&) override {
        std::lock_guard<std::mutex> lk(mu);
        status = s;
    }
    void on_event(const ControllerEvent& ev) override {
        std::lock_guard<std::mutex> lk(mu);
        if (ev.kind == ControllerEvent::Kind::AgentInfo) {
            info = ev.agent;
            ++infos;
        }
        if (ev.kind == ControllerEvent::Kind::StreamState) {
            stream = ev.stream;
            ++states;
        }
    }
    void on_audio(const uint8_t* p, size_t n, uint64_t seq, bool lost) override {
        std::lock_guard<std::mutex> lk(mu);
        audio.push_back({seq, Bytes(p, p + n), lost});
    }
};

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
    FakeStreams streams;
    ServerConfig cfg;
    std::unique_ptr<Server> srv;
    bool started = false;
    Rig() {
        cfg.agent_id = random_array<16>();
        cfg.agent_name = "PC";
        cfg.loopback_only = true;
        cfg.port = 0;
        srv = std::make_unique<Server>(cfg, be, store, obs);
        srv->set_stream_service(&streams);
        std::string err;
        started = srv->start(&err);
    }
    ~Rig() { srv->stop(); }
    PairingRecord add_phone(const Id16& cid) {
        PairedController pc;
        pc.id = cid;
        pc.name = "phone";
        pc.key = random_array<32>();
        store.put(pc);
        PairingRecord rec;
        rec.agent_id = cfg.agent_id;
        rec.agent_name = "PC";
        rec.pair_key = pc.key;
        return rec;
    }
};

bool live(Listener& l) {
    std::lock_guard<std::mutex> lk(l.mu);
    return l.status == ClientStatus::Connected && l.infos > 0;
}

}  // namespace

TEST(v13_stream_messages_roundtrip) {
    StreamStart a{50123, kStreamFlagMutePc, "OUT1"};
    Bytes b = encode_stream_start(a);
    Reader r1(b.data() + 1, b.size() - 1);
    StreamStart ab;
    CHECK(decode_stream_start(r1, ab));
    CHECK(ab.udp_port == 50123 && ab.flags == kStreamFlagMutePc && ab.endpoint_id == "OUT1");

    StreamState s;
    s.active = true;
    s.codec = kStreamCodecOpus;
    s.sample_rate = 48000;
    s.channels = 2;
    s.frame_ms = 20;
    s.key = random_array<32>();
    b = encode_stream_state(s);
    Reader r2(b.data() + 1, b.size() - 1);
    StreamState sb;
    CHECK(decode_stream_state(r2, sb));
    CHECK(sb.active && sb.codec == 1 && sb.sample_rate == 48000 && sb.channels == 2 && sb.frame_ms == 20);
    CHECK(sb.key == s.key && sb.error == 0);
    b.resize(b.size() - 3);  // truncated key/error: rejected
    Reader r3(b.data() + 1, b.size() - 1);
    CHECK(!decode_stream_state(r3, sb));
}

TEST(v13_audio_datagrams_authenticate) {
    const Key32 key = random_array<32>();
    const Bytes payload = pkt(0x42, 300);
    Bytes d = seal_audio(key, 7, 6720, kStreamCodecOpus, payload.data(), payload.size());
    CHECK(d.size() == kAudioHeader + 16 + payload.size());
    uint64_t seq = 0;
    uint32_t ts = 0;
    uint8_t codec = 0;
    Bytes out;
    CHECK(open_audio(key, d.data(), d.size(), seq, ts, codec, out));
    CHECK(seq == 7 && ts == 6720 && codec == kStreamCodecOpus && out == payload);

    Bytes tampered = d;
    tampered[kAudioHeader + 20] ^= 1;  // ciphertext
    CHECK(!open_audio(key, tampered.data(), tampered.size(), seq, ts, codec, out));
    tampered = d;
    tampered[11] ^= 1;  // seq in the header (associated data)
    CHECK(!open_audio(key, tampered.data(), tampered.size(), seq, ts, codec, out));
    tampered = d;
    tampered[0] = 2;  // unknown version
    CHECK(!open_audio(key, tampered.data(), tampered.size(), seq, ts, codec, out));
    Key32 other = key;
    other[3] ^= 1;
    CHECK(!open_audio(other, d.data(), d.size(), seq, ts, codec, out));
    CHECK(!open_audio(key, d.data(), kAudioHeader + 10, seq, ts, codec, out));
    // Same payload, next seq: different bytes (nonce changes).
    Bytes d2 = seal_audio(key, 8, 6720, kStreamCodecOpus, payload.data(), payload.size());
    CHECK(Bytes(d.begin() + kAudioHeader, d.end()) != Bytes(d2.begin() + kAudioHeader, d2.end()));
}

TEST(v13_reorder_fixes_order_reports_loss_and_drops_late) {
    Reorder r;
    std::vector<std::pair<uint64_t, bool>> out;
    auto sink = [&](uint64_t s, const Bytes&, bool lost) { out.emplace_back(s, lost); };
    for (uint64_t s : {10, 12, 11, 13}) r.push(s, pkt(1), sink);
    REQUIRE(out.size() == 4);
    for (size_t i = 0; i < 4; ++i) CHECK(out[i].first == 10 + i && !out[i].second);
    out.clear();
    r.push(13, pkt(1), sink);  // duplicate
    r.push(5, pkt(1), sink);   // late
    CHECK(out.empty());
    // 14 never arrives: after the window fills it is reported lost, then the rest flows.
    for (uint64_t s : {15, 16, 17, 18}) r.push(s, pkt(1), sink);
    REQUIRE(out.size() == 5);
    CHECK(out[0].first == 14 && out[0].second);
    for (size_t i = 1; i < 5; ++i) CHECK(out[i].first == 14 + i && !out[i].second);
    out.clear();
    r.push(5000, pkt(1), sink);  // sender restarted far away: resync, no flood of losses
    REQUIRE(out.size() == 1);
    CHECK(out[0].first == 5000 && !out[0].second);
}

TEST(v13_receiver_accepts_only_the_pc_and_the_key) {
    Collect c;
    StreamReceiver rx(c);
    std::string err;
    const uint16_t port = rx.open(&err);
    REQUIRE(port != 0);
    const Key32 key = random_array<32>();
    net::socket_t tx = net::bind_udp(0, nullptr, &err);
    REQUIRE(tx != net::kInvalidSocket);
    auto send = [&](const Key32& k, uint64_t seq) {
        const Bytes p = pkt(uint8_t(seq));
        const Bytes d = seal_audio(k, seq, 0, kStreamCodecOpus, p.data(), p.size());
        net::send_udp(tx, "127.0.0.1", port, d.data(), d.size());
    };
    send(key, 0);  // not started yet: ignored
    std::this_thread::sleep_for(50ms);
    rx.start(key, "127.0.0.1");
    for (uint64_t s = 0; s < 4; ++s) send(key, s);
    send(random_array<32>(), 4);  // someone else's key
    CHECK(eventually([&] { return c.size() == 4; }));
    std::this_thread::sleep_for(50ms);
    CHECK(c.size() == 4);
    CHECK(rx.rejected() >= 1);
    rx.start(key, "10.9.9.9");  // only that address is accepted now
    send(key, 0);
    std::this_thread::sleep_for(80ms);
    CHECK(c.size() == 4);
    rx.stop();
    net::close_socket(tx);
}

TEST(v13_end_to_end_audio_reaches_the_app_and_mute_pc_is_undone) {
    Rig rig;
    REQUIRE(rig.started);
    const Id16 cid = random_array<16>();
    const PairingRecord rec = rig.add_phone(cid);
    Listener l;
    Client c(cid, "phone", l, quick());
    c.resume("127.0.0.1", rig.srv->port(), rec);
    REQUIRE(eventually([&] { return live(l); }));
    {
        std::lock_guard<std::mutex> lk(l.mu);
        CHECK(l.info.features & kFeatureStream);
    }
    REQUIRE(c.stream_start(kStreamFlagMutePc));
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(l.mu);
        return l.stream.active && l.audio.size() == 5;
    }));
    {
        std::lock_guard<std::mutex> lk(l.mu);
        for (size_t i = 0; i < l.audio.size(); ++i) {
            CHECK(l.audio[i].seq == i && !l.audio[i].lost);
            CHECK(l.audio[i].packet == pkt(uint8_t(0x10 + i)));
        }
    }
    CHECK(rig.streams.n_started() == 1);
    {
        std::lock_guard<std::mutex> lk(rig.streams.mu);
        CHECK(rig.streams.started[0].endpoint == "OUT1");  // empty request = default output
    }
    CHECK(eventually([&] { return rig.be.muted("OUT1"); }));  // "mute the PC while streaming"
    CHECK(rig.obs.streams.load() == 1);
    c.stream_stop();
    CHECK(eventually([&] { return !rig.be.muted("OUT1") && rig.streams.n_stopped() == 1; }));
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(l.mu);
        return !l.stream.active;
    }));
    CHECK(rig.obs.streams.load() == 0);
}

TEST(v13_stream_stops_when_the_phone_leaves_or_the_output_goes) {
    Rig rig;
    REQUIRE(rig.started);
    const Id16 cid = random_array<16>();
    const PairingRecord rec = rig.add_phone(cid);
    {
        Listener l;
        Client c(cid, "phone", l, quick());
        c.resume("127.0.0.1", rig.srv->port(), rec);
        REQUIRE(eventually([&] { return live(l); }));
        REQUIRE(c.stream_start(kStreamFlagMutePc));
        REQUIRE(eventually([&] { return rig.be.muted("OUT1"); }));
    }  // phone gone (client destroyed)
    CHECK(eventually([&] { return rig.streams.n_stopped() == 1 && !rig.be.muted("OUT1"); }, 5000));

    Listener l2;
    Client c2(cid, "phone", l2, quick());
    c2.resume("127.0.0.1", rig.srv->port(), rec);
    REQUIRE(eventually([&] { return live(l2); }));
    REQUIRE(c2.stream_start(0));
    REQUIRE(eventually([&] {
        std::lock_guard<std::mutex> lk(l2.mu);
        return l2.stream.active;
    }));
    rig.be.remove("OUT1");  // the output disappears (USB headset unplugged)
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(l2.mu);
        return !l2.stream.active && l2.stream.error == uint16_t(ErrorCode::ConnectionLost);
    }));
    CHECK(rig.streams.n_stopped() == 2);
}

TEST(v13_stream_refuses_inputs_capacity_and_failed_capture) {
    Rig rig;
    REQUIRE(rig.started);
    std::vector<std::unique_ptr<Listener>> ls;
    std::vector<std::unique_ptr<Client>> cs;
    for (int i = 0; i < 3; ++i) {
        const Id16 cid = random_array<16>();
        const PairingRecord rec = rig.add_phone(cid);
        ls.push_back(std::make_unique<Listener>());
        cs.push_back(std::make_unique<Client>(cid, "phone", *ls.back(), quick()));
        cs.back()->resume("127.0.0.1", rig.srv->port(), rec);
        REQUIRE(eventually([&] { return live(*ls.back()); }));
    }
    auto state = [&](int i) {
        std::lock_guard<std::mutex> lk(ls[i]->mu);
        return std::make_pair(ls[i]->states, ls[i]->stream);
    };
    REQUIRE(cs[0]->stream_start(0, "MIC1"));  // a microphone is not an output
    CHECK(eventually([&] { return state(0).first == 1; }));
    CHECK(!state(0).second.active && state(0).second.error == uint16_t(ErrorCode::Protocol));
    REQUIRE(cs[0]->stream_start(0));
    REQUIRE(cs[1]->stream_start(0));
    CHECK(eventually([&] { return state(0).second.active && state(1).second.active; }));
    REQUIRE(cs[2]->stream_start(0));  // third stream: over the limit
    CHECK(eventually([&] { return state(2).first == 1; }));
    CHECK(!state(2).second.active && state(2).second.error == uint16_t(ErrorCode::Busy));
    cs[1]->stream_stop();
    CHECK(eventually([&] { return !state(1).second.active; }));
    rig.streams.fail = true;  // capture cannot start
    REQUIRE(cs[2]->stream_start(0));
    // The key goes out before capture starts, so the phone may see "active"
    // first; what matters is that it ends inactive with the reason.
    CHECK(eventually([&] {
        const auto st = state(2);
        return st.first >= 2 && !st.second.active && st.second.error == uint16_t(ErrorCode::Busy);
    }));
}

TEST(v13_datagrams_arriving_before_the_key_are_kept) {
    // The key travels over TCP; the first UDP datagrams can arrive before it.
    Collect c;
    StreamReceiver rx(c);
    std::string err;
    const uint16_t port = rx.open(&err);
    REQUIRE(port != 0);
    const Key32 key = random_array<32>();
    net::socket_t tx = net::bind_udp(0, nullptr, &err);
    REQUIRE(tx != net::kInvalidSocket);
    for (uint64_t s = 0; s < 3; ++s) {
        const Bytes p = pkt(uint8_t(s));
        const Bytes d = seal_audio(key, s, 0, kStreamCodecOpus, p.data(), p.size());
        net::send_udp(tx, "127.0.0.1", port, d.data(), d.size());
    }
    std::this_thread::sleep_for(100ms);  // they arrive with no key yet
    CHECK(c.size() == 0);
    rx.start(key, "127.0.0.1");
    CHECK(eventually([&] { return c.size() == 3; }));
    std::lock_guard<std::mutex> lk(c.mu);
    for (size_t i = 0; i < c.got.size(); ++i) CHECK(c.got[i].seq == i && !c.got[i].lost);
    net::close_socket(tx);
}

// Review 1.3 (SEC-1): an old authenticated datagram must never be played again.
TEST(v13_reorder_never_goes_back_so_replays_are_dropped) {
    Reorder r;
    std::vector<uint64_t> out;
    auto sink = [&](uint64_t s, const Bytes&, bool) { out.push_back(s); };
    for (uint64_t s = 0; s < 300; ++s) r.push(s, pkt(1), sink);
    REQUIRE(out.size() == 300);
    out.clear();
    for (uint64_t s = 10; s < 20; ++s) r.push(s, pkt(1), sink);  // replay of 2 s ago
    r.push(0, pkt(1), sink);
    r.push(150, pkt(1), sink);  // within kResync behind: also old
    CHECK(out.empty());
    r.push(300, pkt(1), sink);  // the live stream carries on untouched
    REQUIRE(out.size() == 1);
    CHECK(out[0] == 300);
}

// SEC-7: once listening stops, nothing that arrives is kept for later.
TEST(v13_receiver_keeps_nothing_after_stop) {
    Collect c;
    StreamReceiver rx(c);
    std::string err;
    const uint16_t port = rx.open(&err);
    REQUIRE(port != 0);
    rx.stop();
    const Key32 key = random_array<32>();
    net::socket_t tx = net::bind_udp(0, nullptr, &err);
    REQUIRE(tx != net::kInvalidSocket);
    const Bytes p = pkt(7);
    const Bytes d = seal_audio(key, 0, 0, kStreamCodecOpus, p.data(), p.size());
    net::send_udp(tx, "127.0.0.1", port, d.data(), d.size());
    CHECK(eventually([&] { return rx.rejected() >= 1; }));
    rx.open(&err);  // a new stream is asked for later with the same key: the old datagram is gone
    rx.start(key, "127.0.0.1");
    std::this_thread::sleep_for(80ms);
    CHECK(c.size() == 0);
    net::close_socket(tx);
}

namespace {
struct Phone {
    Listener l;
    Client c;
    Phone(Rig& rig, const Id16& cid, const PairingRecord& rec) : c(cid, "phone", l, quick()) {
        c.resume("127.0.0.1", rig.srv->port(), rec);
    }
    bool streaming() {
        std::lock_guard<std::mutex> lk(l.mu);
        return l.stream.active;
    }
};
}  // namespace

// SEC-2: ending a stream during a panic must not bring the speakers back.
TEST(v13_panic_keeps_the_output_muted_when_a_stream_ends) {
    Rig rig;
    REQUIRE(rig.started);
    const Id16 cid = random_array<16>();
    Phone a(rig, cid, rig.add_phone(cid));
    REQUIRE(eventually([&] { return live(a.l); }));
    REQUIRE(a.c.stream_start(kStreamFlagMutePc));
    REQUIRE(eventually([&] { return a.streaming() && rig.be.muted("OUT1"); }));
    rig.srv->panic_mute(true);
    CHECK(eventually([&] {
        std::lock_guard<std::mutex> lk(a.l.mu);
        return a.l.info.panic;
    }));
    a.c.stream_stop();
    CHECK(eventually([&] { return !a.streaming() && rig.streams.n_stopped() == 1; }));
    std::this_thread::sleep_for(150ms);
    CHECK(rig.be.muted("OUT1"));  // still muted: it belongs to the panic now
    rig.srv->panic_mute(false);
    CHECK(eventually([&] { return !rig.be.muted("OUT1"); }));  // and comes back with it
}

// SEC-2 (related): two phones asked to mute the PC; it stays muted until the last one stops.
TEST(v13_mute_pc_lasts_until_the_last_stream_ends) {
    Rig rig;
    REQUIRE(rig.started);
    const Id16 ida = random_array<16>(), idb = random_array<16>();
    Phone a(rig, ida, rig.add_phone(ida));
    Phone b(rig, idb, rig.add_phone(idb));
    REQUIRE(eventually([&] { return live(a.l) && live(b.l); }));
    REQUIRE(a.c.stream_start(kStreamFlagMutePc));
    REQUIRE(eventually([&] { return a.streaming() && rig.be.muted("OUT1"); }));
    REQUIRE(b.c.stream_start(kStreamFlagMutePc));
    REQUIRE(eventually([&] { return b.streaming(); }));
    a.c.stream_stop();
    CHECK(eventually([&] { return !a.streaming(); }));
    std::this_thread::sleep_for(150ms);
    CHECK(rig.be.muted("OUT1"));  // B is still listening
    b.c.stream_stop();
    CHECK(eventually([&] { return !b.streaming() && !rig.be.muted("OUT1"); }));
}

// SEC-4: a failure reported by an old capture must not end the stream that replaced it.
TEST(v13_late_failure_of_an_old_stream_is_ignored) {
    Rig rig;
    REQUIRE(rig.started);
    const Id16 cid = random_array<16>();
    Phone a(rig, cid, rig.add_phone(cid));
    REQUIRE(eventually([&] { return live(a.l); }));
    REQUIRE(a.c.stream_start(0));
    REQUIRE(eventually([&] { return a.streaming() && rig.streams.n_started() == 1; }));
    REQUIRE(a.c.stream_start(0));  // restart (e.g. another output)
    REQUIRE(eventually([&] { return rig.streams.n_started() == 2; }));
    uint64_t old_id = 0, new_id = 0;
    {
        std::lock_guard<std::mutex> lk(rig.streams.mu);
        old_id = rig.streams.started[0].id;
        new_id = rig.streams.started[1].id;
    }
    CHECK(old_id != new_id);
    rig.srv->stream_failed(old_id, ErrorCode::ConnectionLost);
    std::this_thread::sleep_for(150ms);
    CHECK(a.streaming());
    CHECK(rig.obs.streams.load() == 1);
    rig.srv->stream_failed(new_id, ErrorCode::ConnectionLost);
    CHECK(eventually([&] { return !a.streaming(); }));
}

// Datagrams kept before the key must play before newer ones that arrive while
// the key is being installed (a race: the newer one used to start the sequence
// and the kept ones were then dropped as late — the first frames went missing).
TEST(v13_kept_datagrams_play_before_newer_ones) {
    for (int round = 0; round < 3; ++round) {
        Collect c;
        StreamReceiver rx(c);
        std::string err;
        const uint16_t port = rx.open(&err);
        REQUIRE(port != 0);
        const Key32 key = random_array<32>();
        std::atomic<bool> go{true};
        std::thread tx([&] {
            std::string e;
            net::socket_t s = net::bind_udp(0, nullptr, &e);
            for (uint64_t seq = 0; seq < 60 && go; ++seq) {
                const Bytes p = pkt(uint8_t(seq));
                const Bytes d = seal_audio(key, seq, 0, kStreamCodecOpus, p.data(), p.size());
                net::send_udp(s, "127.0.0.1", port, d.data(), d.size());
                std::this_thread::sleep_for(1ms);
            }
            net::close_socket(s);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(6 + round));  // the key arrives mid-stream
        rx.start(key, "127.0.0.1");
        tx.join();
        CHECK(eventually([&] { return c.size() >= 55; }));
        std::lock_guard<std::mutex> lk(c.mu);
        REQUIRE(!c.got.empty());
        CHECK(c.got.front().seq == 0);  // nothing from before the key was lost
        for (size_t i = 1; i < c.got.size(); ++i) CHECK(c.got[i].seq == c.got[i - 1].seq + 1);
        rx.close();
    }
}
