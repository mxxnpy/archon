// Adversarial: decoders on every truncation / extension of valid messages
// (each candidate copied into an exact-size heap block so ASan/page-heap can
// see any over-read), and SecureChannel against replay, reorder, reflection,
// tamper and truncation.

#include <cstring>
#include <functional>
#include <memory>

#include "adv_common.hpp"
#include "archon/controller_session.hpp"

using namespace adv;

namespace {

using Decoder = std::function<bool(Reader&)>;

struct Case {
    const char* name;
    Bytes body;       // message body (after the type byte)
    Decoder decode;
    bool tolerant;    // Endpoint/Source/AgentInfo: trailing bytes allowed (later versions append)
    size_t optional_tail = 0;  // bytes at the end older agents don't send (meter 1.1, route 1.2, group+detail 1.4, battery 1.6)
};

bool run_exact(const Bytes& b, const Decoder& d) {
    std::unique_ptr<uint8_t[]> block(new uint8_t[b.size() ? b.size() : 1]);
    if (!b.empty()) std::memcpy(block.get(), b.data(), b.size());
    Reader r(block.get(), b.size());
    return d(r);
}

Bytes body_of(Bytes msg) { return Bytes(msg.begin() + 1, msg.end()); }

std::vector<Case> cases() {
    std::vector<Case> v;
    Hello hp;
    hp.mode = Mode::Pair;
    hp.controller_id = random_array<16>();
    hp.name = "ctrl-é";
    Hello hr = hp;
    hr.mode = Mode::Resume;
    hr.nonce = random_array<32>();
    v.push_back({"hello_pair", body_of(encode_hello(hp)), [](Reader& r) { Hello h; return decode_hello(r, h); }, false});
    v.push_back({"hello_resume", body_of(encode_hello(hr)), [](Reader& r) { Hello h; return decode_hello(r, h); }, false});
    HelloAck ap;
    ap.name = "PC";
    HelloAck ar = ap;
    ar.mode = Mode::Resume;
    v.push_back({"hello_ack_pair", body_of(encode_hello_ack(ap)), [](Reader& r) { HelloAck a; return decode_hello_ack(r, a); }, false});
    v.push_back({"hello_ack_resume", body_of(encode_hello_ack(ar)), [](Reader& r) { HelloAck a; return decode_hello_ack(r, a); }, false});
    v.push_back({"pair_response", body_of(encode_pair_response(Key32{}, Key32{})),
                 [](Reader& r) { Key32 a, b; return decode_two_keys(r, a, b); }, false});
    v.push_back({"resume_confirm", body_of(encode_resume_confirm(Key32{})), [](Reader& r) { Key32 k; return decode_key(r, k); }, false});
    v.push_back({"error", body_of(encode_error(ErrorMsg{ErrorCode::Busy, "busy now"})),
                 [](Reader& r) { ErrorMsg e; return decode_error(r, e); }, false});
    v.push_back({"set_volume", body_of(encode_set_volume(MsgType::SetEndpointVolume, "{0.0.0}", 77)),
                 [](Reader& r) { std::string id; uint16_t vol; return decode_set_volume(r, id, vol); }, false});
    v.push_back({"set_mute", body_of(encode_set_mute(MsgType::SetSourceMute, "E1|9", true)),
                 [](Reader& r) { std::string id; bool m; return decode_set_mute(r, id, m); }, false});
    v.push_back({"id", body_of(encode_id(MsgType::SourceRemoved, "E1|9")),
                 [](Reader& r) { std::string id; return decode_id(r, id, kMaxId); }, false});
    v.push_back({"seq", body_of(encode_seq(MsgType::Pong, 42)), [](Reader& r) { uint32_t s; return decode_seq(r, s); }, false});
    v.push_back({"flag", body_of(encode_flag(MsgType::SetMeters, true)), [](Reader& r) { bool on; return decode_flag(r, on); }, false});
    const Bytes png(300, 0xAB);
    v.push_back({"icon", body_of(encode_icon("k", png.data(), png.size())),
                 [](Reader& r) { std::string k; Bytes p; return decode_icon(r, k, p); }, false});
    EndpointInfo e{"E", "Speakers", 100, true, false, 3};
    v.push_back({"endpoint", body_of(encode_endpoint(e)), [](Reader& r) { EndpointInfo x; return decode_endpoint(r, x); }, true, 3});
    SourceInfo s;
    s.id = "S";
    s.endpoint_id = "E";
    s.name = "App";
    s.icon_key = "ik";
    s.meter = 9;
    v.push_back({"source", body_of(encode_source(s)), [](Reader& r) { SourceInfo x; return decode_source(r, x); }, true, 8});
    AgentInfo ai{kFeatureInputs | kFeatureRouting, true, "windows"};
    v.push_back({"agent_info", body_of(encode_agent_info(ai)), [](Reader& r) { AgentInfo x; return decode_agent_info(r, x); }, true});
    v.push_back({"set_route", body_of(encode_set_route("S1", "E2")),
                 [](Reader& r) { std::string a, b; return decode_set_route(r, a, b); }, false});
    return v;
}

}  // namespace

TEST(adv_decoders_reject_every_truncation_and_strict_ones_reject_extension) {
    int bad = 0;
    for (const Case& c : cases()) {
        REQUIRE(run_exact(c.body, c.decode));
        // Older agents omit the optional tail (u16 meter, then str route): those
        // shorter forms may decode; any other truncation must be rejected.
        const size_t min_ok = c.body.size() - c.optional_tail;
        for (size_t len = 0; len < min_ok; ++len) {
            Bytes cut(c.body.begin(), c.body.begin() + std::ptrdiff_t(len));
            if (run_exact(cut, c.decode)) {
                ++bad;
                std::printf("  %s accepted a %zu/%zu-byte truncation\n", c.name, len, c.body.size());
            }
        }
        Bytes longer = c.body;
        longer.push_back(0x00);
        if (!c.tolerant && run_exact(longer, c.decode)) {
            ++bad;
            std::printf("  %s accepted a trailing byte\n", c.name);
        }
    }
    CHECK(bad == 0);
}

TEST(adv_decoders_length_fields_lie) {
    // String length larger than the remaining bytes / larger than the field limit.
    {
        Writer w;
        w.u16(600).raw(reinterpret_cast<const uint8_t*>("abc"), 3).u16(5);
        Bytes b = w.take();
        CHECK(!run_exact(b, [](Reader& r) { std::string id; uint16_t v; return decode_set_volume(r, id, v); }));
    }
    {
        Writer w;  // 513-byte id: above kMaxId even though the bytes are present
        w.str(std::string(513, 'a'), 65535).u16(5);
        Bytes b = w.take();
        CHECK(!run_exact(b, [](Reader& r) { std::string id; uint16_t v; return decode_set_volume(r, id, v); }));
    }
    {   // METERS count says 255, one entry present.
        Bytes b{255, 0, 1, 7};
        CHECK(!run_exact(b, [](Reader& r) { std::vector<MeterLevel> m; return decode_meters(r, m); }));
    }
    {   // ICON length 48 KiB + 1 and length > remaining.
        Writer w1;
        w1.str("k", 128).u32(uint32_t(kMaxIconBytes + 1));
        Bytes b1 = w1.take();
        b1.resize(b1.size() + kMaxIconBytes + 1, 0);
        CHECK(!run_exact(b1, [](Reader& r) { std::string k; Bytes p; return decode_icon(r, k, p); }));
        Writer w2;
        w2.str("k", 128).u32(1000).u8(1);
        CHECK(!run_exact(w2.take(), [](Reader& r) { std::string k; Bytes p; return decode_icon(r, k, p); }));
    }
    {   // HELLO with an unknown mode / wrong version / bad magic.
        Hello h;
        Bytes b = body_of(encode_hello(h));
        Bytes bad_mode = b;
        bad_mode[6] = 7;
        CHECK(!run_exact(bad_mode, [](Reader& r) { Hello x; return decode_hello(r, x); }));
        Bytes bad_magic = b;
        bad_magic[0] ^= 0xFF;
        CHECK(!run_exact(bad_magic, [](Reader& r) { Hello x; return decode_hello(r, x); }));
    }
    {   // Pairing record: wrong version byte and trailing junk.
        PairingRecord p;
        p.agent_name = "PC";
        Bytes rec = serialize_pairing(p);
        Bytes v2 = rec;
        v2[4] = 2;
        CHECK(!deserialize_pairing(v2.data(), v2.size()).has_value());
        rec.push_back(0);
        CHECK(!deserialize_pairing(rec.data(), rec.size()).has_value());
        for (size_t len = 0; len + 1 < rec.size(); ++len) CHECK(!deserialize_pairing(rec.data(), len).has_value());
    }
}

TEST(adv_secure_channel_replay_reorder_reflection_tamper) {
    const Key32 k_c2a = random_array<32>(), k_a2c = random_array<32>();
    SecureChannel agent, ctrl;
    agent.init(k_a2c, k_c2a);
    ctrl.init(k_c2a, k_a2c);
    const Bytes m1 = encode_set_volume(MsgType::SetEndpointVolume, "E1", 1);
    const Bytes m2 = encode_set_volume(MsgType::SetEndpointVolume, "E1", 2);
    const Bytes f1 = ctrl.seal(m1), f2 = ctrl.seal(m2);
    Bytes out;
    {   // Reflection: the agent's own frame sent back to it.
        SecureChannel a2, c2;
        a2.init(k_a2c, k_c2a);
        c2.init(k_c2a, k_a2c);
        const Bytes own = a2.seal(m1);
        CHECK(!a2.open(own.data(), own.size(), out));
    }
    CHECK(!agent.open(f2.data(), f2.size(), out));  // reorder
    CHECK(agent.open(f1.data(), f1.size(), out) && out == m1);
    CHECK(!agent.open(f1.data(), f1.size(), out));  // replay
    CHECK(agent.open(f2.data(), f2.size(), out) && out == m2);
    // Tamper: every bit of a fresh frame, each against a fresh receiver at ctr 0.
    SecureChannel s0;
    s0.init(k_c2a, k_a2c);
    const Bytes f = s0.seal(m1);
    int accepted = 0;
    for (size_t i = 0; i < f.size(); ++i) {
        for (int bit = 0; bit < 8; ++bit) {
            SecureChannel r0;
            r0.init(k_a2c, k_c2a);
            Bytes t = f;
            t[i] ^= uint8_t(1 << bit);
            if (r0.open(t.data(), t.size(), out)) ++accepted;
        }
    }
    CHECK(accepted == 0);
    // Truncations, including the bare 16-byte MAC and nothing.
    for (size_t len = 0; len < f.size(); ++len) {
        SecureChannel r0;
        r0.init(k_a2c, k_c2a);
        CHECK(!r0.open(f.data(), len, out));
    }
}

TEST(adv_controller_session_rejects_forged_agent_frames) {
    // Resume against an agent that answers with a forged tag or a mismatched agent id.
    PairingRecord rec;
    rec.agent_id = random_array<16>();
    rec.pair_key = random_array<32>();
    const Id16 cid = random_array<16>();
    {
        ControllerSession s(cid, "c");
        s.begin_resume(rec);
        HelloAck a;
        a.mode = Mode::Resume;
        a.agent_id = rec.agent_id;
        a.nonce = random_array<32>();
        a.tag = random_array<32>();
        Bytes m = encode_hello_ack(a);
        ControllerSession::Output o;
        s.on_frame(m.data(), m.size(), o);
        CHECK(o.close && s.state() != ControllerSession::State::Ready);
    }
    {
        ControllerSession s(cid, "c");
        s.begin_resume(rec);
        HelloAck a;
        a.mode = Mode::Pair;  // downgrade attempt: agent answers a resume with a pairing ack
        a.agent_id = rec.agent_id;
        Bytes m = encode_hello_ack(a);
        ControllerSession::Output o;
        s.on_frame(m.data(), m.size(), o);
        CHECK(o.close && s.state() != ControllerSession::State::Ready);
    }
    {   // Encrypted-looking frame before the channel exists.
        ControllerSession s(cid, "c");
        s.begin_pair();
        Bytes junk(40, 0x41);
        ControllerSession::Output o;
        s.on_frame(junk.data(), junk.size(), o);
        CHECK(o.close);
        CHECK(s.app(Bytes{0x20}).empty());
    }
}
