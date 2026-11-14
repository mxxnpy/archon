// Adversarial: hostile peers against a real Server over loopback.
// Hypothesis under attack: the server never crashes or hangs, never lets an
// unauthenticated peer see state or change volume, and enforces framing,
// buffer and timeout limits (docs/protocol.md §2, §3).

#include <cstring>

#include "adv_common.hpp"

using namespace adv;

namespace {

// Liveness probe: a fresh, legitimate peer still completes a resume and gets a PONG.
bool server_still_serves(AdvRig& rig) {
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid, "probe");
    RawPeer p;
    if (!p.connect(rig.port)) return false;
    if (!p.resume(cid, key, rig.cfg.agent_id)) return false;
    p.send_app(encode_seq(MsgType::Ping, 77));
    Bytes m;
    return p.wait_type(MsgType::Pong, 2000, &m);
}

}  // namespace

// Contract §3: HELLO starts with magic "ARCN". A controller written from the
// spec (ESP32, third-party tool) must be accepted.
TEST(adv_contract_hello_magic_is_ARCN) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    RawPeer p;
    REQUIRE(p.connect(rig.port));
    Writer w;  // built from the spec text, not from encode_hello
    w.u8(0x01).raw(reinterpret_cast<const uint8_t*>("ARCN"), 4).u16(1).u8(1);
    w.arr(random_array<16>()).str("spec-controller", 64);
    REQUIRE(p.send_frame(w.take()));
    Bytes m;
    const Rx r = p.recv_msg(m, 2000);
    REQUIRE(r == Rx::Frame);
    CHECK(m[0] == uint8_t(MsgType::HelloAck));  // observed: ERROR 1 "bad hello"
    // And the reference encoder must emit the spec magic.
    Hello h;
    const Bytes enc = encode_hello(h);
    CHECK(enc.size() > 5 && std::memcmp(enc.data() + 1, "ARCN", 4) == 0);
}

TEST(adv_zero_length_frame_closes_link_server_survives) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    RawPeer p;
    REQUIRE(p.connect(rig.port));
    const uint8_t zero[4] = {0, 0, 0, 0};
    REQUIRE(p.send_all(zero, 4));
    CHECK(p.wait_closed(2000));
    CHECK(server_still_serves(rig));
}

TEST(adv_oversized_frames_rejected_at_exact_boundary) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    {   // 65537: protocol error, link closed, no reply needed.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        const uint8_t hdr[4] = {0x00, 0x01, 0x00, 0x01};
        REQUIRE(p.send_all(hdr, 4));
        CHECK(p.wait_closed(2000));
    }
    {   // 0xFFFFFFFF: must not allocate / wait for 4 GiB.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        const uint8_t hdr[4] = {0xFF, 0xFF, 0xFF, 0xFF};
        REQUIRE(p.send_all(hdr, 4));
        CHECK(p.wait_closed(2000));
    }
    {   // Exactly 65536 is a legal frame: it is parsed (garbage type -> ERROR 1).
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        Bytes payload(65536, 0xEE);
        REQUIRE(p.send_frame(payload));
        auto e = p.wait_error(2000);
        CHECK(e.has_value() && *e == ErrorCode::Protocol);
    }
    CHECK(server_still_serves(rig));
}

TEST(adv_http_probe_and_garbage_before_hello) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    {
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        const char* http = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
        REQUIRE(p.send_all(reinterpret_cast<const uint8_t*>(http), std::strlen(http)));
        CHECK(p.wait_closed(2000));
    }
    {   // Well-framed garbage: unknown type during the handshake -> ERROR 1.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.send_frame(Bytes{0x26, 0, 0, 0, 1}));  // PING before HELLO
        auto e = p.wait_error(2000);
        CHECK(e.has_value() && *e == ErrorCode::Protocol);
        CHECK(p.wait_closed(2000));
    }
    CHECK(server_still_serves(rig));
}

TEST(adv_garbage_after_hello_releases_pairing_slot) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    RawPeer a;
    REQUIRE(a.connect(rig.port));
    REQUIRE(a.send_frame(a.hello_pair(random_array<16>())));
    REQUIRE(a.wait_type(MsgType::HelloAck, 2000));
    REQUIRE(a.send_frame(Bytes{0x99, 1, 2, 3}));
    auto e = a.wait_error(2000);
    CHECK(e.has_value() && *e == ErrorCode::Protocol);
    CHECK(wait_for(rig.observer.mu, [&] { return rig.observer.finished_fail == 1; }, 2000));
    // The slot is free again: a new pairing is accepted (not ERROR 7).
    RawPeer b;
    REQUIRE(b.connect(rig.port));
    REQUIRE(b.send_frame(b.hello_pair(random_array<16>())));
    Bytes m;
    REQUIRE(b.recv_msg(m, 2000) == Rx::Frame);
    CHECK(m[0] == uint8_t(MsgType::HelloAck));
}

TEST(adv_trickled_byte_by_byte_full_resume) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    RawPeer p;
    REQUIRE(p.connect(rig.port));
    const Key32 nonce_c = random_array<32>();
    const Bytes hello = make_frame(p.hello_resume(cid, nonce_c));
    for (uint8_t b : hello) {
        REQUIRE(p.send_all(&b, 1));
        std::this_thread::sleep_for(1ms);
    }
    Bytes m;
    REQUIRE(p.recv_msg(m, 2000) == Rx::Frame);
    auto ack = decode_ack_msg(m);
    REQUIRE(ack.has_value());
    SessionKeys keys = derive_resume_keys(key, cid, rig.cfg.agent_id, nonce_c, ack->nonce, "raw", ack->name);
    REQUIRE(ct_equal(keys.tag_agent, ack->tag));
    const Bytes confirm = make_frame(encode_resume_confirm(keys.tag_controller));
    for (uint8_t b : confirm) {
        REQUIRE(p.send_all(&b, 1));
        std::this_thread::sleep_for(1ms);
    }
    p.ch.init(keys.key_c2a, keys.key_a2c);
    p.secure = true;
    REQUIRE(p.wait_type(MsgType::StateSynced, 2000));
    const Bytes ping = make_frame(p.ch.seal(encode_seq(MsgType::Ping, 5)));
    for (uint8_t b : ping) REQUIRE(p.send_all(&b, 1));
    CHECK(p.wait_type(MsgType::Pong, 2000));
}

TEST(adv_trickle_cannot_extend_handshake_deadline) {
    ServerConfig cfg;
    cfg.handshake_timeout_ms = 600;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    RawPeer p;
    const auto t0 = SClock::now();
    REQUIRE(p.connect(rig.port));
    const Bytes hello = make_frame(p.hello_resume(random_array<16>(), random_array<32>()));
    // ~70 bytes at 1 byte / 50 ms would take 3.5 s; the deadline is 0.6 s.
    std::optional<ErrorCode> e;
    bool ended = false;
    const char* how = "still open";
    for (size_t i = 0; i < hello.size() - 1 && !ended; ++i) {
        if (!p.send_all(&hello[i], 1)) {
            ended = true;
            how = "send failed (link reset)";
            break;
        }
        Bytes m;
        Rx r = p.recv_msg(m, 50);
        if (r == Rx::Frame && m[0] == uint8_t(MsgType::Error)) {
            Reader rd(m.data() + 1, m.size() - 1);
            ErrorMsg em;
            if (decode_error(rd, em)) e = em.code;
            ended = true;
            how = "ERROR frame";
        } else if (r == Rx::Closed) {
            ended = true;
            how = "closed/reset before any ERROR was read";
        }
    }
    const auto elapsed = ms_since(t0);
    std::printf("  trickle ended after %lld ms: %s\n", static_cast<long long>(elapsed), how);
    // The hypothesis under attack: data arriving byte by byte must not extend the deadline.
    CHECK(ended);
    CHECK(elapsed < 2000);
}

// While the peer keeps the server busy with pipelined input, the agent's
// "ERROR then close" (protocol §3) must still deliver the ERROR: otherwise the
// controller cannot tell timeout/revoked from a network drop.
TEST(adv_error_frame_survives_pipelined_input) {
    ServerConfig cfg;
    cfg.handshake_timeout_ms = 300;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    int delivered = 0, lost = 0;
    for (int rep = 0; rep < 10; ++rep) {
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        // A HELLO split in two: the first half now, then one byte every 5 ms.
        const Bytes hello = make_frame(p.hello_resume(random_array<16>(), random_array<32>()));
        p.send_all(hello.data(), 10);
        std::optional<ErrorCode> e;
        bool done = false;
        for (size_t i = 10; i < hello.size() - 1 && !done; ++i) {
            p.send_all(&hello[i], 1);  // may fail once the server is gone
            Bytes m;
            Rx r = p.recv_msg(m, 5);
            if (r == Rx::Frame && m[0] == uint8_t(MsgType::Error)) {
                Reader rd(m.data() + 1, m.size() - 1);
                ErrorMsg em;
                if (decode_error(rd, em)) e = em.code;
                done = true;
            } else if (r == Rx::Closed) {
                done = true;
            }
        }
        if (!done) {
            Bytes m;
            if (p.recv_msg(m, 1000) == Rx::Frame && m[0] == uint8_t(MsgType::Error)) e = ErrorCode::Timeout;
        }
        (e && *e == ErrorCode::Timeout) ? ++delivered : ++lost;
    }
    std::printf("  ERROR 9 delivered %d/10, lost to connection reset %d/10\n", delivered, lost);
    CHECK(lost == 0);
}

TEST(adv_silent_peer_and_stalled_handshakes_time_out) {
    ServerConfig cfg;
    cfg.handshake_timeout_ms = 400;
    cfg.code_timeout_ms = 700;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    {   // Says nothing at all.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        auto e = p.wait_error(2000);
        CHECK(e.has_value() && *e == ErrorCode::Timeout);
    }
    {   // Resume stalled after HELLO_ACK.
        const Id16 cid = random_array<16>();
        rig.provision(cid);
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.resume_hello(cid, random_array<32>()).has_value());
        auto e = p.wait_error(2000);
        CHECK(e.has_value() && *e == ErrorCode::Timeout);
    }
    {   // Pairing stalled waiting for the code; slot must be released afterwards.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.send_frame(p.hello_pair(random_array<16>())));
        REQUIRE(p.wait_type(MsgType::HelloAck, 2000));
        auto e = p.wait_error(3000);
        CHECK(e.has_value() && *e == ErrorCode::Timeout);
        RawPeer q;
        REQUIRE(q.connect(rig.port));
        REQUIRE(q.send_frame(q.hello_pair(random_array<16>())));
        Bytes m;
        REQUIRE(q.recv_msg(m, 2000) == Rx::Frame);
        CHECK(m[0] == uint8_t(MsgType::HelloAck));
    }
}

TEST(adv_half_open_and_rst_peers_are_reaped) {
    ServerConfig cfg;
    cfg.idle_timeout_ms = 700;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    {   // Half-close before HELLO.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        p.half_close();
        CHECK(p.wait_closed(2000));
    }
    {   // Authenticated peer vanishes silently (no FIN): idle timeout must reap it.
        const Id16 cid = random_array<16>();
        const Key32 key = rig.provision(cid);
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.resume(cid, key, rig.cfg.agent_id));
        CHECK(wait_until([&] { return rig.observer.connected_now() == 1; }, 2000));
        const auto t0 = SClock::now();
        CHECK(wait_until([&] { return rig.observer.connected_now() == 0; }, 3000));
        CHECK(ms_since(t0) < 2500);
    }
    {   // RST mid-session.
        const Id16 cid = random_array<16>();
        const Key32 key = rig.provision(cid);
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.resume(cid, key, rig.cfg.agent_id));
        CHECK(wait_until([&] { return rig.observer.connected_now() == 1; }, 2000));
        p.abort_rst();
        CHECK(wait_until([&] { return rig.observer.connected_now() == 0; }, 1500));
    }
    CHECK(server_still_serves(rig));
}

TEST(adv_socket_flood_beyond_max_sockets) {
    ServerConfig cfg;
    cfg.max_sockets = 4;
    cfg.handshake_timeout_ms = 3000;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    // One legitimate controller is connected before the flood.
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    RawPeer good;
    REQUIRE(good.connect(rig.port));
    REQUIRE(good.resume(cid, key, rig.cfg.agent_id));
    std::vector<std::unique_ptr<RawPeer>> flood;
    for (int i = 0; i < 24; ++i) {
        auto p = std::make_unique<RawPeer>();
        if (p->connect(rig.port)) flood.push_back(std::move(p));
    }
    // Sockets beyond the cap are closed by the server, not left hanging.
    int closed = 0;
    for (auto& p : flood)
        if (p->wait_closed(150)) ++closed;
    CHECK(closed >= 24 - 3);
    // The already-authenticated controller keeps working during the flood.
    REQUIRE(good.send_app(encode_seq(MsgType::Ping, 1)));
    CHECK(good.wait_type(MsgType::Pong, 2000));
    flood.clear();
    // After the flood a newcomer gets in.
    const Id16 cid2 = random_array<16>();
    const Key32 key2 = rig.provision(cid2);
    bool joined = false;
    for (int i = 0; i < 20 && !joined; ++i) {
        RawPeer p;
        if (p.connect(rig.port) && p.resume(cid2, key2, rig.cfg.agent_id)) joined = true;
        else std::this_thread::sleep_for(50ms);
    }
    CHECK(joined);
}

TEST(adv_unauthenticated_peers_get_no_state_and_change_nothing) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 known = random_array<16>();
    rig.provision(known);
    std::atomic<bool> go{true};
    std::thread noise([&] {  // state changes are broadcast during the attack
        int v = 0;
        while (go) {
            rig.server->endpoint_upsert(EndpointInfo{"E1", "Speakers", uint16_t(v++ % 10000), false, true});
            std::this_thread::sleep_for(1ms);
        }
    });
    std::vector<uint8_t> seen_a, seen_b, seen_c;
    {   // A: pairing peer that skips the code and talks app protocol (plain and fake-sealed).
        RawPeer a;
        REQUIRE(a.connect(rig.port));
        REQUIRE(a.send_frame(a.hello_pair(random_array<16>())));
        Bytes set = encode_set_volume(MsgType::SetEndpointVolume, "E1", 1);
        Bytes fake(16, 0);
        fake.insert(fake.end(), set.begin(), set.end());
        a.send_frame(set);
        a.send_frame(fake);
        auto e = a.wait_error(2000, &seen_a);
        CHECK(e.has_value() && *e == ErrorCode::Protocol);
    }
    {   // B: resume with a sniffed controller id but no key.
        RawPeer b;
        REQUIRE(b.connect(rig.port));
        REQUIRE(b.resume_hello(known, random_array<32>()).has_value());
        b.send_frame(encode_resume_confirm(random_array<32>()));
        b.send_frame(encode_set_volume(MsgType::SetEndpointVolume, "E1", 2));
        auto e = b.wait_error(2000, &seen_b);
        CHECK(e.has_value() && *e == ErrorCode::AuthFailed);
    }
    {   // C: resume HELLO then GET_STATE in the clear before confirming.
        RawPeer c;
        REQUIRE(c.connect(rig.port));
        REQUIRE(c.resume_hello(known, random_array<32>()).has_value());
        c.send_frame(encode_simple(MsgType::GetState));
        auto e = c.wait_error(2000, &seen_c);
        CHECK(e.has_value() && *e == ErrorCode::Protocol);
    }
    go = false;
    noise.join();
    std::this_thread::sleep_for(100ms);
    {
        std::lock_guard<std::mutex> lk(rig.backend.mu);
        CHECK(rig.backend.commands == 0);
    }
    for (auto* seen : {&seen_a, &seen_b, &seen_c})
        for (uint8_t t : *seen) CHECK(t == uint8_t(MsgType::HelloAck) || t == uint8_t(MsgType::Error));
    CHECK(rig.observer.connected_now() == 0);
}

TEST(adv_never_reading_peer_hits_output_cap_and_is_dropped) {
    ServerConfig cfg;
    cfg.max_output_buffer = 256 * 1024;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    for (int i = 0; i < 200; ++i) {
        SourceInfo s;
        s.id = "S" + std::to_string(i);
        s.endpoint_id = "E1";
        s.name = std::string(250, 'n');
        s.icon_key = std::string(100, 'k');
        rig.server->source_upsert(s);
    }
    const Id16 hog_id = random_array<16>(), good_id = random_array<16>();
    const Key32 hog_key = rig.provision(hog_id), good_key = rig.provision(good_id);
    RawPeer hog, good;
    REQUIRE(hog.connect(rig.port));
    hog.small_rcvbuf(4096);
    REQUIRE(hog.resume(hog_id, hog_key, rig.cfg.agent_id));
    REQUIRE(good.connect(rig.port));
    REQUIRE(good.resume(good_id, good_key, rig.cfg.agent_id));
    REQUIRE(wait_until([&] { return rig.observer.connected_now() == 2; }, 2000));
    // The hog asks for the full dump over and over and never reads.
    const auto t0 = SClock::now();
    for (int i = 0; i < 400; ++i)
        if (!hog.send_app(encode_simple(MsgType::GetState))) break;
    CHECK(wait_until([&] { return rig.observer.connected_now() == 1; }, 5000));
    CHECK(ms_since(t0) < 5000);
    CHECK(rig.observer.logged("not reading"));
    // The well-behaved controller was not starved.
    REQUIRE(good.send_app(encode_seq(MsgType::Ping, 9)));
    CHECK(good.wait_type(MsgType::Pong, 3000));
}

TEST(adv_ready_peer_replay_reorder_tamper_close_link_without_effect) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    {   // Replay of a captured frame inside the same session.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.resume(cid, key, rig.cfg.agent_id));
        const Bytes f = p.ch.seal(encode_set_volume(MsgType::SetEndpointVolume, "E1", 1111));
        REQUIRE(p.send_frame(f));
        REQUIRE(p.send_frame(f));
        CHECK(p.wait_closed(2000));
        CHECK(rig.backend.command_count() == 1);
    }
    {   // Reordered frames.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.resume(cid, key, rig.cfg.agent_id));
        const Bytes f1 = p.ch.seal(encode_set_volume(MsgType::SetEndpointVolume, "E1", 2222));
        const Bytes f2 = p.ch.seal(encode_set_volume(MsgType::SetEndpointVolume, "E1", 3333));
        REQUIRE(p.send_frame(f2));
        p.send_frame(f1);
        CHECK(p.wait_closed(2000));
        CHECK(rig.backend.command_count() == 1);
    }
    {   // Tampered byte in the ciphertext.
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        REQUIRE(p.resume(cid, key, rig.cfg.agent_id));
        Bytes f = p.ch.seal(encode_set_volume(MsgType::SetEndpointVolume, "E1", 4444));
        f[f.size() - 1] ^= 0x01;
        REQUIRE(p.send_frame(f));
        CHECK(p.wait_closed(2000));
        CHECK(rig.backend.command_count() == 1);
    }
    {   // A frame captured in an old session replayed in a new one.
        RawPeer p1;
        REQUIRE(p1.connect(rig.port));
        REQUIRE(p1.resume(cid, key, rig.cfg.agent_id));
        const Bytes old = p1.ch.seal(encode_set_volume(MsgType::SetEndpointVolume, "E1", 5555));
        p1.close();
        RawPeer p2;
        REQUIRE(p2.connect(rig.port));
        REQUIRE(p2.resume(cid, key, rig.cfg.agent_id));
        REQUIRE(p2.send_frame(old));
        CHECK(p2.wait_closed(2000));
        CHECK(rig.backend.command_count() == 1);
    }
}

TEST(adv_resume_confirm_replay_rejected) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    const Key32 nonce_c = random_array<32>();
    Key32 captured{};
    {
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        auto ack = p.resume_hello(cid, nonce_c);
        REQUIRE(ack.has_value());
        SessionKeys k = derive_resume_keys(key, cid, rig.cfg.agent_id, nonce_c, ack->nonce, "raw", ack->name);
        captured = k.tag_controller;
        REQUIRE(p.send_frame(encode_resume_confirm(captured)));
    }
    // Attacker without the key replays HELLO (same nonce_c) and the captured tag.
    RawPeer x;
    REQUIRE(x.connect(rig.port));
    REQUIRE(x.resume_hello(cid, nonce_c).has_value());
    REQUIRE(x.send_frame(encode_resume_confirm(captured)));
    auto e = x.wait_error(2000);
    CHECK(e.has_value() && *e == ErrorCode::AuthFailed);
}
