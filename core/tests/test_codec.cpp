#include "archon/frame.hpp"
#include "archon/protocol.hpp"
#include "test.hpp"

using namespace arc;

static Reader body(const Bytes& b) { return Reader(b.data() + 1, b.size() - 1); }

TEST(utf8_sanitize_replaces_invalid_and_strips_nul) {
    CHECK(sanitize_utf8("Música") == "Música");
    CHECK(sanitize_utf8(std::string("a\0b", 3)) == "ab");
    CHECK(sanitize_utf8("\xC3") == "\xEF\xBF\xBD");             // truncated sequence
    CHECK(sanitize_utf8("\xC0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD");  // overlong NUL
    CHECK(sanitize_utf8("\xED\xA0\x80").find("\xED") == std::string::npos);  // surrogate
}

TEST(writer_truncates_on_utf8_boundary) {
    Writer w;
    w.str("ééé", 5);  // 6 bytes; must cut to 4, not split the third é
    Reader r(w.data().data(), w.data().size());
    CHECK(r.str(100) == "éé");
}

TEST(hello_roundtrip_pair_and_resume) {
    Hello h;
    h.mode = Mode::Resume;
    h.controller_id[3] = 7;
    h.name = "Meu iPhone";
    h.nonce[31] = 9;
    Bytes b = encode_hello(h);
    CHECK(b[0] == uint8_t(MsgType::Hello));
    Reader r = body(b);
    Hello d;
    REQUIRE(decode_hello(r, d));
    CHECK(d.mode == Mode::Resume && d.controller_id == h.controller_id && d.name == h.name && d.nonce == h.nonce);

    h.mode = Mode::Pair;
    b = encode_hello(h);
    Reader r2 = body(b);
    REQUIRE(decode_hello(r2, d));
    CHECK(d.mode == Mode::Pair);

    b.push_back(0);  // trailing byte -> reject
    Reader r3 = body(b);
    CHECK(!decode_hello(r3, d));
}

TEST(hello_ack_roundtrip) {
    HelloAck a;
    a.agent_id[0] = 1;
    a.name = "DESKTOP";
    a.mode = Mode::Pair;
    a.sid[5] = 5;
    a.y[6] = 6;
    Bytes b = encode_hello_ack(a);
    Reader r = body(b);
    HelloAck d;
    REQUIRE(decode_hello_ack(r, d));
    CHECK(d.sid == a.sid && d.y == a.y && d.name == "DESKTOP" && d.agent_id == a.agent_id);
}

TEST(model_messages_roundtrip) {
    EndpointInfo e{"{0.0.0}.{abc}", "Alto-falantes", 7350, true, true};
    Bytes b = encode_endpoint(e);
    Reader r = body(b);
    EndpointInfo de;
    REQUIRE(decode_endpoint(r, de));
    CHECK(de == e);

    SourceInfo s;
    s.id = "ep|1234";
    s.endpoint_id = e.id;
    s.name = "Spotify";
    s.icon_key = "k1";
    s.pid = 1234;
    s.volume = 10000;
    s.playing = true;
    b = encode_source(s);
    Reader r2 = body(b);
    SourceInfo ds;
    REQUIRE(decode_source(r2, ds));
    CHECK(ds == s);
}

TEST(meters_roundtrip_and_legacy_source_without_slot) {
    std::vector<MeterLevel> in = {{1, 0}, {2, 128}, {65535, 255}};
    Bytes b = encode_meters(in);
    Reader r = body(b);
    std::vector<MeterLevel> out;
    REQUIRE(decode_meters(r, out));
    REQUIRE(out.size() == 3);
    CHECK(out[2].slot == 65535 && out[2].level == 255 && out[1].level == 128);

    // A 1.0 agent sends SOURCE without the trailing meter slot (nor the 1.2
    // route): still decodes.
    SourceInfo s;
    s.id = "x";
    s.meter = 7;
    b = encode_source(s);
    b.resize(b.size() - 4);  // empty route str (2) + meter (2)
    Reader r2 = body(b);
    SourceInfo d;
    REQUIRE(decode_source(r2, d));
    CHECK(d.meter == 0 && d.id == "x" && d.route.empty());
}

TEST(volume_is_clamped) {
    Bytes b = encode_set_volume(MsgType::SetSourceVolume, "x", 60000);
    Reader r = body(b);
    std::string id;
    uint16_t v = 0;
    REQUIRE(decode_set_volume(r, id, v));
    CHECK(v == kMaxVolume && id == "x");
}

TEST(icon_roundtrip_and_limit) {
    Bytes png(100, 0xAB);
    Bytes b = encode_icon("k", png.data(), png.size());
    Reader r = body(b);
    std::string key;
    Bytes out;
    REQUIRE(decode_icon(r, key, out));
    CHECK(key == "k" && out == png);

    Bytes big(kMaxIconBytes + 1, 1);
    b = encode_icon("k", big.data(), big.size());
    Reader r2 = body(b);
    REQUIRE(decode_icon(r2, key, out));
    CHECK(out.empty());
}

TEST(pairing_record_roundtrip) {
    PairingRecord p;
    p.agent_id[1] = 1;
    p.pair_key[2] = 2;
    p.agent_name = "PC";
    Bytes b = serialize_pairing(p);
    auto d = deserialize_pairing(b.data(), b.size());
    REQUIRE(d.has_value());
    CHECK(d->agent_id == p.agent_id && d->pair_key == p.pair_key && d->agent_name == "PC");
    b[4] = 2;  // unknown version
    CHECK(!deserialize_pairing(b.data(), b.size()).has_value());
}

TEST(frame_assembler_handles_split_and_rejects_oversize) {
    Bytes stream = make_frame(Bytes{1, 2, 3});
    append_frame(stream, Bytes{4});
    FrameAssembler a;
    Bytes out;
    for (uint8_t byte : stream) a.feed(&byte, 1);
    REQUIRE(a.next(out) == FrameAssembler::Result::Frame);
    CHECK((out == Bytes{1, 2, 3}));
    REQUIRE(a.next(out) == FrameAssembler::Result::Frame);
    CHECK((out == Bytes{4}));
    CHECK(a.next(out) == FrameAssembler::Result::NeedMore);

    FrameAssembler bad;
    const uint8_t huge[4] = {0x00, 0x01, 0x00, 0x01};  // 65537
    bad.feed(huge, 4);
    CHECK(bad.next(out) == FrameAssembler::Result::Error);
    FrameAssembler zero;
    const uint8_t z[4] = {0, 0, 0, 0};
    zero.feed(z, 4);
    CHECK(zero.next(out) == FrameAssembler::Result::Error);
}
