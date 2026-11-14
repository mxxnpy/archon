// Adversarial: controller-side Client behaviour (reconnect, command freshness,
// coalescing, meter subscription) and server-side metering lifecycle.

#include "adv_common.hpp"

using namespace adv;

namespace {

bool connected_and_synced(Listener& l, int min_synced = 1, int timeout_ms = 5000) {
    return wait_for(l.mu, [&] { return l.status == ClientStatus::Connected && l.synced >= min_synced; }, timeout_ms);
}

}  // namespace

// Claim: "commands made while disconnected are discarded on reconnect" — the
// fresh state dump is the truth, a stale slider value must not be replayed.
TEST(adv_client_discards_commands_made_while_disconnected) {
    MemoryStore shared;
    const Id16 agent = random_array<16>();
    const Id16 cid = random_array<16>();
    Listener l;
    Client c(cid, "phone", l, fast_timing());
    uint16_t port = 0;
    {
        AdvRig rig1({}, 0, &agent, &shared);
        REQUIRE(rig1.port != 0);
        port = rig1.port;
        const Key32 key = rig1.provision(cid);
        c.resume("127.0.0.1", port, rig1.record_for(key));
        REQUIRE(connected_and_synced(l));
        rig1.server->stop();
        REQUIRE(wait_for(l.mu, [&] { return l.status == ClientStatus::Reconnecting; }));
    }
    // User drags sliders while the PC is unreachable.
    c.set_endpoint_volume("E1", 1234);
    c.set_source_mute("E1|100", true);
    c.request_icon("icon1");
    AdvRig rig2({}, port, &agent, &shared);
    REQUIRE(rig2.port == port);
    REQUIRE(connected_and_synced(l, 1, 8000));
    std::this_thread::sleep_for(300ms);
    std::lock_guard<std::mutex> lk(rig2.backend.mu);
    std::printf("  commands applied after reconnect: %d (expected 0)\n", rig2.backend.commands);
    CHECK(rig2.backend.commands == 0);
    CHECK(rig2.backend.endpoints["E1"].volume == 5000);
}

TEST(adv_client_backoff_is_exponential_and_bounded) {
    // An "agent" that accepts and immediately hangs up: every attempt fails fast.
    std::string err;
    uint16_t port = 0;
    net::socket_t ls = net::listen_tcp(0, true, &port, &err);
    REQUIRE(ls != net::kInvalidSocket);
    std::atomic<bool> go{true};
    std::atomic<int> attempts{0};
    std::thread acceptor([&] {
        while (go) {
            std::vector<net::PollItem> it{{ls, true, false}};
            net::poll(it, 20);
            if (!it[0].readable) continue;
            net::socket_t s = net::accept_tcp(ls, nullptr);
            if (s != net::kInvalidSocket) {
                ++attempts;
                net::close_socket(s);
            }
        }
    });
    Listener l;
    ClientTiming t;
    t.backoff_min_ms = 50;
    t.backoff_max_ms = 400;
    Client c(random_array<16>(), "phone", l, t);
    PairingRecord rec;
    rec.agent_id = random_array<16>();
    rec.pair_key = random_array<32>();
    c.resume("127.0.0.1", port, rec);
    std::this_thread::sleep_for(2500ms);
    const int n = attempts.load();
    ClientStatus st;
    {
        std::lock_guard<std::mutex> lk(l.mu);
        st = l.status;
    }
    c.disconnect();
    go = false;
    acceptor.join();
    net::close_socket(ls);
    std::printf("  attempts in 2.5 s: %d (50->400 ms backoff predicts ~8)\n", n);
    CHECK(n >= 4 && n <= 14);
    CHECK(st == ClientStatus::Reconnecting || st == ClientStatus::Connecting);
}

TEST(adv_client_coalesces_and_keeps_last_value) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    Listener l;
    Client c(cid, "phone", l, fast_timing());
    c.resume("127.0.0.1", rig.port, rig.record_for(key));
    REQUIRE(connected_and_synced(l));
    constexpr int N = 3000;
    for (int i = 1; i <= N; ++i) {
        c.set_endpoint_volume("E1", uint16_t(i % 10000));
        c.set_source_volume("E1|100", uint16_t((N - i) % 10000));
    }
    CHECK(wait_for(rig.backend.mu, [&] {
        return rig.backend.endpoints["E1"].volume == N % 10000 && rig.backend.sources["E1|100"].volume == 0;
    }));
    std::this_thread::sleep_for(100ms);
    std::lock_guard<std::mutex> lk(rig.backend.mu);
    // Informational: with a fast reader coalescing is opportunistic (timing dependent).
    std::printf("  %d slider updates -> %d commands on the PC (fast reader)\n", 2 * N, rig.backend.commands);
    CHECK(rig.backend.endpoints["E1"].volume == N % 10000);
}

// The case coalescing exists for: the link is slower than the slider. A fake
// agent completes the handshake and then stops reading for a while.
TEST(adv_client_coalesces_under_backpressure) {
    std::string err;
    uint16_t port = 0;
    net::socket_t ls = net::listen_tcp(0, true, &port, &err);
    REQUIRE(ls != net::kInvalidSocket);
    int small = 4096;
    setsockopt(ls, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&small), sizeof small);
    const Id16 agent = random_array<16>(), cid = random_array<16>();
    const Key32 key = random_array<32>();
    PairingRecord rec;
    rec.agent_id = agent;
    rec.agent_name = "FAKE";
    rec.pair_key = key;
    Listener l;
    Client c(cid, "phone", l, fast_timing());
    c.resume("127.0.0.1", port, rec);
    RawPeer a;  // the fake agent's side of the link
    REQUIRE(wait_until([&] {
        std::vector<net::PollItem> it{{ls, true, false}};
        net::poll(it, 20);
        if (it[0].readable) a.fd = net::accept_tcp(ls, nullptr);
        return a.fd != net::kInvalidSocket;
    }, 3000));
    Bytes m;
    REQUIRE(a.recv_msg(m, 2000) == Rx::Frame && m[0] == uint8_t(MsgType::Hello));
    Hello h;
    {
        Reader r(m.data() + 1, m.size() - 1);
        REQUIRE(decode_hello(r, h) && h.mode == Mode::Resume);
    }
    const Key32 nonce_s = random_array<32>();
    SessionKeys keys = derive_resume_keys(key, cid, agent, h.nonce, nonce_s, h.name, "FAKE");
    HelloAck ack;
    ack.agent_id = agent;
    ack.name = "FAKE";
    ack.mode = Mode::Resume;
    ack.nonce = nonce_s;
    ack.tag = keys.tag_agent;
    REQUIRE(a.send_frame(encode_hello_ack(ack)));
    REQUIRE(a.recv_msg(m, 2000) == Rx::Frame && m[0] == uint8_t(MsgType::ResumeConfirm));
    a.ch.init(keys.key_a2c, keys.key_c2a);
    a.secure = true;
    a.send_app(encode_simple(MsgType::StateReset));
    a.send_app(encode_endpoint(EndpointInfo{"E1", "Speakers", 5000, false, true, 1}));
    a.send_app(encode_simple(MsgType::StateSynced));
    REQUIRE(connected_and_synced(l));
    // Slider drag while the agent is not reading (busy PC, congested Wi-Fi).
    constexpr int N = 50000;
    for (int i = 1; i <= N; ++i) c.set_endpoint_volume("E1", uint16_t(i % 10001));
    std::this_thread::sleep_for(500ms);
    // Agent catches up: count how many volume commands actually crossed the wire.
    int volume_cmds = 0;
    uint16_t last = 0;
    for (;;) {
        Rx r = a.recv_msg(m, 400);
        if (r != Rx::Frame) break;
        if (m[0] != uint8_t(MsgType::SetEndpointVolume)) continue;
        Reader rd(m.data() + 1, m.size() - 1);
        std::string id;
        uint16_t v = 0;
        if (decode_set_volume(rd, id, v)) {
            ++volume_cmds;
            last = v;
        }
    }
    c.disconnect();
    net::close_socket(ls);
    std::printf("  %d slider updates under backpressure -> %d SET_ENDPOINT_VOLUME sent\n", N, volume_cmds);
    CHECK(last == N % 10001);
    CHECK(volume_cmds <= N / 20);  // "only the latest value is sent": expect a handful, not every step
}

TEST(adv_client_resubscribes_meters_after_agent_restart) {
    MemoryStore shared;
    const Id16 agent = random_array<16>();
    const Id16 cid = random_array<16>();
    Listener l;
    Client c(cid, "phone", l, fast_timing());
    uint16_t port = 0;
    {
        AdvRig rig1({}, 0, &agent, &shared);
        REQUIRE(rig1.port != 0);
        port = rig1.port;
        const Key32 key = rig1.provision(cid);
        c.resume("127.0.0.1", port, rig1.record_for(key));
        REQUIRE(connected_and_synced(l));
        c.set_meters(true);
        REQUIRE(wait_until([&] { return rig1.backend.is_metering(); }, 2000));
        rig1.server->stop();
        REQUIRE(wait_for(l.mu, [&] { return l.status == ClientStatus::Reconnecting; }));
    }
    AdvRig rig2({}, port, &agent, &shared);
    REQUIRE(rig2.port == port);
    REQUIRE(connected_and_synced(l, 1, 8000));
    CHECK(wait_until([&] { return rig2.backend.is_metering(); }, 2000));
    uint16_t slot = 0;
    {
        std::lock_guard<std::mutex> lk(l.mu);
        slot = l.endpoints["E1"].meter;
    }
    // Meters are disposable: one frame may be missed (dropped for a busy link, or
    // sent a moment before the re-subscription lands; seen once under ASan on
    // Linux). The product sends them at ~20 Hz, so the test keeps sending too.
    int ticks = 0;
    CHECK(wait_until([&] {
        if (ticks++ % 25 == 0) rig2.server->meters({{"E1", 123}});  // every ~50 ms
        std::lock_guard<std::mutex> lk(l.mu);
        return l.levels.count(slot) && l.levels[slot] == 123;
    }, 2000));
}

TEST(adv_metering_stops_when_last_subscriber_leaves_abruptly) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 a_id = random_array<16>(), b_id = random_array<16>();
    const Key32 a_key = rig.provision(a_id), b_key = rig.provision(b_id);
    {   // RST from the only subscriber.
        RawPeer a;
        REQUIRE(a.connect(rig.port));
        REQUIRE(a.resume(a_id, a_key, rig.cfg.agent_id));
        REQUIRE(a.send_app(encode_flag(MsgType::SetMeters, true)));
        REQUIRE(wait_until([&] { return rig.backend.is_metering(); }, 2000));
        a.abort_rst();
        CHECK(wait_until([&] { return !rig.backend.is_metering(); }, 2000));
    }
    {   // Two subscribers: one leaving keeps metering; both leaving stops it.
        RawPeer a, b;
        REQUIRE(a.connect(rig.port));
        REQUIRE(a.resume(a_id, a_key, rig.cfg.agent_id));
        REQUIRE(b.connect(rig.port));
        REQUIRE(b.resume(b_id, b_key, rig.cfg.agent_id));
        a.send_app(encode_flag(MsgType::SetMeters, true));
        b.send_app(encode_flag(MsgType::SetMeters, true));
        REQUIRE(wait_until([&] { return rig.backend.is_metering(); }, 2000));
        a.abort_rst();
        std::this_thread::sleep_for(200ms);
        CHECK(rig.backend.is_metering());
        b.close();
        CHECK(wait_until([&] { return !rig.backend.is_metering(); }, 2000));
    }
    {   // Subscriber replaced by its own reconnect (which has not subscribed yet).
        RawPeer a1, a2;
        REQUIRE(a1.connect(rig.port));
        REQUIRE(a1.resume(a_id, a_key, rig.cfg.agent_id));
        a1.send_app(encode_flag(MsgType::SetMeters, true));
        REQUIRE(wait_until([&] { return rig.backend.is_metering(); }, 2000));
        REQUIRE(a2.connect(rig.port));
        REQUIRE(a2.resume(a_id, a_key, rig.cfg.agent_id));
        CHECK(wait_until([&] { return !rig.backend.is_metering(); }, 2000));
    }
    {   // Subscriber revoked.
        RawPeer b;
        REQUIRE(b.connect(rig.port));
        REQUIRE(b.resume(b_id, b_key, rig.cfg.agent_id));
        b.send_app(encode_flag(MsgType::SetMeters, true));
        REQUIRE(wait_until([&] { return rig.backend.is_metering(); }, 2000));
        rig.server->revoke(b_id);
        CHECK(wait_until([&] { return !rig.backend.is_metering(); }, 2000));
    }
}

TEST(adv_client_disconnect_and_destroy_racing_event_flood) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    std::atomic<bool> go{true};
    std::thread flood([&] {
        uint16_t v = 0;
        while (go) {
            rig.server->endpoint_upsert(EndpointInfo{"E1", "Speakers", uint16_t(v++ % 10000), false, true});
            rig.server->meters({{"E1", uint8_t(v)}});
            if ((v & 63) == 0) std::this_thread::sleep_for(1ms);
        }
    });
    int slow = 0;
    for (int i = 0; i < 25; ++i) {
        Listener l;
        auto* c = new Client(cid, "phone", l, fast_timing());
        c->resume("127.0.0.1", rig.port, rig.record_for(key));
        c->set_meters(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(5 + (i * 7) % 40));
        const auto t0 = SClock::now();
        if (i % 2 == 0) {
            c->disconnect();
            delete c;
        } else {  // destroyed from another thread while events are being delivered
            std::thread killer([c] { delete c; });
            killer.join();
        }
        if (ms_since(t0) > 2000) ++slow;
    }
    go = false;
    flood.join();
    CHECK(slow == 0);
    CHECK(wait_until([&] { return rig.observer.connected_now() == 0; }, 3000));
    CHECK(wait_until([&] { return !rig.backend.is_metering(); }, 3000));
}
