// Adversarial: pairing and admission policy (docs/protocol.md §3 "Agent policy").

#include "adv_common.hpp"

using namespace adv;

namespace {

std::string observed_code(AdvRig& rig) {
    std::lock_guard<std::mutex> lk(rig.observer.mu);
    return rig.observer.code;
}

std::string other_code(const std::string& code) {
    std::string w = code;
    w[0] = w[0] == '9' ? '0' : char(w[0] + 1);
    return w;
}

// Opens a pairing and returns the ack (or nullopt).
std::optional<HelloAck> start_pair(AdvRig& rig, RawPeer& p, const Id16& cid) {
    const int before = [&] { std::lock_guard<std::mutex> lk(rig.observer.mu); return rig.observer.requests; }();
    if (!p.connect(rig.port)) return std::nullopt;
    if (!p.send_frame(p.hello_pair(cid))) return std::nullopt;
    Bytes m;
    if (p.recv_msg(m, 2000) != Rx::Frame) return std::nullopt;
    auto ack = decode_ack_msg(m);
    if (!ack) return std::nullopt;
    if (!wait_for(rig.observer.mu, [&] { return rig.observer.requests > before; }, 2000)) return std::nullopt;
    return ack;
}

}  // namespace

TEST(adv_pair_by_hand_positive_control) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    RawPeer p;
    auto ack = start_pair(rig, p, cid);
    REQUIRE(ack.has_value());
    PairMath pm = pair_math(observed_code(rig), *ack, cid);
    REQUIRE(p.send_frame(encode_pair_response(pm.yc, pm.tag)));
    Bytes m;
    REQUIRE(p.recv_msg(m, 2000) == Rx::Frame);
    CHECK(m[0] == uint8_t(MsgType::PairConfirm));
    CHECK(rig.store->find(cid).has_value());
}

TEST(adv_one_attempt_per_code_even_when_pipelined) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    RawPeer p;
    auto ack = start_pair(rig, p, cid);
    REQUIRE(ack.has_value());
    const std::string code = observed_code(rig);
    PairMath wrong = pair_math(other_code(code), *ack, cid);
    PairMath right = pair_math(code, *ack, cid);
    // Both guesses in one TCP write: the second must never be evaluated.
    Bytes burst = make_frame(encode_pair_response(wrong.yc, wrong.tag));
    append_frame(burst, encode_pair_response(right.yc, right.tag));
    REQUIRE(p.send_bytes(burst));
    std::vector<uint8_t> seen;
    auto e = p.wait_error(2000, &seen);
    CHECK(e.has_value() && *e == ErrorCode::WrongCode);
    for (uint8_t t : seen) CHECK(t != uint8_t(MsgType::PairConfirm));
    std::this_thread::sleep_for(100ms);
    CHECK(!rig.store->find(cid).has_value());
    std::lock_guard<std::mutex> lk(rig.observer.mu);
    CHECK(rig.observer.finished_fail == 1 && rig.observer.finished_ok == 0);
}

TEST(adv_lockout_after_n_failures_including_degenerate_share) {
    ServerConfig cfg;
    cfg.max_failed_pairings = 3;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    const Id16 paired = random_array<16>();
    const Key32 paired_key = rig.provision(paired);
    for (int i = 0; i < 3; ++i) {
        const Id16 cid = random_array<16>();
        RawPeer p;
        auto ack = start_pair(rig, p, cid);
        REQUIRE(ack.has_value());
        if (i == 0) {  // all-zero share -> K == 0, must still count as a guess
            REQUIRE(p.send_frame(encode_pair_response(Key32{}, random_array<32>())));
        } else {
            PairMath w = pair_math(other_code(observed_code(rig)), *ack, cid);
            REQUIRE(p.send_frame(encode_pair_response(w.yc, w.tag)));
        }
        auto e = p.wait_error(2000);
        CHECK(e.has_value() && *e == ErrorCode::WrongCode);
    }
    RawPeer locked;
    REQUIRE(locked.connect(rig.port));
    REQUIRE(locked.send_frame(locked.hello_pair(random_array<16>())));
    auto e = locked.wait_error(2000);
    CHECK(e.has_value() && *e == ErrorCode::PairingLocked);
    // Lockout must not lock out already-paired controllers.
    RawPeer ok;
    REQUIRE(ok.connect(rig.port));
    CHECK(ok.resume(paired, paired_key, rig.cfg.agent_id));
}

TEST(adv_reject_mid_handshake_then_correct_code_is_refused) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    RawPeer p;
    auto ack = start_pair(rig, p, cid);
    REQUIRE(ack.has_value());
    uint64_t conn = 0;
    {
        std::lock_guard<std::mutex> lk(rig.observer.mu);
        conn = rig.observer.conn;
    }
    PairMath right = pair_math(observed_code(rig), *ack, cid);
    rig.server->reject_pairing(conn);
    auto e = p.wait_error(2000);
    CHECK(e.has_value() && *e == ErrorCode::PairingRejected);
    p.send_frame(encode_pair_response(right.yc, right.tag));
    std::this_thread::sleep_for(150ms);
    CHECK(!rig.store->find(cid).has_value());
}

TEST(adv_reject_racing_correct_code_is_consistent) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    int paired = 0, rejected = 0, inconsistent = 0;
    for (int rep = 0; rep < 12; ++rep) {
        const Id16 cid = random_array<16>();
        RawPeer p;
        auto ack = start_pair(rig, p, cid);
        REQUIRE(ack.has_value());
        uint64_t conn = 0;
        {
            std::lock_guard<std::mutex> lk(rig.observer.mu);
            conn = rig.observer.conn;
        }
        PairMath right = pair_math(observed_code(rig), *ack, cid);
        std::thread t([&] { rig.server->reject_pairing(conn); });
        p.send_frame(encode_pair_response(right.yc, right.tag));
        t.join();
        std::vector<uint8_t> seen;
        auto e = p.wait_error(400, &seen);  // ERROR 6 arrives in ms; the paired path just idles
        const bool got_confirm = !seen.empty() && seen[0] == uint8_t(MsgType::PairConfirm);
        std::this_thread::sleep_for(20ms);
        const bool stored = rig.store->find(cid).has_value();
        if (got_confirm) ++paired;
        if (e && *e == ErrorCode::PairingRejected) ++rejected;
        // Invariant: stored <=> the controller was told it is paired.
        if (stored != got_confirm) ++inconsistent;
        if (stored) rig.server->revoke(cid);
        std::this_thread::sleep_for(20ms);
    }
    CHECK(inconsistent == 0);
    CHECK(paired + rejected == 12);
}

TEST(adv_revoke_during_resume_handshake_is_honoured) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    RawPeer p;
    REQUIRE(p.connect(rig.port));
    const Key32 nonce_c = random_array<32>();
    auto ack = p.resume_hello(cid, nonce_c);
    REQUIRE(ack.has_value());
    rig.server->revoke(cid);
    REQUIRE(wait_until([&] { return !rig.store->find(cid).has_value(); }, 1000));
    // Read what the agent says before answering (an answer racing the close
    // would trigger a reset that discards the ERROR; see adv_error_frame_survives_pipelined_input).
    auto e = p.wait_error(2000);
    CHECK(e.has_value() && *e == ErrorCode::Revoked);
    SessionKeys k = derive_resume_keys(key, cid, rig.cfg.agent_id, nonce_c, ack->nonce, "raw", ack->name);
    p.send_frame(encode_resume_confirm(k.tag_controller));
    CHECK(p.wait_closed(2000));
    std::this_thread::sleep_for(100ms);
    CHECK(rig.observer.connected_now() == 0);
    std::lock_guard<std::mutex> lk(rig.observer.mu);
    CHECK(rig.observer.max_connected == 0);
}

// Same race seen from a real Client that is mid-drag when the PC revokes it:
// it must end Failed/Revoked (forget the pairing), not reconnect as if the
// network had dropped.
TEST(adv_revoke_while_client_streams_commands_reports_revoked) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    int revoked = 0, other = 0;
    for (int rep = 0; rep < 8; ++rep) {
        const Id16 cid = random_array<16>();
        const Key32 key = rig.provision(cid);
        Listener l;
        Client c(cid, "phone", l, fast_timing());
        c.resume("127.0.0.1", rig.port, rig.record_for(key));
        REQUIRE(wait_for(l.mu, [&] { return l.status == ClientStatus::Connected && l.synced > 0; }));
        std::atomic<bool> go{true};
        std::thread drag([&] {
            uint16_t v = 0;
            while (go) {
                c.set_endpoint_volume("E1", uint16_t(v++ % 10000));
                std::this_thread::sleep_for(std::chrono::microseconds(200));
            }
        });
        std::this_thread::sleep_for(30ms);
        const size_t mark = [&] { std::lock_guard<std::mutex> lk(l.mu); return l.history.size(); }();
        rig.server->revoke(cid);
        std::optional<ErrorCode> reason;
        const bool ended = wait_until([&] { return (reason = l.first_end_reason(mark)).has_value(); }, 3000);
        go = false;
        drag.join();
        const ErrorCode first_err = reason ? *reason : ErrorCode::None;
        if (first_err != ErrorCode::Revoked) std::printf("  rep %d: first reason %s\n", rep, error_code_name(first_err));
        c.disconnect();
        (ended && first_err == ErrorCode::Revoked) ? ++revoked : ++other;
    }
    std::printf("  client saw Revoked %d/8, something else (connection_lost/unknown) %d/8\n", revoked, other);
    CHECK(other == 0);
}

TEST(adv_revoke_racing_resume_confirm_never_leaves_live_link) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    int leaked = 0;
    for (int rep = 0; rep < 20; ++rep) {
        const Id16 cid = random_array<16>();
        const Key32 key = rig.provision(cid);
        RawPeer p;
        REQUIRE(p.connect(rig.port));
        const Key32 nonce_c = random_array<32>();
        auto ack = p.resume_hello(cid, nonce_c);
        REQUIRE(ack.has_value());
        SessionKeys k = derive_resume_keys(key, cid, rig.cfg.agent_id, nonce_c, ack->nonce, "raw", ack->name);
        std::thread t([&] { rig.server->revoke(cid); });
        p.send_frame(encode_resume_confirm(k.tag_controller));
        t.join();
        // Whatever the interleaving, the link must end closed and nothing stays connected.
        if (!p.wait_closed(2500)) ++leaked;
        if (!wait_until([&] { return rig.observer.connected_now() == 0; }, 1000)) ++leaked;
    }
    CHECK(leaked == 0);
}

TEST(adv_reconnect_replaces_old_link) {
    AdvRig rig;
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    RawPeer old_link, new_link;
    REQUIRE(old_link.connect(rig.port));
    REQUIRE(old_link.resume(cid, key, rig.cfg.agent_id));
    REQUIRE(new_link.connect(rig.port));
    REQUIRE(new_link.resume(cid, key, rig.cfg.agent_id));
    CHECK(old_link.wait_closed(2000));
    CHECK(wait_until([&] { return rig.observer.connected_now() == 1; }, 1000));
}

// The old link is half-open (phone switched Wi-Fi). At capacity the reconnect
// must still replace it instead of being refused for up to idle_timeout.
TEST(adv_reconnect_replaces_old_link_at_capacity) {
    ServerConfig cfg;
    cfg.max_controllers = 1;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    const Id16 cid = random_array<16>();
    const Key32 key = rig.provision(cid);
    RawPeer old_link;
    REQUIRE(old_link.connect(rig.port));
    REQUIRE(old_link.resume(cid, key, rig.cfg.agent_id));
    REQUIRE(wait_until([&] { return rig.observer.connected_now() == 1; }, 1000));
    RawPeer new_link;
    REQUIRE(new_link.connect(rig.port));
    const Key32 nonce_c = random_array<32>();
    REQUIRE(new_link.send_frame(new_link.hello_resume(cid, nonce_c)));
    Bytes m;
    REQUIRE(new_link.recv_msg(m, 2000) == Rx::Frame);
    CHECK(m[0] == uint8_t(MsgType::HelloAck));  // observed: ERROR 7 busy
    if (m[0] == uint8_t(MsgType::Error)) {
        Reader rd(m.data() + 1, m.size() - 1);
        ErrorMsg em;
        decode_error(rd, em);
        std::printf("  observed ERROR %u '%s'\n", unsigned(em.code), em.message.c_str());
    }
}

// Admission is checked at HELLO, the slot is taken at confirm: parallel
// handshakes can all pass the check.
TEST(adv_max_controllers_holds_under_parallel_handshakes) {
    ServerConfig cfg;
    cfg.max_controllers = 2;
    AdvRig rig(cfg);
    REQUIRE(rig.port != 0);
    constexpr int N = 6;
    std::vector<std::unique_ptr<RawPeer>> peers;
    std::vector<Key32> keys, nonces;
    std::vector<Id16> ids;
    std::vector<std::optional<HelloAck>> acks;
    for (int i = 0; i < N; ++i) {
        ids.push_back(random_array<16>());
        keys.push_back(rig.provision(ids.back()));
        nonces.push_back(random_array<32>());
        peers.push_back(std::make_unique<RawPeer>());
        REQUIRE(peers.back()->connect(rig.port));
    }
    for (int i = 0; i < N; ++i) acks.push_back(peers[size_t(i)]->resume_hello(ids[size_t(i)], nonces[size_t(i)]));
    int acked = 0;
    for (int i = 0; i < N; ++i) {
        if (!acks[size_t(i)]) continue;
        ++acked;
        SessionKeys k = derive_resume_keys(keys[size_t(i)], ids[size_t(i)], rig.cfg.agent_id, nonces[size_t(i)],
                                           acks[size_t(i)]->nonce, "raw", acks[size_t(i)]->name);
        peers[size_t(i)]->send_frame(encode_resume_confirm(k.tag_controller));
    }
    std::this_thread::sleep_for(400ms);
    size_t max_seen = 0;
    {
        std::lock_guard<std::mutex> lk(rig.observer.mu);
        max_seen = rig.observer.max_connected;
    }
    std::printf("  handshakes acked=%d, max simultaneous controllers=%zu (limit 2)\n", acked, max_seen);
    CHECK(max_seen <= 2);
}
